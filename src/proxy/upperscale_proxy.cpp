// upperscale_proxy.cpp — drop-in replacement for amd_fidelityfx_loader_dx12.dll
//
// What it does:
//   * Exports the exact 5 symbols AMD's loader exports (ffxConfigure, ffxCreateContext,
//     ffxDestroyContext, ffxDispatch, ffxQuery) + one internal helper (upperscaleGetState).
//   * In ACTIVE mode (config enabled): walks the pNext desc chain on create/query, finds the
//     FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12 node and SWAPS its ID3D12Device* for a
//     device we created BY LUID on the user-selected GPU B. All FFX compute then runs on B.
//   * In PASSTHROUGH mode (default): forwards everything untouched to AMD's real loader,
//     which we load from a bundled copy (upperscale_real_loader.dll next to us) or an explicit
//     path. This is the safe first in-game test: proves drop-in replacement works without
//     changing where FFX runs.
//   * Keeps per-context bookkeeping (original game device vs swapped device) for the upcoming
//     cross-GPU dispatch transfer (task 5).
//
// Config (env vars, read at DllMain; ini fallback upperscale.ini in CWD):
//   UPPERSCALE_ENABLE=1            enable active mode (device swap)
//   UPPERSCALE_GPU_LUID=<hex>      low part of GPU B LUID (e.g. 0x27214)
//   UPPERSCALE_GPU_LUID_HI=<hex>   high part (default 0)
//   UPPERSCALE_REAL_LOADER=<path>  path to AMD's real loader DLL (default: upperscale_real_loader.dll next to us)
//   UPPERSCALE_LOG=<0|1|2>         0=off, 1=create/destroy only (default), 2+=verbose per-call
//
// NOTE (task 5 pending): in ACTIVE mode ffxDispatch is NOT yet wired for cross-GPU transfer.
// Calling it would record GPU-A resources on a GPU-B-bound context (undefined behavior).
// The proxy therefore logs and returns FFX_API_RETURN_ERROR instead of forwarding. Use
// PASSTHROUGH mode for real-game testing until dispatch wiring lands.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <combaseapi.h>
#include <stdint.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

// ---- FFX API (ABI from AMD's public headers; we only need the 5 entry points + desc layout) ----
typedef void* ffxContext;
typedef uint32_t ffxReturnCode_t;
#define FFX_API_RETURN_OK                     0u
#define FFX_API_RETURN_ERROR                  1u
#define FFX_API_RETURN_ERROR_PARAMETER        6u

typedef struct ffxApiHeader {
    uint64_t type;
    struct ffxApiHeader* pNext;
} ffxApiHeader;

// FFX_API_MAKE_BACKEND_SUB_ID(FFX_API_BACKEND_ID_DX12=0, 0x02) == 0x02
#define UPPERSCALE_DESC_TYPE_BACKEND_DX12     0x00000002ull
struct upperscaleBackendDX12Desc {
    ffxApiHeader header;
    ID3D12Device* device;   // offset 8 — matches AMD's ffxCreateBackendDX12Desc exactly
};

typedef ffxReturnCode_t (*PfnFfxCreateContext)(ffxContext*, ffxApiHeader*, const void*);
typedef ffxReturnCode_t (*PfnFfxDestroyContext)(ffxContext*, const void*);
typedef ffxReturnCode_t (*PfnFfxConfigure)(ffxContext*, const ffxApiHeader*);
typedef ffxReturnCode_t (*PfnFfxQuery)(ffxContext*, ffxApiHeader*);
typedef ffxReturnCode_t (*PfnFfxDispatch)(ffxContext*, const ffxApiHeader*);

// ---- State ----
struct Config {
    int      enable;        // 1 = active (swap device), 0 = passthrough
    ULONG    luidHi, luidLo;
    char     realLoaderPath[512];
    int      logLevel;
};

static Config          g_cfg{};
static CRITICAL_SECTION g_cs;
static int             g_csInit = 0;
static HINSTANCE       g_hInst = nullptr;
static HMODULE         g_realLoader = nullptr;   // AMD's real loader (bundled copy)
static PfnFfxCreateContext    g_pRealCreate = nullptr;
static PfnFfxDestroyContext   g_pRealDestroy = nullptr;
static PfnFfxConfigure        g_pRealConfigure = nullptr;
static PfnFfxQuery            g_pRealQuery = nullptr;
static PfnFfxDispatch         g_pRealDispatch = nullptr;
static ID3D12Device*     g_gpuBDev = nullptr;    // our LUID-selected device (lazy)
static char              g_logPath[512]{};
static FILE*             g_log = nullptr;
static int               g_swapsDone = 0;

// context bookkeeping for task 5: ffxContext -> original game device (GPU A)
struct CtxInfo { ffxContext ctx; ID3D12Device* origDev; };
#define MAX_TRACKED_CTX 64
static CtxInfo g_ctxTable[MAX_TRACKED_CTX];      // small fixed table, no heap in render path

// ---- Logging ----
static void Log(const char* fmt, ...) {
    if (!g_log || g_cfg.logLevel <= 0) return;
    va_list ap; va_start(ap, fmt);
    fprintf(g_log, "[upperscale] ");
    vfprintf(g_log, fmt, ap);
    fputc('\n', g_log);
    fflush(g_log);
    va_end(ap);
}

static void LogAlways(const char* fmt, ...) {
    if (!g_log) return;   // level 0 = fully off
    va_list ap; va_start(ap, fmt);
    fprintf(g_log, "[upperscale] ");
    vfprintf(g_log, fmt, ap);
    fputc('\n', g_log);
    fflush(g_log);
    va_end(ap);
}

// ---- Config parsing ----
static void ParseHexEnv(const char* name, ULONG* out) {
    char buf[64] = {};
    if (GetEnvironmentVariableA(name, buf, sizeof(buf)) == 0) return;
    *out = (ULONG)strtoul(buf, nullptr, 16);
}

static void LoadConfig() {
    g_cfg.enable = 0;
    g_cfg.luidHi = 0;
    g_cfg.luidLo = 0;
    g_cfg.logLevel = 1;
    g_cfg.realLoaderPath[0] = 0;

    char buf[64] = {};
    if (GetEnvironmentVariableA("UPPERSCALE_ENABLE", buf, sizeof(buf)))
        g_cfg.enable = atoi(buf) != 0;
    ParseHexEnv("UPPERSCALE_GPU_LUID", &g_cfg.luidLo);
    ParseHexEnv("UPPERSCALE_GPU_LUID_HI", &g_cfg.luidHi);
    if (GetEnvironmentVariableA("UPPERSCALE_REAL_LOADER", buf, sizeof(buf)))
        strncpy_s(g_cfg.realLoaderPath, buf, _TRUNCATE);
    if (GetEnvironmentVariableA("UPPERSCALE_LOG", buf, sizeof(buf)))
        g_cfg.logLevel = atoi(buf);

    // ini fallback: upperscale.ini in CWD  [proxy] enable=1 gpu_luid_low=0x27214 ...
    FILE* ini = fopen("upperscale.ini", "r");
    if (ini) {
        char line[256];
        int inProxy = 0;
        while (fgets(line, sizeof(line), ini)) {
            char* p = line; while (*p == ' ' || *p == '\t') ++p;
            if (!stricmp(p, "[proxy]")) { inProxy = 1; continue; }
            if (p[0] == '[') { inProxy = 0; continue; }
            if (!inProxy) continue;
            char key[64], val[192];
            if (sscanf(p, "%63[^=]=%191s", key, val) == 2) {
                while (val[strlen(val)-1] == '\r' || val[strlen(val)-1] == '\n') val[strlen(val)-1] = 0;
                if (!stricmp(key, "enable")) g_cfg.enable = atoi(val);
                else if (!stricmp(key, "gpu_luid_low")) g_cfg.luidLo = (ULONG)strtoul(val, nullptr, 16);
                else if (!stricmp(key, "gpu_luid_high")) g_cfg.luidHi = (ULONG)strtoul(val, nullptr, 16);
                else if (!stricmp(key, "real_loader") && g_cfg.realLoaderPath[0] == 0)
                    strncpy_s(g_cfg.realLoaderPath, val, _TRUNCATE);
                else if (!stricmp(key, "log")) g_cfg.logLevel = atoi(val);
            }
        }
        fclose(ini);
    }

    // default real-loader path: upperscale_real_loader.dll next to our module (g_hInst set in DllMain)
    if (g_cfg.realLoaderPath[0] == 0 && g_hInst) {
        char mod[512] = {};
        GetModuleFileNameA(g_hInst, mod, sizeof(mod));
        char* slash = strrchr(mod, '\\');
        if (slash) *(slash + 1) = 0;
        strncpy_s(g_cfg.realLoaderPath, mod, _TRUNCATE);
        strncat_s(g_cfg.realLoaderPath, "upperscale_real_loader.dll", _TRUNCATE);
    }

    // log file in CWD (games set CWD to their install dir)
    GetCurrentDirectoryA(sizeof(g_logPath), g_logPath);
    strncat_s(g_logPath, "\\upperscale.log", _TRUNCATE);
}

// ---- Real loader loading (lazy — never in DllMain) ----
static int EnsureRealLoader() {
    if (g_pRealCreate) return 1;
    EnterCriticalSection(&g_cs);
    if (!g_pRealCreate) {
        LogAlways("loading real loader: %s", g_cfg.realLoaderPath);
        g_realLoader = LoadLibraryA(g_cfg.realLoaderPath);
        if (!g_realLoader) {
            LogAlways("ERROR: LoadLibrary(%s) failed err=%lu", g_cfg.realLoaderPath, GetLastError());
            LeaveCriticalSection(&g_cs);
            return 0;
        }
        g_pRealCreate   = (PfnFfxCreateContext)(void*)GetProcAddress(g_realLoader, "ffxCreateContext");
        g_pRealDestroy  = (PfnFfxDestroyContext)(void*)GetProcAddress(g_realLoader, "ffxDestroyContext");
        g_pRealConfigure= (PfnFfxConfigure)(void*)GetProcAddress(g_realLoader, "ffxConfigure");
        g_pRealQuery    = (PfnFfxQuery)(void*)GetProcAddress(g_realLoader, "ffxQuery");
        g_pRealDispatch = (PfnFfxDispatch)(void*)GetProcAddress(g_realLoader, "ffxDispatch");
        if (!g_pRealCreate || !g_pRealDestroy || !g_pRealConfigure || !g_pRealQuery || !g_pRealDispatch) {
            LogAlways("ERROR: real loader missing ffx* exports");
            FreeLibrary(g_realLoader); g_realLoader = nullptr;
            LeaveCriticalSection(&g_cs);
            return 0;
        }
        LogAlways("real loader loaded OK (all 5 exports resolved)");
    }
    LeaveCriticalSection(&g_cs);
    return 1;
}

// ---- GPU B device by LUID (lazy, cached) ----
static ID3D12Device* GetGpuBDevice() {
    if (g_gpuBDev) return g_gpuBDev;
    EnterCriticalSection(&g_cs);
    if (!g_gpuBDev) {
        IDXGIFactory1* factory = nullptr;
        if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory))) {
            for (UINT i = 0; ; ++i) {
                IDXGIAdapter1* adapter = nullptr;
                if (factory->EnumAdapters1(i, &adapter) != S_OK) break;
                DXGI_ADAPTER_DESC1 desc{};
                adapter->GetDesc1(&desc);
                if (desc.AdapterLuid.HighPart == g_cfg.luidHi && desc.AdapterLuid.LowPart == g_cfg.luidLo) {
                    ID3D12Device* dev = nullptr;
                    HRESULT hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device), (void**)&dev);
                    char nameA[512]{};
                    WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, nameA, sizeof(nameA), nullptr, nullptr);
                    if (SUCCEEDED(hr)) {
                        g_gpuBDev = dev;
                        LogAlways("created GPU-B device by LUID {%lx,%lx}: %s", g_cfg.luidHi, g_cfg.luidLo, nameA);
                    } else {
                        LogAlways("ERROR: D3D12CreateDevice on matched adapter failed hr=0x%08lX", (unsigned)hr);
                    }
                    break;
                }
                adapter->Release();
            }
            factory->Release();
        } else {
            LogAlways("ERROR: CreateDXGIFactory1 failed");
        }
    }
    LeaveCriticalSection(&g_cs);
    return g_gpuBDev;
}

// ---- Desc-chain device swap ----
// Walks the pNext chain; if a backend-DX12 node is found and active mode is on, replaces its
// device with our GPU-B device. Returns 1 if swapped (and stores original in *origOut).
static int SwapBackendDevice(ffxApiHeader* head, ID3D12Device** origOut) {
    if (!head || !g_cfg.enable) return 0;
    for (ffxApiHeader* n = head; n; n = n->pNext) {
        if (n->type == UPPERSCALE_DESC_TYPE_BACKEND_DX12) {
            upperscaleBackendDX12Desc* be = (upperscaleBackendDX12Desc*)n;
            ID3D12Device* bDev = GetGpuBDevice();
            if (!bDev) { LogAlways("ERROR: no GPU-B device; not swapping"); return 0; }
            *origOut = be->device;
            be->device = bDev;
            g_swapsDone++;
            LogAlways("SWAP: backend device %p -> %p (swap #%d)", *origOut, bDev, g_swapsDone);
            return 1;
        }
    }
    return 0;
}

// ---- Context table helpers (advisory; task 5 keys on ffxContext properly) ----
static void CtxTrack(ffxContext ctx, ID3D12Device* orig) {
    for (int i = 0; i < MAX_TRACKED_CTX; ++i)
        if (!g_ctxTable[i].ctx && ctx) { g_ctxTable[i].ctx = ctx; g_ctxTable[i].origDev = orig; return; }
}
static void CtxUntrack(ffxContext ctx) {
    for (int i = 0; i < MAX_TRACKED_CTX; ++i)
        if (g_ctxTable[i].ctx == ctx) { g_ctxTable[i].ctx = nullptr; g_ctxTable[i].origDev = nullptr; }
}

// ---- DllMain ----
BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hInst = hinst;
        DisableThreadLibraryCalls(hinst);
        InitializeCriticalSection(&g_cs);
        g_csInit = 1;
        LoadConfig();
        if (g_cfg.logLevel > 0) {
            g_log = fopen(g_logPath, "a");
            LogAlways("=== upperscale proxy loaded === mode=%s luid={%lx,%lx} log=%d",
                      g_cfg.enable ? "ACTIVE" : "PASSTHROUGH", g_cfg.luidHi, g_cfg.luidLo, g_cfg.logLevel);
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_log) { fclose(g_log); g_log = nullptr; }
        if (g_gpuBDev) { g_gpuBDev->Release(); g_gpuBDev = nullptr; }
        if (g_realLoader) { FreeLibrary(g_realLoader); g_realLoader = nullptr; }
        if (g_csInit) DeleteCriticalSection(&g_cs);
    }
    return TRUE;
}

// ---- The 5 exported FFX entry points ----
extern "C" {

ffxReturnCode_t __declspec(dllexport) ffxCreateContext(ffxContext* context, ffxApiHeader* desc, const void* memCb) {
    if (!context || !desc) return FFX_API_RETURN_ERROR_PARAMETER;
    ID3D12Device* origDev = nullptr;
    int swapped = SwapBackendDevice(desc, &origDev);   // no-op in passthrough mode
    if (g_cfg.logLevel >= 2) Log("ffxCreateContext topType=0x%llx swapped=%d", (unsigned long long)desc->type, swapped);
    if (!EnsureRealLoader()) return FFX_API_RETURN_ERROR;
    ffxReturnCode_t rc = g_pRealCreate(context, desc, memCb);
    LogAlways("ffxCreateContext: topType=0x%llx rc=%u swapped=%d ctx=%p", (unsigned long long)desc->type, rc, swapped, context ? *context : nullptr);
    if (rc == FFX_API_RETURN_OK && swapped) CtxTrack(*context, origDev);
    return rc;
}

ffxReturnCode_t __declspec(dllexport) ffxDestroyContext(ffxContext* context, const void* memCb) {
    if (!context) return FFX_API_RETURN_ERROR_PARAMETER;
    if (g_cfg.logLevel >= 2) Log("ffxDestroyContext ctx=%p", *context);
    if (!EnsureRealLoader()) return FFX_API_RETURN_ERROR;
    ffxReturnCode_t rc = g_pRealDestroy(context, memCb);
    LogAlways("ffxDestroyContext: rc=%u", rc);
    CtxUntrack(*context);
    return rc;
}

ffxReturnCode_t __declspec(dllexport) ffxConfigure(ffxContext* context, const ffxApiHeader* desc) {
    if (!desc) return FFX_API_RETURN_ERROR_PARAMETER;
    // configure descs (global debug etc.) carry no device — nothing to swap.
    if (g_cfg.logLevel >= 2) Log("ffxConfigure ctx=%p type=0x%llx", context, (unsigned long long)desc->type);
    if (!EnsureRealLoader()) return FFX_API_RETURN_ERROR;
    ffxReturnCode_t rc = g_pRealConfigure(context, desc);
    if (g_cfg.logLevel >= 2) Log("ffxConfigure: rc=%u", rc);
    return rc;
}

ffxReturnCode_t __declspec(dllexport) ffxQuery(ffxContext* context, ffxApiHeader* desc) {
    if (!desc) return FFX_API_RETURN_ERROR_PARAMETER;
    // Null-context queries (GET_VERSIONS, GetGPUMemoryUsageV2, ...) carry a backend node with the
    // game's device. Swap it too so all provider lookups target GPU B in active mode.
    ID3D12Device* origDev = nullptr;
    int swapped = SwapBackendDevice(desc, &origDev);
    if (g_cfg.logLevel >= 2) Log("ffxQuery ctx=%p type=0x%llx swapped=%d", context, (unsigned long long)desc->type, swapped);
    if (!EnsureRealLoader()) return FFX_API_RETURN_ERROR;
    ffxReturnCode_t rc = g_pRealQuery(context, desc);
    if (g_cfg.logLevel >= 2) Log("ffxQuery: rc=%u", rc);
    return rc;
}

ffxReturnCode_t __declspec(dllexport) ffxDispatch(ffxContext* context, const ffxApiHeader* desc) {
    if (!context || !desc) return FFX_API_RETURN_ERROR_PARAMETER;
    // TASK 5 PENDING: cross-GPU input transfer (color/depth/MV A->B via RAM bounce) is not wired yet.
    // In ACTIVE mode the context is bound to GPU B but the game's command list + resources live on
    // GPU A — forwarding would be undefined behavior. Refuse loudly instead of corrupting state.
    if (g_cfg.enable && g_swapsDone > 0) {
        LogAlways("ffxDispatch: ACTIVE mode without dispatch wiring — returning ERROR (task 5 pending). type=0x%llx", (unsigned long long)desc->type);
        return FFX_API_RETURN_ERROR;
    }
    if (g_cfg.logLevel >= 2) Log("ffxDispatch ctx=%p type=0x%llx", *context, (unsigned long long)desc->type);
    if (!EnsureRealLoader()) return FFX_API_RETURN_ERROR;
    ffxReturnCode_t rc = g_pRealDispatch(context, desc);
    if (g_cfg.logLevel >= 3) Log("ffxDispatch: rc=%u", rc);
    return rc;
}

// Internal helper for the smoke test / diagnostics (not part of AMD's ABI).
typedef struct upperscaleState {
    uint32_t magic;          // 'UPSC'
    int      enable;         // 1 active, 0 passthrough
    ULONG    luidHi, luidLo;
    void*    gpuBDevice;     // our LUID device (null if not created yet)
    void*    realLoader;     // HMODULE of AMD's real loader (null if not loaded)
    int      swapsDone;
} upperscaleState;

upperscaleState __declspec(dllexport) upperscaleGetState() {
    upperscaleState s{};
    s.magic = 'UPSC';
    s.enable = g_cfg.enable;
    s.luidHi = g_cfg.luidHi;
    s.luidLo = g_cfg.luidLo;
    s.gpuBDevice = (void*)g_gpuBDev;
    s.realLoader = (void*)g_realLoader;
    s.swapsDone = g_swapsDone;
    return s;
}

} // extern "C"

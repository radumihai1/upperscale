// upperscale_proxy.cpp — drop-in replacement for amd_fidelityfx_loader_dx12.dll
//
// What it does:
//   * Exports the exact 5 symbols AMD's loader exports (ffxConfigure, ffxCreateContext,
//     ffxDestroyContext, ffxDispatch, ffxQuery) + one internal helper (upperscaleGetState).
//   * In ACTIVE mode (config enabled): walks the pNext desc chain on create/query, finds the
//     FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12 node and SWAPS its ID3D12Device* for a
//     device we created BY LUID on the user-selected GPU B. All FFX compute then runs on B.
//   * In ACTIVE mode ffxDispatch is fully wired (task 5): upperscale_xgpu.cpp RAM-bounces each
//     input A->B, records FFX into our own GPU-B command list, executes it, captures the output
//     back through RAM and copies it into the game's original output texture via the game's
//     command list. The desc is restored exactly before returning.
//   * In PASSTHROUGH mode (default): forwards everything untouched to AMD's real loader, which
//     we load from a bundled copy (upperscale_real_loader.dll next to us) or an explicit path.
//     This is the safe first in-game test: proves drop-in replacement works without changing
//     where FFX runs.
//   * Keeps per-context bookkeeping (original game device vs swapped device + transfer hub).
//
// Config (env vars, read at DllMain; ini fallback upperscale.ini in CWD):
//   UPPERSCALE_ENABLE=1            enable active mode (device swap)
//   UPPERSCALE_GPU_LUID=<hex>      low part of GPU B LUID (e.g. 0x27214)
//   UPPERSCALE_GPU_LUID_HI=<hex>   high part (default 0)
//   UPPERSCALE_REAL_LOADER=<path>  path to AMD's real loader DLL (default: upperscale_real_loader.dll next to us)
//   UPPERSCALE_LOG=<0|1|2>         0=off, 1=create/destroy only (default), 2+=verbose per-call

#include "upperscale_xgpu.h"   // windows.h + d3d12/dxgi + FFX SDK headers in the right order
#include "upperscale_overlay.h"  // HUD + live control API (g_upperscaleStats defined below)
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

// ---- FFX API (ABI from AMD's public headers; we only need the 5 entry points + desc layout) ----
// Deliberately local typedefs instead of including ffx_api.h: that header declares its entry
// points with __declspec(dllexport), which collides with our own definitions in this DLL (C2733).
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
    int      fgOnB;        // 1 = frame generation on GPU B (default), 0 = FG native on A (safe mode)
};

static Config          g_cfg{};
// Exposed to upperscale_xgpu.cpp so its [xgpu] diagnostics honor the ini/env log level in-game.
int g_cfgLogLevel = 0;
// fg=1: frame generation also runs on GPU B (full cross-GPU, EXPERIMENTAL — Cyberpunk's FG-on-B
// path removes device B after the first GENERATE dispatch). fg=0 (default): FG stays native on the
// main GPU A and its dispatches are forwarded untouched — only upscaling goes cross-GPU. Wired in
// v10: ffxCreateContext skips the device swap for FG-family contexts, ffxDispatch forwards them.
int g_cfgFgOnB = 0;
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

// ---- Live stats + control state (read by the HUD thread, written by render/config threads) ----
UpperscaleStats g_upperscaleStats{};   // zero-initialized at load time

static void LogAlways(const char* fmt, ...);   // fwd — defined below with the other logging helpers

// Persist a single key=value line into upperscale.ini in CWD (create if missing). Used by the
// live hotkey toggles so a mode/log/fg change survives a game restart. Best-effort: never blocks
// or fails the render path — on any error we just skip persistence.
// Section-aware: only replaces keys inside [proxy]; every other section, comment and line is
// preserved verbatim. Writes to a temp file first and atomically renames over the ini so a crash
// mid-write can never corrupt it.
static void IniSetKey(const char* key, const char* value) {
    char dir[MAX_PATH] = {};
    if (!GetCurrentDirectoryA(sizeof(dir), dir)) return;
    char iniPath[MAX_PATH * 2] = {};
    snprintf(iniPath, sizeof(iniPath), "%s\\upperscale.ini", dir);

    // Read existing lines (keep everything except the key we're replacing inside [proxy]). If no
    // ini exists yet, buf stays empty and a fresh [proxy] section is created below.
    FILE* f = fopen(iniPath, "r");
    char buf[8192] = {};
    size_t n = 0;
    if (f) {
        while (n < sizeof(buf) - 64 && fgets(buf + n, sizeof(buf) - n, f)) n += strlen(buf + n);
        fclose(f);
    }

    char out[16384] = {};
    size_t o = 0;
    int inProxy = 0;      // current section while scanning
    int replaced = 0;     // did we drop the old occurrence of our key?
    int sawProxySection = (strstr(buf, "[proxy]") != nullptr);

    char* savep = nullptr;
    for (char* line = strtok_s(buf, "\r\n", &savep); line; line = strtok_s(nullptr, "\r\n", &savep)) {
        // section header? (first non-whitespace char is '[') — else: maybe the key we're replacing.
        {
            char* p = line; while (*p == ' ' || *p == '\t') ++p;
            if (*p == '[') {
                inProxy = (!stricmp(p, "[proxy]"));
            } else if (inProxy && !replaced && !strnicmp(p, key, strlen(key)) && p[strlen(key)] == '=') {
                replaced = 1;   // skip the old occurrence of our key inside [proxy] — new one appended below
                continue;
            }
        }
        snprintf(out + o, sizeof(out) - o, "%s\n", line);
        o += strlen(out + o);
    }

    if (!replaced) {
        // append (or create) the key at the end of [proxy] — open a new section if none existed.
        if (!sawProxySection) { snprintf(out + o, sizeof(out) - o, "[proxy]\n"); o += strlen(out + o); }
        snprintf(out + o, sizeof(out) - o, "%s=%s\n", key, value);
    }

    // Atomic replace: write temp file in the same directory, then rename over the target.
    char tmpPath[MAX_PATH * 2] = {};
    snprintf(tmpPath, sizeof(tmpPath), "%s\\upperscale.ini.tmp", dir);
    FILE* w = fopen(tmpPath, "w");
    if (!w) return;
    fputs(out, w);
    fclose(w);
    MoveFileExA(tmpPath, iniPath, MOVEFILE_REPLACE_EXISTING);   // fails silently — best effort
}

// Live control API — called by the HUD hotkeys (and exportable for external tools).
int upperscaleSetMode(int enable) {
    EnterCriticalSection(&g_cs);
    g_cfg.enable = enable ? 1 : 0;
    LeaveCriticalSection(&g_cs);
    InterlockedExchange(&g_upperscaleStats.mode, g_cfg.enable);
    IniSetKey("enable", g_cfg.enable ? "1" : "0");
    LogAlways("LIVE: mode -> %s (persisted to upperscale.ini)", g_cfg.enable ? "ACTIVE" : "PASSTHROUGH");
    return g_cfg.enable;
}

int upperscaleSetLogLevel(int level) {
    if (level < 0) level = 0;
    if (level > 3) level = 3;
    EnterCriticalSection(&g_cs);
    g_cfg.logLevel = level;
    LeaveCriticalSection(&g_cs);
    g_cfgLogLevel = level;   // upperscale_xgpu.cpp reads this for [xgpu] diagnostics
    InterlockedExchange((volatile LONG*)&g_upperscaleStats.logLevel, level);
    char val[2] = { (char)('0' + level), 0 };
    IniSetKey("log", val);
    LogAlways("LIVE: log level -> %d (persisted to upperscale.ini)", level);
    return level;
}

// Live FG toggle (Home hotkey). Affects contexts created AFTER the toggle — Cyberpunk creates its
// FFX contexts at startup, so a live flip takes effect on next game launch. The ini write is what
// makes it stick: LoadConfig reads fg= at attach time.
int upperscaleSetFgOnB(int on) {
    EnterCriticalSection(&g_cs);
    g_cfg.fgOnB = on ? 1 : 0;
    LeaveCriticalSection(&g_cs);
    g_cfgFgOnB = g_cfg.fgOnB;   // published for the create/dispatch gates
    InterlockedExchange((volatile LONG*)&g_upperscaleStats.fgOnB, g_cfg.fgOnB);
    IniSetKey("fg", g_cfg.fgOnB ? "1" : "0");
    LogAlways("LIVE: fg -> %d (persisted to upperscale.ini; takes effect for contexts created after restart)", g_cfg.fgOnB);
    return g_cfg.fgOnB;
}

// context bookkeeping for task 5: ffxContext -> original game device (GPU A) + transfer hub.
// swapped=1: backend device was replaced with GPU B at create time — dispatches MUST be routed
// through the cross-GPU intercept. swapped=0: FG-family context kept native on GPU A because of
// fg=0 (safe mode) — its dispatches are forwarded untouched to the real loader.
struct CtxInfo { ffxContext ctx; ID3D12Device* origDev; XGpuHub* hub; int swapped; int nativeLogged; };
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
    // fg default is 0 (SAFE MODE) as of v10: Cyberpunk's FG-on-B path removes GPU B after the first
    // GENERATE dispatch (see docs/HANDOFF.md §4). Until that is fixed, frame generation stays native
    // on GPU A and only upscaling goes cross-GPU. Set fg=1 in upperscale.ini to opt into full
    // cross-GPU FG (experimental — known crash in Cyberpunk 2077).
    g_cfg.fgOnB = 0;
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
    if (GetEnvironmentVariableA("UPPERSCALE_FG", buf, sizeof(buf)))
        g_cfg.fgOnB = atoi(buf) != 0;

    // ini fallback: upperscale.ini in CWD  [proxy] enable=1 gpu_luid_low=0x27214 ...
    FILE* ini = fopen("upperscale.ini", "r");
    if (ini) {
        char line[256];
        int inProxy = 0;
        while (fgets(line, sizeof(line), ini)) {
            // strip trailing CR/LF/whitespace so "[proxy]\r\n" matches and values are clean.
            for (int li = (int)strlen(line); li > 0 && (line[li-1]=='\r' || line[li-1]=='\n' || line[li-1]==' ' || line[li-1]=='\t'); ) line[--li] = 0;
            char* p = line; while (*p == ' ' || *p == '\t') ++p;
            if (!stricmp(p, "[proxy]")) { inProxy = 1; continue; }
            if (p[0] == '[') { inProxy = 0; continue; }   // any other section ends [proxy]
            if (!inProxy || !*p) continue;
            char key[64], val[192];
            if (sscanf(p, "%63[^=]=%191s", key, val) == 2) {
                // trim trailing whitespace from value (already stripped line-ending above).
                while (*val && (val[strlen(val)-1]==' ' || val[strlen(val)-1]=='\t')) val[strlen(val)-1] = 0;
                if (!stricmp(key, "enable")) g_cfg.enable = atoi(val);
                else if (!stricmp(key, "gpu_luid_low")) g_cfg.luidLo = (ULONG)strtoul(val, nullptr, 16);
                else if (!stricmp(key, "gpu_luid_high")) g_cfg.luidHi = (ULONG)strtoul(val, nullptr, 16);
                else if (!stricmp(key, "real_loader") && g_cfg.realLoaderPath[0] == 0)
                    strncpy_s(g_cfg.realLoaderPath, val, _TRUNCATE);
                else if (!stricmp(key, "log")) g_cfg.logLevel = atoi(val);
                else if (!stricmp(key, "fg")) g_cfg.fgOnB = atoi(val) != 0;
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

    // publish the final level so upperscale_xgpu.cpp's [xgpu] diagnostics honor it in-game.
    g_cfgLogLevel = g_cfg.logLevel;
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
                        strncpy_s(g_upperscaleStats.gpuBName, nameA, _TRUNCATE);   // HUD display
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
static void CtxTrack(ffxContext ctx, ID3D12Device* orig, int swapped) {
    for (int i = 0; i < MAX_TRACKED_CTX; ++i)
        if (!g_ctxTable[i].ctx && ctx) { g_ctxTable[i].ctx = ctx; g_ctxTable[i].origDev = orig; g_ctxTable[i].hub = nullptr; g_ctxTable[i].swapped = swapped; return; }
}
static void CtxUntrack(ffxContext ctx) {
    for (int i = 0; i < MAX_TRACKED_CTX; ++i)
        if (g_ctxTable[i].ctx == ctx) {
            XGpuHub* hub = g_ctxTable[i].hub;
            // only release the hub if no other tracked context still uses it (hubs are shared per device pair)
            int stillUsed = 0;
            for (int j = 0; j < MAX_TRACKED_CTX; ++j)
                if (j != i && g_ctxTable[j].hub == hub) { stillUsed = 1; break; }
            if (hub && !stillUsed) xgpuReleaseHub(hub);
            g_ctxTable[i].ctx = nullptr; g_ctxTable[i].origDev = nullptr; g_ctxTable[i].hub = nullptr;
        }
}
static CtxInfo* CtxFind(ffxContext ctx) {
    for (int i = 0; i < MAX_TRACKED_CTX; ++i)
        if (g_ctxTable[i].ctx == ctx) return &g_ctxTable[i];
    return nullptr;
}

// ---- DllMain ----
BOOL WINAPI DllMain(HINSTANCE hinst, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_hInst = hinst;
        DisableThreadLibraryCalls(hinst);
        InitializeCriticalSection(&g_cs);
        g_csInit = 1;
        LoadConfig();
        g_cfgFgOnB = g_cfg.fgOnB;   // published for the dispatch gate in ffxDispatch below
        // publish config into the live stats block (HUD reads it) and start the debug HUD thread.
        g_upperscaleStats.mode = g_cfg.enable ? 1 : 0;
        g_upperscaleStats.logLevel = g_cfg.logLevel;
        g_upperscaleStats.fgOnB = g_cfg.fgOnB;
        g_upperscaleStats.luidLo = g_cfg.luidLo;
        g_upperscaleStats.luidHi = g_cfg.luidHi;
        upperscaleOverlayStart(1);   // always available — Insert hides it (debug tool)
        if (g_cfg.logLevel > 0) {
            g_log = fopen(g_logPath, "a");
            LogAlways("=== upperscale proxy loaded v10 === mode=%s luid={%lx,%lx} log=%d fg=%d",
                      g_cfg.enable ? "ACTIVE" : "PASSTHROUGH", g_cfg.luidHi, g_cfg.luidLo, g_cfg.logLevel, g_cfg.fgOnB);
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_log) { fclose(g_log); g_log = nullptr; }
        if (g_gpuBDev) { g_gpuBDev->Release(); g_gpuBDev = nullptr; }
        if (g_realLoader) { FreeLibrary(g_realLoader); g_realLoader = nullptr; }
        if (g_csInit) DeleteCriticalSection(&g_cs);
    }
    return TRUE;
}

// ---- The 5 exported FFX entry points (signatures match AMD's ABI exactly — see ffx_api.h) ----
extern "C" {

// FG-family effect check: the top-level create desc type encodes backend|effect|subversion.
// Effect ids (ffx_api.h): UPSCALE 0x1xxxx, FRAMEGENERATION 0x2xxxx, FGSWAPCHAIN 0x3xxxx,
// FGSWAPCHAIN_VK 0x4xxxx — i.e. effect bytes 0x2/0x3/0x4 are all frame-generation family.
static int IsFgFamilyContextType(uint64_t topType) {
    uint32_t eff = (uint32_t)(topType & 0x00ff0000ull);   // FFX_API_EFFECT_MASK
    return (eff == 0x00020000u || eff == 0x00030000u || eff == 0x00040000u);
}

ffxReturnCode_t __declspec(dllexport) ffxCreateContext(ffxContext* context, ffxApiHeader* desc, const void* memCb) {
    if (!context || !desc) return FFX_API_RETURN_ERROR_PARAMETER;
    // fg=0 (safe mode): FG-family contexts keep the game's GPU-A device — no swap. Their dispatches
    // are then forwarded untouched (see ffxDispatch). Only upscaling goes cross-GPU. Cyberpunk's
    // whole FSR4 pipeline is FG-family, so fg=0 ≈ passthrough there; standalone upscale contexts
    // still offload to GPU B.
    int skipSwap = 0;
    if (g_cfg.enable && !g_cfgFgOnB && IsFgFamilyContextType(desc->type)) {
        skipSwap = 1;
        LogAlways("fg=0: FG-family context topType=0x%llx kept native on GPU A (no device swap)", (unsigned long long)desc->type);
    }
    ID3D12Device* origDev = nullptr;
    int swapped = skipSwap ? 0 : SwapBackendDevice(desc, &origDev);   // no-op in passthrough mode
    if (g_cfg.logLevel >= 2) Log("ffxCreateContext topType=0x%llx swapped=%d", (unsigned long long)desc->type, swapped);
    if (!EnsureRealLoader()) return FFX_API_RETURN_ERROR;
    ffxReturnCode_t rc = g_pRealCreate(context, desc, memCb);   // memCb is an opaque const void* (ffxAllocationCallbacks*)
    LogAlways("ffxCreateContext: topType=0x%llx rc=%u swapped=%d ctx=%p", (unsigned long long)desc->type, rc, swapped, context ? *context : nullptr);
    if (rc == FFX_API_RETURN_OK && g_cfg.enable) CtxTrack(*context, origDev, swapped);   // track unswapped FG ctxs too (dispatch gate keys on it)
    return rc;
}

ffxReturnCode_t __declspec(dllexport) ffxDestroyContext(ffxContext* context, const void* memCb) {
    if (!context) return FFX_API_RETURN_ERROR_PARAMETER;
    if (g_cfg.logLevel >= 2) Log("ffxDestroyContext ctx=%p", *context);
    if (!EnsureRealLoader()) return FFX_API_RETURN_ERROR;
    ffxReturnCode_t rc = g_pRealDestroy(context, memCb);   // opaque const void* (ffxAllocationCallbacks*)
    LogAlways("ffxDestroyContext: rc=%u", rc);
    CtxUntrack(*context);
    return rc;
}

ffxReturnCode_t __declspec(dllexport) ffxConfigure(ffxContext* context, const ffxApiHeader* desc) {
    if (!desc) return FFX_API_RETURN_ERROR_PARAMETER;
    // E1 diagnostic (UNCONDITIONAL — one-shot at startup, must fire even at log=1): for FG configure
    // (type 0x20002), log the swapchain + callback pointers. If Cyberpunk passes a non-null
    // swapChain and FFX references it during GENERATE dispatch on GPU B, that is the cross-adapter
    // violation removing device B (hypothesis A). Layout per ffx_framegeneration.h: header(16B),
    // swapChain@16, presentCallback@24, presentCallbackUserContext@32, frameGenerationCallback@40,
    // fgCallbackUserContext@48.
    if (desc->type == 0x20002ull) {
        const uint8_t* p = (const uint8_t*)desc;
        LogAlways("E1: ffxConfigure FG ctx=%p swapChain=%p presentCallback=%p pcUserCtx=%p fgCallback=%p fgUserCtx=%p pNext=%p",
                  context, *(const void**)(p + 16), *(const void**)(p + 24), *(const void**)(p + 32),
                  *(const void**)(p + 40), *(const void**)(p + 48), desc->pNext);
    }
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
    // ACTIVE mode: route the dispatch through the cross-GPU transfer hub (task 5 + task 6a).
    // The hub bounces inputs A->B via RAM, records FFX into our GPU-B command list, executes it,
    // captures the output(s) back to A-side upload slots and records the copy-backs into the game's
    // original command list. Handles upscale + reactive-mask + FG-prepare + FG-generation node types;
    // unrecognized dispatch types are refused (ERROR_PARAMETER) — forwarding them would mix GPU-A
    // resources with the GPU-B-bound context (undefined behavior).
    if (g_cfg.enable && g_swapsDone > 0) {
        CtxInfo* ci = CtxFind(*context);
        // fg=0 gate: this context was created WITHOUT a device swap (FG-family kept native on GPU A).
        // Its dispatches must be forwarded untouched — bouncing them would mix GPU-A resources with a
        // GPU-B-bound context. One log line per such context, then quiet.
        if (ci && !ci->swapped) {
            if (!EnsureRealLoader()) return FFX_API_RETURN_ERROR;
            if (!ci->nativeLogged) { ci->nativeLogged = 1; LogAlways("ffxDispatch: ctx=%p is native-on-A (fg=0) — forwarding untouched", *context); }
            return g_pRealDispatch(context, desc);
        }
        if (!EnsureRealLoader()) return FFX_API_RETURN_ERROR;   // need g_pRealDispatch for the intercept
        ID3D12Device* origDev = ci ? ci->origDev : nullptr;
        // Fallback: no tracked context (e.g. query-only flow) — cannot bounce without the game device.
        if (!origDev) {
            LogAlways("ffxDispatch: ACTIVE mode but no original device for ctx=%p — forwarding as-is", *context);
        } else {
            if (!ci->hub) ci->hub = xgpuGetOrCreateHub(origDev, GetGpuBDevice());
            if (ci->hub) {
                ffxReturnCode_t rc = xgpuInterceptDispatch(ci->hub, *context, desc, (void*)g_pRealDispatch);
                if (g_cfg.logLevel >= 3) Log("ffxDispatch: intercepted type=0x%llx rc=%u", (unsigned long long)desc->type, rc);
                return rc;
            }
            LogAlways("ERROR: xgpu hub creation failed — dispatch will be forwarded as-is (undefined behavior risk)");
        }
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

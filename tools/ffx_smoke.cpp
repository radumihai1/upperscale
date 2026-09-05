// ffx_smoke.cpp — smoke test for the upperscale proxy DLL (task 4b).
// Simulates a game: creates a D3D12 device on GPU A, loads OUR amd_fidelityfx_dx12.dll,
// builds the FFX create-desc chain with a backend node pointing at GPU A's device, and calls
// ffxCreateContext. Verifies via upperscaleGetState() + return codes that:
//   ACTIVE mode      -> proxy swapped the device to GPU B (LUID), context created on B
//   PASSTHROUGH mode -> no swap, forwarded untouched to AMD's real loader
//
// Run from a dir containing: amd_fidelityfx_dx12.dll (ours) + upperscale_real_loader.dll (AMD's).
// Env: UPPERSCALE_ENABLE=1|0  UPPERSCALE_GPU_LUID=<hex>  UPPERSCALE_LOG=2

#include <d3d12.h>
#include <dxgi1_6.h>
#include <combaseapi.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <windows.h>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

// FFX ABI (same as proxy)
typedef void* ffxContext;
typedef uint32_t ffxReturnCode_t;
#define FFX_API_RETURN_OK 0u
struct ffxApiHeader { uint64_t type; struct ffxApiHeader* pNext; };
#define DESC_TYPE_UPSCALE        0x00010000ull   // MAKE_EFFECT_SUB_ID(UPSCALE=0x00010000, 0)
#define DESC_TYPE_BACKEND_DX12   0x00000002ull
#define DESC_TYPE_OVERRIDE_VER   5ull

struct DescUpscale {
    ffxApiHeader header;
    uint32_t flags;
    struct { uint32_t width, height; } maxRenderSize, maxUpscaleSize;
    void* fpMessage;
};
struct DescBackendDX12 { ffxApiHeader header; ID3D12Device* device; };
struct DescOverrideVersion { ffxApiHeader header; uint64_t versionId; };

typedef ffxReturnCode_t (*PfnCreate)(ffxContext*, ffxApiHeader*, const void*);
typedef ffxReturnCode_t (*PfnDestroy)(ffxContext*, const void*);
typedef ffxReturnCode_t (*PfnQuery)(ffxContext*, ffxApiHeader*);

struct ProxyState { uint32_t magic; int enable; ULONG luidHi, luidLo; void* gpuBDevice; void* realLoader; int swapsDone; };
typedef ProxyState (*PfnGetState)();

static ID3D12Device* CreateDeviceByLuid(ULONG hi, ULONG lo) {
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return nullptr;
    for (UINT i = 0; ; ++i) {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapters1(i, &adapter) != S_OK) break;
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (desc.AdapterLuid.HighPart == hi && desc.AdapterLuid.LowPart == lo) {
            ID3D12Device* dev = nullptr;
            HRESULT hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev));
            char nameA[512]{};
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, nameA, sizeof(nameA), nullptr, nullptr);
            printf("game device on adapter %u: %s (LUID {%lx,%lx}) -> %s\n", i, nameA, hi, lo, SUCCEEDED(hr) ? "OK" : "FAIL");
            adapter->Release(); factory->Release();
            return dev;
        }
        adapter->Release();
    }
    factory->Release();
    return nullptr;
}

static void FfxMsg(uint32_t type, const wchar_t* msg) {
    char buf[512]{};
    int n = WideCharToMultiByte(CP_UTF8, 0, msg, -1, buf, sizeof(buf)-1, nullptr, nullptr);
    printf("  [FFX %s] %.*s\n", type == 0 ? "ERROR" : "WARN", n, buf);
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    // GPU A = 7900 XTX (the game's render device in this scenario)
    ID3D12Device* devA = CreateDeviceByLuid(0, 0x2A089u);
    if (!devA) { printf("FAIL: no GPU A\n"); return 1; }

    HMODULE hProxy = LoadLibraryA("amd_fidelityfx_dx12.dll");   // OURS (in CWD)
    if (!hProxy) { printf("FAIL: LoadLibrary(ours) err=%lu\n", GetLastError()); return 1; }
    PfnCreate pCreate = (PfnCreate)(void*)GetProcAddress(hProxy, "ffxCreateContext");
    PfnDestroy pDestroy = (PfnDestroy)(void*)GetProcAddress(hProxy, "ffxDestroyContext");
    PfnQuery pQuery = (PfnQuery)(void*)GetProcAddress(hProxy, "ffxQuery");
    PfnGetState pState = (PfnGetState)(void*)GetProcAddress(hProxy, "upperscaleGetState");
    if (!pCreate || !pDestroy || !pQuery || !pState) { printf("FAIL: proxy exports missing\n"); return 1; }

    ProxyState st0 = pState();
    printf("proxy state at load: magic=0x%x enable=%d luid={%lx,%lx} swaps=%d realLoader=%p gpuB=%p\n",
           st0.magic, st0.enable, st0.luidHi, st0.luidLo, st0.swapsDone, st0.realLoader, st0.gpuBDevice);

    // Two-phase version query through the proxy (null context) — like a game does at init
    uint64_t verIds[8] = {0}; const char* verNames[8] = {nullptr};
    int nVer = 0;
    {
        struct QV { ffxApiHeader header; uint64_t createDescType; void* device; uint64_t* outputCount; uint64_t* versionIds; const char** versionNames; } q{};
        q.header.type = 4ull; // FFX_API_QUERY_DESC_TYPE_GET_VERSIONS
        q.createDescType = DESC_TYPE_UPSCALE;
        q.device = devA;
        uint64_t count = 0; q.outputCount = &count;
        ffxReturnCode_t rc1 = pQuery(nullptr, &q.header);
        printf("proxy ffxQuery(GET_VERSIONS) phase1: rc=%u count=%llu\n", rc1, (unsigned long long)count);
        if (rc1 == 0 && count > 0 && count <= 8) {
            nVer = (int)count;
            q.versionIds = verIds; q.versionNames = verNames;
            pQuery(nullptr, &q.header);
        }
    }
    for (int i = 0; i < nVer; ++i) printf("  version[%d] id=0x%llx name=%s\n", i, (unsigned long long)verIds[i], verNames[i]);

    // Build create-desc chain: upscale -> overrideVersion(FSR4) -> backend(devA)
    static DescUpscale up{};
    up.header.type = DESC_TYPE_UPSCALE;
    up.maxRenderSize.width = 512; up.maxRenderSize.height = 288;
    up.maxUpscaleSize.width = 1920; up.maxUpscaleSize.height = 1080;
    up.fpMessage = FfxMsg;

    static DescOverrideVersion ov{};
    ov.header.type = DESC_TYPE_OVERRIDE_VER;
    if (nVer > 0) ov.versionId = verIds[0];

    static DescBackendDX12 be{};
    be.header.type = DESC_TYPE_BACKEND_DX12;
    be.device = devA;   // the game's GPU-A device — proxy must swap this in ACTIVE mode

    if (nVer > 0) { up.header.pNext = &ov.header; ov.header.pNext = &be.header; }
    else up.header.pNext = &be.header;
    be.header.pNext = nullptr;

    ffxContext ctx = nullptr;
    printf(">> proxy ffxCreateContext (backend device = GPU A %p)...\n", devA);
    ffxReturnCode_t rc = pCreate(&ctx, &up.header, nullptr);
    ProxyState st1 = pState();
    printf("ffxCreateContext: rc=%u ctx=%p\n", rc, ctx);
    printf("proxy state after create: swaps=%d gpuB=%p realLoader=%p\n", st1.swapsDone, st1.gpuBDevice, st1.realLoader);

    int pass = 1;
    if (st0.enable) {
        // ACTIVE: expect a swap to GPU B and successful context creation on B
        if (rc != FFX_API_RETURN_OK) { printf("FAIL(active): create rc=%u\n", rc); pass = 0; }
        if (st1.swapsDone < 1 || !st1.gpuBDevice) { printf("FAIL(active): no swap recorded\n"); pass = 0; }
        if (st1.gpuBDevice == devA) { printf("FAIL(active): gpuB device is the game's own device!\n"); pass = 0; }
    } else {
        // PASSTHROUGH: expect no swap, forwarded to real loader on GPU A
        if (rc != FFX_API_RETURN_OK) { printf("FAIL(passthrough): create rc=%u\n", rc); pass = 0; }
        if (st1.swapsDone != 0 || st1.gpuBDevice) { printf("FAIL(passthrough): unexpected swap\n"); pass = 0; }
    }

    // Provider version query on the created context
    struct QPV { ffxApiHeader header; uint64_t versionId; const char* versionName; } pv{};
    pv.header.type = 6ull; // FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION
    if (rc == FFX_API_RETURN_OK) {
        ffxReturnCode_t qrc = pQuery(&ctx, &pv.header);
        printf("provider version: rc=%u id=0x%llx name=%s\n", qrc, (unsigned long long)pv.versionId, pv.versionName ? pv.versionName : "(null)");
    }

    if (rc == FFX_API_RETURN_OK) {
        ffxReturnCode_t drc = pDestroy(&ctx, nullptr);
        printf("ffxDestroyContext: rc=%u\n", drc);
        if (drc != FFX_API_RETURN_OK) pass = 0;
    }

    devA->Release();
    FreeLibrary(hProxy);
    printf(pass ? "SMOKE PASS (%s mode)\n" : "SMOKE FAIL\n", st0.enable ? "ACTIVE" : "PASSTHROUGH");
    return pass ? 0 : 1;
}

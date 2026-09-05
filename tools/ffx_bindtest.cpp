// ffx_bindtest.cpp — DE-RISK THE CORE ASSUMPTION:
// Does the REAL signed amd_fidelityfx_upscaler_dx12.dll (FSR4 v4.1.1) create a working
// context on a D3D12 device we created BY LUID on GPU B (RX 9060 XT)?
//
// This is exactly what our proxy DLL will do: intercept ffxCreateContext, swap the
// ID3D12Device* in the backend desc to a GPU-B device, forward to the real effect DLL.
// If this test passes, the architecture is proven at the API level.
//
// Usage: ffx_bindtest.exe [A|B]   (default B)
#include <d3d12.h>
#include <dxgi1_6.h>
#include <combaseapi.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <windows.h>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

// Real SDK headers (ABI-guaranteed match with the signed DLLs)
#include "../third_party/FidelityFX-SDK/Kits/FidelityFX/api/include/dx12/ffx_api_dx12.h"
#include "../third_party/FidelityFX-SDK/Kits/FidelityFX/upscalers/include/ffx_upscale.h"

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
            printf("matched adapter %u: %s -> D3D12 device %s\n", i, nameA, SUCCEEDED(hr) ? "OK" : "FAILED");
            adapter->Release(); factory->Release();
            return dev;
        }
        adapter->Release();
    }
    factory->Release();
    return nullptr;
}

// ---- FFX message callback (errors/warnings from the effect DLL) ----
static void FfxMsg(uint32_t type, const wchar_t* msg) {
    char buf[1024]{};
    int n = WideCharToMultiByte(CP_UTF8, 0, msg, -1, buf, sizeof(buf)-1, nullptr, nullptr);
    printf("  [FFX %s] %.*s\n", type == FFX_API_MESSAGE_TYPE_ERROR ? "ERROR" : "WARN", n, buf);
}

// ---- Logging resource allocator: proves which device FFX allocates on ----
static ID3D12Device* g_allocDev = nullptr;   // the device we hand to FFX (should equal devB)
static int g_allocCount = 0;
static uint64_t g_allocBytes = 0;

static ffxReturnCode_t MyResourceAlloc(uint32_t effectId, D3D12_RESOURCE_STATES initialState,
                                       const D3D12_HEAP_PROPERTIES* pHeapProps,
                                       const D3D12_RESOURCE_DESC* pD3DDesc,
                                       const FfxApiResourceDescription* pFfxDesc,
                                       const D3D12_CLEAR_VALUE* pOptimizedClear,
                                       ID3D12Resource** ppD3DResource) {
    (void)pHeapProps; (void)pFfxDesc; (void)pOptimizedClear;
    printf("  [ALLOC] dim=%u fmt=0x%x w=%llu h=%llu depth=%u mips=%u flags=0x%x state=0x%x heapType=%d\n",
           pD3DDesc->Dimension, (unsigned)pD3DDesc->Format, (unsigned long long)pD3DDesc->Width,
           (unsigned long long)pD3DDesc->Height, pD3DDesc->DepthOrArraySize, pD3DDesc->MipLevels,
           (unsigned)pD3DDesc->Flags, (unsigned)initialState, pHeapProps ? (int)pHeapProps->Type : -1);
    fflush(stdout);
    // Honor the requested heap properties (FFX asks for UPLOAD when it wants to Map).
    D3D12_HEAP_PROPERTIES hp{};
    if (pHeapProps) hp = *pHeapProps; else hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    HRESULT hr = g_allocDev->CreateCommittedResource(
        &hp, D3D12_HEAP_FLAG_NONE,
        pD3DDesc, initialState, nullptr, IID_PPV_ARGS(ppD3DResource));
    printf("  [ALLOC] -> hr=0x%08x\n", (unsigned)hr); fflush(stdout);
    if (SUCCEEDED(hr)) { g_allocCount++; g_allocBytes += pD3DDesc->Width; }
    return SUCCEEDED(hr) ? FFX_API_RETURN_OK : FFX_API_RETURN_ERROR_RUNTIME_ERROR;
}

static ffxReturnCode_t MyResourceDealloc(uint32_t effectId, ID3D12Resource* res) {
    (void)effectId; if (res) res->Release(); return FFX_API_RETURN_OK;
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    // LUIDs from device_probe: A=7900 XTX {0x2A089}, B=9060 XT {0x27214} (HighPart 0)
    ULONG hi = 0;
    ULONG lo = (argc > 1 && strcmp(argv[1], "A") == 0) ? 0x2A089u : 0x27214u;

    ID3D12Device* dev = CreateDeviceByLuid(hi, lo);
    if (!dev) { printf("FAIL: no device for LUID low=0x%X\n", lo); return 1; }

    // Load the REAL signed FSR4 upscaler DLL (what our proxy forwards to)
    const char* dllPath = "C:\\Users\\mrmih\\Playground\\AI\\upperscale\\third_party\\FidelityFX-SDK\\Kits\\FidelityFX\\signedbin\\amd_fidelityfx_upscaler_dx12.dll";
    HMODULE hUp = LoadLibraryA(dllPath);
    if (!hUp) { printf("FAIL: LoadLibrary upscaler err=%lu\n", GetLastError()); return 1; }
    PfnFfxCreateContext pCreate = (PfnFfxCreateContext)(void*)GetProcAddress(hUp, "ffxCreateContext");
    PfnFfxDestroyContext pDestroy = (PfnFfxDestroyContext)(void*)GetProcAddress(hUp, "ffxDestroyContext");
    PfnFfxQuery pQuery = (PfnFfxQuery)(void*)GetProcAddress(hUp, "ffxQuery");
    if (!pCreate || !pDestroy || !pQuery) { printf("FAIL: missing exports\n"); return 1; }
    printf("loaded real upscaler DLL, all 3 ffx* entry points resolved\n");

    // Enumerate available FSR4 versions (null-context query, like games do).
    // Two-phase: first call with outputCount only -> count; second fills ids/names.
    uint64_t versionIds[16] = {0};
    const char* versionNames[16] = {nullptr};
    int nVersions = 0;
    {
        ffxQueryDescGetVersions q{};
        q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
        q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        q.device = dev;
        uint64_t count = 0;
        q.outputCount = &count;
        printf(">> calling ffxQuery(GET_VERSIONS) phase1...\n"); fflush(stdout);
        uint32_t qrc1 = pQuery(nullptr, &q.header);
        printf("phase1 rc=%u count=%llu\n", qrc1, (unsigned long long)count);
        if (qrc1 == FFX_API_RETURN_OK && count > 0 && count <= 16) {
            nVersions = (int)count;
            for (int i = 0; i < nVersions; ++i) versionNames[i] = nullptr;
            q.versionIds = versionIds;
            q.versionNames = versionNames;
            printf(">> calling ffxQuery(GET_VERSIONS) phase2...\n"); fflush(stdout);
            uint32_t qrc2 = pQuery(nullptr, &q.header);
            printf("phase2 rc=%u\n", qrc2);
        }
    }
    for (int i = 0; i < nVersions; ++i)
        printf("  version[%d]: id=0x%llx name=%s\n", i, (unsigned long long)versionIds[i], versionNames[i] ? versionNames[i] : "(null)");

    // Build the create-desc chain exactly like a game does:
    //   ffxCreateContextDescUpscale -> [ffxOverrideVersion] -> ffxCreateBackendDX12Desc(device) -> allocCallbacks
    static ffxCreateContextDescUpscale up{};
    up.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    up.flags = 0;
    up.maxRenderSize.width = 512;  up.maxRenderSize.height = 288;
    up.maxUpscaleSize.width = 1920; up.maxUpscaleSize.height = 1080;
    up.fpMessage = FfxMsg;

    static ffxOverrideVersion ov{};
    ov.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    if (nVersions > 0) ov.versionId = versionIds[0];   // use first available version

    static ffxCreateBackendDX12Desc be{};
    be.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    be.device = dev;   // <-- THE POINTER OUR PROXY SWAPS (here we pass it directly)

    g_allocDev = dev;
    // NOTE: we deliberately do NOT install custom allocation callbacks here — that is what a
    // real game does (and what our proxy will do). FFX then allocates its internal buffers on
    // the backend device with its own default allocator. The [ALLOC] logging above stays in
    // the code but is unused unless we re-enable ac below.

    if (nVersions > 0) {
        up.header.pNext = &ov.header;
        ov.header.pNext = &be.header;
    } else {
        up.header.pNext = &be.header;
    }
    be.header.pNext = nullptr;   // no allocation callbacks: use FFX's default allocator

    ffxContext ctx = nullptr;
    printf(">> calling ffxCreateContext (versions available: %d)...\n", nVersions); fflush(stdout);
    uint32_t rc = pCreate(&ctx, &up.header, nullptr);
    printf("ffxCreateContext on LUID 0x%X: rc=%u %s\n", lo, rc, rc == FFX_API_RETURN_OK ? "OK" : "FAILED");
    if (rc != FFX_API_RETURN_OK) { printf("FAIL: context creation failed\n"); return 1; }

    // Query the provider version of the created context
    ffxQueryGetProviderVersion pv{};
    pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    uint32_t qrc = pQuery(&ctx, &pv.header);
    printf("provider version query: rc=%u id=0x%llx name=%s\n", qrc, (unsigned long long)pv.versionId, pv.versionName ? pv.versionName : "(null)");

    // Destroy context (frees internal resources through our dealloc callback)
    uint32_t drc = pDestroy(&ctx, nullptr);
    printf("ffxDestroyContext: rc=%u\n", drc);

    dev->Release();
    FreeLibrary(hUp);
    if (rc == FFX_API_RETURN_OK && qrc == FFX_API_RETURN_OK && drc == FFX_API_RETURN_OK) {
        printf("PASS: real signed FSR4 upscaler bound to LUID-selected device\n");
        return 0;
    }
    printf("FAIL: see errors above\n");
    return 1;
}

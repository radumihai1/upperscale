// backend_probe.cpp — loads an FFX loader DLL from Cyberpunk's bin\x64 and asks it basic questions.
// Usage: backend_probe.exe <path-to-loader-dll> [adapter-index]
// Compares what a thin SDK loader vs the game's own stock loader answer for the same queries,
// so we can see exactly why FSR4 disappears from Cyberpunk's upscaler list in passthrough mode.
#include <windows.h>
#include "ffx_api.h"        // real SDK types (from third_party/FidelityFX-SDK)
#include "dx12/ffx_api_dx12.h"   // FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12 etc.
#include <stdio.h>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

typedef ffxReturnCode_t (*PfnFfxQuery)(ffxContext*, ffxApiHeader*);
typedef ffxReturnCode_t (*PfnFfxCreateContext)(ffxContext*, ffxApiHeader*, const void*);

int main(int argc, char** argv) {
    if (argc < 2) { printf("usage: backend_probe.exe <loader-dll> [adapter-index]\n"); return 2; }
    int adapterIdx = (argc > 2) ? atoi(argv[2]) : 0;

    HMODULE m = LoadLibraryA(argv[1]);
    if (!m) { printf("LoadLibrary(%s) FAILED err=%lu\n", argv[1], GetLastError()); return 1; }
    PfnFfxQuery q = (PfnFfxQuery)(void*)GetProcAddress(m, "ffxQuery");
    PfnFfxCreateContext c = (PfnFfxCreateContext)(void*)GetProcAddress(m, "ffxCreateContext");
    printf("loaded: %s\n  ffxQuery=%p ffxCreateContext=%p\n", argv[1], (void*)q, (void*)c);
    if (!q) { printf("no ffxQuery export\n"); return 1; }

    // ---- Query 1: GET_VERSIONS (type=0x4), null context — what Cyberpunk does at startup ----
    {
        ffxApiHeader qd{}; qd.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
        ffxReturnCode_t rc = q(nullptr, &qd);
        printf("ffxQuery(GET_VERSIONS=0x4) rc=%u  (0=OK 2=UNKNOWN_DESCTYPE 4=NO_PROVIDER)\n", rc);
    }

    // ---- Query 2: type 0x10002 — Cyberpunk's second startup query (custom/extended?) ----
    {
        ffxApiHeader qd{}; qd.type = 0x10002;
        ffxReturnCode_t rc = q(nullptr, &qd);
        printf("ffxQuery(type=0x10002)          rc=%u\n", rc);
    }

    // ---- Create a context on the chosen adapter with a backend-DX12 node ----
    IDXGIFactory6* f = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(__uuidof(IDXGIFactory6), (void**)&f))) {
        IDXGIAdapter1* ad = nullptr;
        if (SUCCEEDED(f->EnumAdapters1((UINT)adapterIdx, &ad))) {
            ID3D12Device* dev = nullptr;
            HRESULT hr = D3D12CreateDevice(ad, D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device), (void**)&dev);
            if (SUCCEEDED(hr)) {
                // Build the desc chain exactly like Cyberpunk does: top create-context node + backend dx12 node.
                struct BackendNode { ffxApiHeader h; ID3D12Device* device; } be{};
                struct TopNode { ffxApiHeader h; void* payload[8]; } top{};
                top.h.type = 0x20001;              // FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE (effect id upscale)
                top.h.pNext = &be.h;
                be.h.type = 0x10003;               // backend dx12 node type used by Cyberpunk's chain
                be.device = dev;

                ffxContext ctx = nullptr;
                ffxReturnCode_t crc = c ? c(&ctx, &top.h, nullptr) : 99;
                printf("ffxCreateContext(top=0x20001 + backend node 0x10003, adapter %d) rc=%u ctx=%p\n", adapterIdx, crc, (void*)ctx);

                // Try the FG context type too (0x30001 = frame generation effect id).
                top.h.type = 0x30001;
                ffxContext ctx2 = nullptr;
                ffxReturnCode_t crc2 = c ? c(&ctx2, &top.h, nullptr) : 99;
                printf("ffxCreateContext(top=0x30001 + backend node 0x10003, adapter %d) rc=%u ctx=%p\n", adapterIdx, crc2, (void*)ctx2);

                dev->Release();
            } else {
                printf("D3D12CreateDevice(adapter %d) FAILED hr=0x%08X\n", adapterIdx, (unsigned)hr);
            }
            ad->Release();
        } else {
            printf("EnumAdapters1(%d) failed\n", adapterIdx);
        }
        f->Release();
    }
    return 0;
}

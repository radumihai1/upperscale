// desc_probe2: which flag combos are legal on GPU A/B for Cyberpunk's exact MV texture shape?
// Failing in-game: dim=3 (TEXTURE2D) 600x1248 fmt=0x13 (R8G8B8A8_TYPELESS), all of UAV/RT+UAV/srcNoDS failed.
// Control that succeeded in-game: same size, fmt=0x1c (R16G16_UINT).
#include <d3d12.h>
#include <dxgi1_6.h>
#include <stdio.h>

static void TryCreate(ID3D12Device* dev, const char* gpu, UINT w, UINT h, DXGI_FORMAT fmt, D3D12_RESOURCE_FLAGS flags) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    d.Width = w; d.Height = h; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.Format = fmt;
    d.SampleDesc.Count = 1; d.SampleDesc.Quality = 0;
    d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    d.Alignment = 0;
    d.Flags = flags;
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    ID3D12Resource* r = nullptr;
    HRESULT hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&r));
    printf("%-6s %ux%u fmt=0x%X flags=0x%X  hr=0x%08X %s\n", gpu, w, h, (unsigned)fmt, (unsigned)flags,
           (unsigned)hr, SUCCEEDED(hr) ? "OK" : "FAIL");
    if (r) r->Release();
}

static ID3D12Device* MakeDev(IDXGIFactory6* f, LUID luid, const char** nameOut) {
    IDXGIAdapter1* a = nullptr;
    HRESULT hr = f->EnumAdapterByLuid(luid, __uuidof(IDXGIAdapter1), (void**)&a);
    if (FAILED(hr)) { printf("  EnumAdapterByLuid {%d,%d} failed 0x%08X\n", luid.HighPart, luid.LowPart, (unsigned)hr); return nullptr; }
    ID3D12Device* dev = nullptr;
    hr = D3D12CreateDevice(a, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev));
    a->Release();
    if (FAILED(hr)) { printf("  D3D12CreateDevice failed 0x%08X\n", (unsigned)hr); return nullptr; }
    *nameOut = "ok";
    return dev;
}

int main() {
    IDXGIFactory6* f = nullptr;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&f));
    if (FAILED(hr)) { printf("factory failed 0x%08X\n", (unsigned)hr); return 1; }

    // enumerate all adapters with their LUIDs first
    for (UINT i = 0; ; ++i) {
        IDXGIAdapter1* a = nullptr;
        if (f->EnumAdapters1(i, &a)) break;
        DXGI_ADAPTER_DESC ad{};
        a->GetDesc(&ad);
        wprintf(L"adapter[%u] LUID={%d,%d} %ls\n", i, ad.AdapterLuid.HighPart, ad.AdapterLuid.LowPart, ad.Description);
        a->Release();
    }

    // NOTE: LUID is {DWORD LowPart; LONG HighPart} — LowPart FIRST!
    LUID luidB = {0x27214, 0};   // RX 9060 XT (adapter[0])
    LUID luidA = {0x2A089, 0};   // RX 7900 XTX (adapter[2])
    const char* nm = "";
    ID3D12Device* devB = MakeDev(f, luidB, &nm);
    ID3D12Device* devA = MakeDev(f, luidA, &nm);

    DXGI_FORMAT fmtMV  = (DXGI_FORMAT)0x13;   // R32G8X24_TYPELESS — the failing one (depth family, typeless!)
    DXGI_FORMAT fmtCtl = (DXGI_FORMAT)0x1c;   // R8G8B8A8_UNORM_SRGB — control that worked in-game

    if (devB) {
        printf("=== GPU B (9060 XT) ===\n");
        TryCreate(devB, "B", 600, 1248, fmtMV, (D3D12_RESOURCE_FLAGS)0);
        TryCreate(devB, "B", 600, 1248, fmtMV, D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        TryCreate(devB, "B", 600, 1248, fmtMV, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
        TryCreate(devB, "B", 600, 1248, fmtMV, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
        TryCreate(devB, "B", 600, 1248, fmtMV, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        // typed sibling: D32_FLOAT_S8X24_UINT = 0x14
        TryCreate(devB, "B", 600, 1248, (DXGI_FORMAT)0x14, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
        TryCreate(devB, "B", 600, 1248, (DXGI_FORMAT)0x14, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
        TryCreate(devB, "B", 600, 1248, fmtCtl, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    } else printf("GPU B device failed\n");
    if (devA) {
        printf("=== GPU A (7900 XTX) ===\n");
        TryCreate(devA, "A", 600, 1248, fmtMV, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
        TryCreate(devA, "A", 600, 1248, fmtMV, (D3D12_RESOURCE_FLAGS)0);
    } else printf("GPU A device failed\n");

    if (devB) devB->Release();
    if (devA) devA->Release();
    f->Release();
    return 0;
}

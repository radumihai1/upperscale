// desc_probe: standalone D3D12 probe — which resource flag combos are legal on GPU B
// for the exact Cyberpunk FG PREPARE MV texture (R16G16_FLOAT TEXTURE3D 1024x768x1)?
#include <d3d12.h>
#include <dxgi1_6.h>
#include <stdio.h>

static void TryCreate(ID3D12Device* dev, const char* label, D3D12_RESOURCE_FLAGS flags) {
    D3D12_RESOURCE_DESC d{};
    d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE3D;
    d.Width = 1024; d.Height = 768; d.DepthOrArraySize = 1; d.MipLevels = 1;
    d.Format = DXGI_FORMAT_R16G16_FLOAT;
    d.SampleDesc.Count = 1;
    d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    d.Flags = flags;
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    ID3D12Resource* r = nullptr;
    HRESULT hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
        D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&r));
    printf("%-28s flags=0x%X  hr=0x%08X %s\n", label, (unsigned)flags, (unsigned)hr,
           SUCCEEDED(hr) ? "OK" : "FAIL");
    if (r) r->Release();
}

int main() {
    IDXGIFactory6* f = nullptr;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&f));
    if (FAILED(hr)) { printf("factory failed 0x%08X\n", (unsigned)hr); return 1; }
    for (UINT i = 0; ; ++i) {
        IDXGIAdapter1* a = nullptr;
        if (f->EnumAdapters1(i, &a)) break;
        DXGI_ADAPTER_DESC1 ad{}; a->GetDesc1(&ad);
        wprintf(L"[%u] %ls\n", i, ad.Description);
        a->Release();
    }
    // target LUID {0, 27214} = RX 9060 XT (GPU B)
    IDXGIAdapter1* target = nullptr;
    for (UINT i = 0; ; ++i) {
        IDXGIAdapter1* a = nullptr;
        if (f->EnumAdapters1(i, &a)) break;
        DXGI_ADAPTER_DESC ad0{};
        a->GetDesc(&ad0);
        if (ad0.AdapterLuid.LowPart == 0x27214 && ad0.AdapterLuid.HighPart == 0) { target = a; break; }
        a->Release();
    }
    if (!target) { printf("GPU B not found\n"); return 1; }
    ID3D12Device* dev = nullptr;
    hr = D3D12CreateDevice(target, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev));
    if (FAILED(hr)) { printf("device failed 0x%08X\n", (unsigned)hr); return 1; }

    TryCreate(dev, "UAV only", D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    TryCreate(dev, "RT+UAV (illegal?)", D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    TryCreate(dev, "DS+UAV (float fmt)", D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    TryCreate(dev, "none", (D3D12_RESOURCE_FLAGS)0);

    // also: 2D R16G16_FLOAT with RT+UAV (control — should be legal)
    {
        D3D12_RESOURCE_DESC d{};
        d.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        d.Width = 1024; d.Height = 768; d.DepthOrArraySize = 1; d.MipLevels = 1;
        d.Format = DXGI_FORMAT_R16G16_FLOAT; d.SampleDesc.Count = 1;
        d.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        d.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        ID3D12Resource* r = nullptr;
        HRESULT h2 = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
            D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&r));
        printf("%-28s flags=0x5  hr=0x%08X %s\n", "2D control RT+UAV", (unsigned)h2, SUCCEEDED(h2) ? "OK" : "FAIL");
        if (r) r->Release();
    }

    // and: what does the GAME's original resource look like? Probe GPU A (7900 XTX, LUID 0x2A089=172345)
    IDXGIAdapter1* targetA = nullptr;
    for (UINT i = 0; ; ++i) {
        IDXGIAdapter1* a = nullptr;
        if (f->EnumAdapters1(i, &a)) break;
        DXGI_ADAPTER_DESC ad0{};
        a->GetDesc(&ad0);
        if (ad0.AdapterLuid.LowPart == 0x2A089 && ad0.AdapterLuid.HighPart == 0) { targetA = a; break; }
        a->Release();
    }
    if (targetA) {
        ID3D12Device* devA = nullptr;
        hr = D3D12CreateDevice(targetA, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&devA));
        if (SUCCEEDED(hr)) {
            printf("--- same desc on GPU A (7900 XTX) ---\n");
            TryCreate(devA, "UAV only", D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            TryCreate(devA, "RT+UAV", D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
            devA->Release();
        } else printf("GPU A device failed 0x%08X\n", (unsigned)hr);
    }

    dev->Release(); target->Release(); f->Release();
    return 0;
}

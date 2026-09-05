// xgpu_probe5.cpp — Minimal isolation: which committed-resource configs work?
#include <d3d12.h>
#include <dxgi1_6.h>
#include <combaseapi.h>
#include <stdio.h>

static void Try(ID3D12Device* dev, const char* label, D3D12_HEAP_TYPE ht,
                D3D12_RESOURCE_DIMENSION dim, UINT w, DXGI_FORMAT fmt,
                D3D12_TEXTURE_LAYOUT layout, D3D12_RESOURCE_STATES st) {
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = ht;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = dim; rd.Width = w; rd.Height = 1; rd.DepthOrArraySize = 1;
    rd.MipLevels = 1; rd.Format = fmt; rd.Layout = layout; rd.SampleDesc.Count = 1;
    ID3D12Resource* r = nullptr;
    HRESULT hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, st, nullptr, IID_PPV_ARGS(&r));
    printf("  %-40s -> 0x%08X\n", label, (unsigned)hr);
    if (r) r->Release();
}

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    IDXGIFactory1* f = nullptr; CreateDXGIFactory1(IID_PPV_ARGS(&f));
    IDXGIAdapter1* a = nullptr; f->EnumAdapters1(1, &a); // 7900 XTX
    ID3D12Device* dev = nullptr; HRESULT hd = D3D12CreateDevice(a, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev));
    printf("device on adapter[1]: 0x%08X\n", (unsigned)hd);

    const char* htn[] = {"DEFAULT","UPLOAD","READBACK"};
    D3D12_HEAP_TYPE hts[] = {D3D12_HEAP_TYPE_DEFAULT, D3D12_HEAP_TYPE_UPLOAD, D3D12_HEAP_TYPE_READBACK};
    for (int i=0;i<3;i++){
        char lbl[64];
        snprintf(lbl,sizeof lbl,"buffer %s COMMON", htn[i]);
        Try(dev, lbl, hts[i], D3D12_RESOURCE_DIMENSION_BUFFER, 4096, DXGI_FORMAT_UNKNOWN, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_STATE_COMMON);
        snprintf(lbl,sizeof lbl,"buffer %s GENERIC_READ", htn[i]);
        Try(dev, lbl, hts[i], D3D12_RESOURCE_DIMENSION_BUFFER, 4096, DXGI_FORMAT_UNKNOWN, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_STATE_GENERIC_READ);
    }
    // texture controls
    Try(dev,"tex DEFAULT COPY_DEST", D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 64, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_STATE_COPY_DEST);
    Try(dev,"tex DEFAULT COMMON",   D3D12_HEAP_TYPE_DEFAULT, D3D12_RESOURCE_DIMENSION_TEXTURE2D, 64, DXGI_FORMAT_R8G8B8A8_UNORM, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_STATE_COMMON);
    // buffer with explicit SampleDesc + no layout (some drivers want Layout UNKNOWN for buffers)
    Try(dev,"buffer UPLOAD (Layout=UNKNOWN)", D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_DIMENSION_BUFFER, 4096, DXGI_FORMAT_UNKNOWN, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_STATE_COMMON);

    dev->Release(); a->Release(); f->Release();
    return 0;
}

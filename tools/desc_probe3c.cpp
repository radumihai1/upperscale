// desc_probe3c: which creation states are legal for a >1MB ROW_MAJOR texture on GPU A?
#include <d3d12.h>
#include <dxgi1_6.h>
#include <stdio.h>

int main() {
    IDXGIFactory6* f = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) return 1;
    LUID luidA = {0x2A089, 0};
    IDXGIAdapter1* a = nullptr;
    if (FAILED(f->EnumAdapterByLuid(luidA, __uuidof(IDXGIAdapter1), (void**)&a))) return 1;
    ID3D12Device* dev = nullptr;
    if (FAILED(D3D12CreateDevice(a, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev)))) { a->Release(); f->Release(); return 1; }
    a->Release();

    struct { const char* name; D3D12_RESOURCE_STATES st; DXGI_FORMAT fmt; D3D12_RESOURCE_FLAGS fl; } cases[] = {
        {"COMMON color",      D3D12_RESOURCE_STATE_COMMON, (DXGI_FORMAT)0x1c, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS},
        {"COPY_SRC color",    D3D12_RESOURCE_STATE_COPY_SOURCE, (DXGI_FORMAT)0x1c, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS},
        {"PSR|NPSR color",    (D3D12_RESOURCE_STATES)(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE), (DXGI_FORMAT)0x1c, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS},
        {"DEPTH_READ depth",  D3D12_RESOURCE_STATE_DEPTH_READ, (DXGI_FORMAT)0x13, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL},
        {"COMMON depth",      D3D12_RESOURCE_STATE_COMMON, (DXGI_FORMAT)0x13, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL},
    };
    for (auto& c : cases) {
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = 600; td.Height = 1248; td.DepthOrArraySize = 1; td.MipLevels = 1;
        td.Format = c.fmt; td.SampleDesc.Count = 1;
        td.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        td.Flags = c.fl;
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        ID3D12Resource* r = nullptr;
        HRESULT hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, c.st, nullptr, IID_PPV_ARGS(&r));
        printf("%-16s fmt=0x%X -> create 0x%08X %s\n", c.name, (unsigned)c.fmt, (unsigned)hr, SUCCEEDED(hr) ? "OK" : "FAIL");
        if (r) r->Release();
    }

    dev->Release(); f->Release();
    return 0;
}

// desc_probe3g: step-by-step HRESULTs for the exact in-game bounce path (UNKNOWN layout, >1MB).
#include <d3d12.h>
#include <dxgi1_6.h>
#include <stdio.h>

static ID3D12Device* MakeDev(IDXGIFactory6* f, LUID luid) {
    IDXGIAdapter1* a = nullptr;
    if (FAILED(f->EnumAdapterByLuid(luid, __uuidof(IDXGIAdapter1), (void**)&a))) return nullptr;
    ID3D12Device* dev = nullptr;
    HRESULT hr = D3D12CreateDevice(a, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev));
    a->Release();
    if (FAILED(hr)) return nullptr;
    return dev;
}

static void Step(ID3D12Device* dev, const char* tag, DXGI_FORMAT fmt, bool isDepth, D3D12_RESOURCE_STATES fromState) {
    printf("--- %s ---\n", tag);
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = 600; td.Height = 1248; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = fmt; td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;   // game's layout (layout=0 in logs)
    td.Flags = isDepth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL
                       : (D3D12_RESOURCE_FLAGS)(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    ID3D12Resource* tex = nullptr;
    HRESULT hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, fromState, nullptr, IID_PPV_ARGS(&tex));
    printf("  create(UNKNOWN layout) -> 0x%08X %s\n", (unsigned)hr, SUCCEEDED(hr) ? "OK" : "FAIL");
    if (FAILED(hr)) return;

    // what does GetDesc report back?
    D3D12_RESOURCE_DESC gd = tex->GetDesc();
    printf("  GetDesc: layout=%u align=%llu flags=0x%X\n", (unsigned)gd.Layout, (unsigned long long)gd.Alignment, (unsigned)gd.Flags);

    // GetCopyableFootprints — unchecked call in the proxy (returns void in this SDK)
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT rows[1]; UINT64 pitch[1], slice[1];
    dev->GetCopyableFootprints(&td, 0, 1, 0, &fp, rows, pitch, slice);
    printf("  GetCopyableFootprints: fmt=0x%X %ux%u rowPitch=%llu offset=%llu slice=%llu\n",
           (unsigned)fp.Footprint.Format, fp.Footprint.Width,
           fp.Footprint.Height, (unsigned long long)fp.Footprint.RowPitch, (unsigned long long)fp.Offset,
           (unsigned long long)slice[0]);

    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    // CRITICAL: size from the actual footprint slice (row-pitch padded), not W*H*bpp!
    bd.Width = (UINT)(slice[0] + 65536); bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; bd.SampleDesc.Count = 1;
    ID3D12Resource* rb = nullptr;
    hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb));
    printf("  readback buf create -> 0x%08X %s\n", (unsigned)hr, SUCCEEDED(hr) ? "OK" : "FAIL");
    if (FAILED(hr)) { tex->Release(); return; }

    ID3D12CommandAllocator* ca = nullptr;
    dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ca));
    ID3D12GraphicsCommandList* cl = nullptr;
    dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca, nullptr, IID_PPV_ARGS(&cl));
    hr = cl->Reset(ca, nullptr);
    printf("  Reset -> 0x%08X\n", (unsigned)hr);

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = tex;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = fromState;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cl->ResourceBarrier(1, &b);

    {
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = rb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset = fp.Offset;
        dst.PlacedFootprint.Footprint.Format = fmt;
        dst.PlacedFootprint.Footprint.Width = fp.Footprint.Width;
        dst.PlacedFootprint.Footprint.Height = fp.Footprint.Height;
        dst.PlacedFootprint.Footprint.Depth = 1;
        dst.PlacedFootprint.Footprint.RowPitch = fp.Footprint.RowPitch;
        D3D12_TEXTURE_COPY_LOCATION src{};
        src.pResource = tex; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        src.SubresourceIndex = 0;
        cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter = fromState;
    cl->ResourceBarrier(1, &b);

    hr = cl->Close();
    printf("  Close -> 0x%08X %s\n", (unsigned)hr, SUCCEEDED(hr) ? "OK" : "FAIL");
    cl->Release(); ca->Release(); rb->Release(); tex->Release();
}

int main() {
    IDXGIFactory6* f = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) return 1;
    LUID luidA = {0x2A089, 0};
    ID3D12Device* dev = MakeDev(f, luidA);
    if (!dev) { printf("no GPU A\n"); f->Release(); return 1; }

    D3D12_RESOURCE_STATES psrNpsr = (D3D12_RESOURCE_STATES)(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Step(dev, "color RT|UAV from RENDER_TARGET", (DXGI_FORMAT)0x1c, false, D3D12_RESOURCE_STATE_RENDER_TARGET);
    Step(dev, "depth DS from DEPTH_READ",        (DXGI_FORMAT)0x13, true,  D3D12_RESOURCE_STATE_DEPTH_READ);
    Step(dev, "depth DS from PSR|NPSR",          (DXGI_FORMAT)0x13, true,  psrNpsr);

    dev->Release(); f->Release();
    return 0;
}

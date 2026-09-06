// desc_probe3d: isolate the >1MB UNKNOWN-layout copy failure seen in-game (clA Close E_INVALIDARG).
// Tests on GPU A with 600x1248 textures (>1MB):
//   T1: UNKNOWN layout, created directly in COPY_SOURCE, no barriers, CopyTextureRegion -> Close?
//   T2: UNKNOWN layout, created in PSR|NPSR, barrier to COPY_SOURCE, copy, barrier back -> Close?
//   T3: ROW_MAJOR layout, created in COMMON, barrier to COPY_SOURCE, copy, barrier back -> Close?
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

static void DoTest(ID3D12Device* dev, const char* tag, DXGI_FORMAT fmt, D3D12_TEXTURE_LAYOUT layout,
                   D3D12_RESOURCE_STATES createState, bool useBarrier) {
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = 600; td.Height = 1248; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = fmt; td.SampleDesc.Count = 1;
    td.Layout = layout;
    bool isDepth = ((UINT)fmt >= 0x13 && (UINT)fmt <= 0x16);
    td.Flags = isDepth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL
                       : (D3D12_RESOURCE_FLAGS)(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    ID3D12Resource* tex = nullptr;
    HRESULT hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, createState, nullptr, IID_PPV_ARGS(&tex));
    if (FAILED(hr)) { printf("%s: create FAIL 0x%08X\n", tag, (unsigned)hr); return; }

    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = 600 * 1248 * 4 + 65536; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; bd.SampleDesc.Count = 1;
    ID3D12Resource* rb = nullptr;
    hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb));
    if (FAILED(hr)) { printf("%s: buf create FAIL 0x%08X\n", tag, (unsigned)hr); tex->Release(); return; }

    ID3D12CommandAllocator* ca = nullptr;
    dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ca));
    ID3D12GraphicsCommandList* cl = nullptr;
    dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca, nullptr, IID_PPV_ARGS(&cl));

    if (useBarrier) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = tex;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = createState;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        cl->ResourceBarrier(1, &b);
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT rows[1]; UINT64 pitch[1], slice[1];
    dev->GetCopyableFootprints(&td, 0, 1, 0, &fp, rows, pitch, slice);
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = rb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = fp.Offset;
    dst.PlacedFootprint.Footprint.Format = (isDepth ? fmt : fmt); // pass-through like proxy's CopyFormatFor for 0x1c/0x13
    dst.PlacedFootprint.Footprint.Width = fp.Footprint.Width;
    dst.PlacedFootprint.Footprint.Height = fp.Footprint.Height;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = fp.Footprint.RowPitch;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = tex; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    if (useBarrier) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = tex;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter = createState;
        cl->ResourceBarrier(1, &b);
    }

    hr = cl->Close();
    printf("%s: Close 0x%08X %s\n", tag, (unsigned)hr, SUCCEEDED(hr) ? "OK" : "FAIL");
    cl->Release(); ca->Release(); rb->Release(); tex->Release();
}

int main() {
    IDXGIFactory6* f = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) return 1;
    LUID luidA = {0x2A089, 0};
    ID3D12Device* dev = MakeDev(f, luidA);
    if (!dev) { printf("no GPU A\n"); f->Release(); return 1; }

    D3D12_RESOURCE_STATES psrNpsr = (D3D12_RESOURCE_STATES)(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    DXGI_FORMAT color = (DXGI_FORMAT)0x1c;   // R8G8B8A8_UNORM_SRGB — the game's FG input

    DoTest(dev, "T1 UNKNOWN+COPY_SRC-direct", color, D3D12_TEXTURE_LAYOUT_UNKNOWN, D3D12_RESOURCE_STATE_COPY_SOURCE, false);
    DoTest(dev, "T2 UNKNOWN+PSR|NPSR+barrier", color, D3D12_TEXTURE_LAYOUT_UNKNOWN, psrNpsr, true);
    DoTest(dev, "T3 ROW_MAJOR+COMMON+barrier", color, D3D12_TEXTURE_LAYOUT_ROW_MAJOR, D3D12_RESOURCE_STATE_COMMON, true);

    dev->Release(); f->Release();
    return 0;
}

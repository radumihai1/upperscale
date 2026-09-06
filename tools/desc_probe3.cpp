// desc_probe3: reproduce the in-game clA Close() failure on GPU A/B.
// Hypothesis: transitioning a depth-family (fmt 0x13) texture into PSR|NPSR and/or out of it
// is rejected at Close() time by this AMD driver; DEPTH_READ works instead.
#include <d3d12.h>
#include <dxgi1_6.h>
#include <stdio.h>

static ID3D12Device* MakeDev(IDXGIFactory6* f, LUID luid) {
    IDXGIAdapter1* a = nullptr;
    if (FAILED(f->EnumAdapterByLuid(luid, __uuidof(IDXGIAdapter1), (void**)&a))) return nullptr;
    ID3D12Device* dev = nullptr;
    HRESULT hr = D3D12CreateDevice(a, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev));
    a->Release();
    if (FAILED(hr)) { printf("  device failed 0x%08X\n", (unsigned)hr); return nullptr; }
    return dev;
}

// Record: barrier(tex, from -> COPY_SOURCE), CopyTextureRegion to readback buffer, barrier back.
static HRESULT TestBounce(ID3D12Device* dev, const char* gpu, DXGI_FORMAT fmt, D3D12_RESOURCE_STATES fromState) {
    // texture with DS flag (like the game's depth resource)
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = 600; td.Height = 1248; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = fmt; td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    ID3D12Resource* tex = nullptr;
    HRESULT hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, fromState, nullptr, IID_PPV_ARGS(&tex));
    if (FAILED(hr)) { printf("%s fmt=0x%X create failed 0x%08X\n", gpu, (unsigned)fmt, (unsigned)hr); return hr; }

    // readback buffer
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = 600 * 1248 * 4 + 65536; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; bd.SampleDesc.Count = 1;
    ID3D12Resource* rb = nullptr;
    hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb));
    if (FAILED(hr)) { printf("%s fmt=0x%X rb create failed 0x%08X\n", gpu, (unsigned)fmt, (unsigned)hr); tex->Release(); return hr; }

    // command allocator + list
    ID3D12CommandAllocator* ca = nullptr;
    dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ca));
    ID3D12GraphicsCommandList* cl = nullptr;
    dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca, nullptr, IID_PPV_ARGS(&cl));

    // barrier from -> COPY_SOURCE
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = tex;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = fromState;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cl->ResourceBarrier(1, &b);

    // copy to placed footprint (row-major), using the same format mapping as the proxy
    DXGI_FORMAT copyFmt = fmt;   // 0x13 passes through in CopyFormatFor
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT rows[1]; UINT64 pitch[1], slice[1];
    dev->GetCopyableFootprints(&td, 0, 1, 0, &fp, rows, pitch, slice);
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = rb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = fp.Offset;
    dst.PlacedFootprint.Footprint.Format = copyFmt;
    dst.PlacedFootprint.Footprint.Width = fp.Footprint.Width;
    dst.PlacedFootprint.Footprint.Height = fp.Footprint.Height;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = fp.Footprint.RowPitch;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = tex; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    // barrier back COPY_SOURCE -> fromState
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter = fromState;
    cl->ResourceBarrier(1, &b);

    hr = cl->Close();
    const char* stName = (fromState == (D3D12_RESOURCE_STATES)(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)) ? "PSR|NPSR" :
                         (fromState == D3D12_RESOURCE_STATE_DEPTH_READ) ? "DEPTH_READ" : "?";
    printf("%s fmt=0x%X bounce via %-8s -> Close hr=0x%08X %s\n", gpu, (unsigned)fmt, stName, (unsigned)hr, SUCCEEDED(hr) ? "OK" : "FAIL");

    cl->Release(); ca->Release(); rb->Release(); tex->Release();
    return hr;
}

int main() {
    IDXGIFactory6* f = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) return 1;
    LUID luidA = {0x2A089, 0};   // LowPart first!
    LUID luidB = {0x27214, 0};
    ID3D12Device* devA = MakeDev(f, luidA);
    ID3D12Device* devB = MakeDev(f, luidB);

    DXGI_FORMAT fmtDepth = (DXGI_FORMAT)0x13;   // the game's depth-family typeless
    D3D12_RESOURCE_STATES psrNpsr = (D3D12_RESOURCE_STATES)(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

    if (devA) {
        printf("=== GPU A (7900 XTX) ===\n");
        TestBounce(devA, "A", fmtDepth, psrNpsr);
        TestBounce(devA, "A", fmtDepth, D3D12_RESOURCE_STATE_DEPTH_READ);
    } else printf("GPU A unavailable\n");
    if (devB) {
        printf("=== GPU B (9060 XT) ===\n");
        TestBounce(devB, "B", fmtDepth, psrNpsr);
        TestBounce(devB, "B", fmtDepth, D3D12_RESOURCE_STATE_DEPTH_READ);
    } else printf("GPU B unavailable\n");

    if (devA) devA->Release();
    if (devB) devB->Release();
    f->Release();
    return 0;
}

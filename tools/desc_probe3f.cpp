// desc_probe3f: exact in-game paths + recovery test.
// P1 = v8's successful FG path (color 0x1c RT|UAV, state RENDER_TARGET) — MUST be OK to validate probe fidelity.
// P2 = PREPARE depth with PSR|NPSR (current proxy behavior) — expected FAIL.
// P3 = PREPARE depth with DEPTH_READ (proposed fix) — expected OK.
// P4 = after a failed Close, does Reset+record+Close recover? (the v10 cascade question)
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

static ID3D12Resource* MakeTex(ID3D12Device* dev, DXGI_FORMAT fmt, bool isDepth, D3D12_RESOURCE_STATES st) {
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = 600; td.Height = 1248; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = fmt; td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;   // game's layout (layout=0 in logs)
    td.Flags = isDepth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL
                       : (D3D12_RESOURCE_FLAGS)(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    ID3D12Resource* r = nullptr;
    HRESULT hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, st, nullptr, IID_PPV_ARGS(&r));
    if (FAILED(hr)) { printf("  tex create FAIL 0x%08X\n", (unsigned)hr); return nullptr; }
    return r;
}

static ID3D12Resource* MakeRb(ID3D12Device* dev) {
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = 600 * 1248 * 4 + 65536; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; bd.SampleDesc.Count = 1;
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_READBACK;   // proxy uses READBACK for rbA!
    ID3D12Resource* r = nullptr;
    HRESULT hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&r));
    if (FAILED(hr)) { printf("  rb create FAIL 0x%08X\n", (unsigned)hr); return nullptr; }
    return r;
}

// Record the proxy's exact bounce sequence for one input and Close. Returns Close hr.
static HRESULT BounceAndClose(ID3D12Device* dev, ID3D12CommandAllocator* ca, ID3D12GraphicsCommandList* cl,
                              ID3D12Resource* tex, DXGI_FORMAT fmt, D3D12_RESOURCE_STATES fromState) {
    D3D12_RESOURCE_DESC td = tex->GetDesc();
    HRESULT hr = cl->Reset(ca, nullptr);
    if (FAILED(hr)) printf("  Reset -> 0x%08X (quirk)\n", (unsigned)hr);

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = tex;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = fromState;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cl->ResourceBarrier(1, &b);

    ID3D12Resource* rb = MakeRb(dev);
    if (!rb) return E_FAIL;
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT rows[1]; UINT64 pitch[1], slice[1];
    dev->GetCopyableFootprints(&td, 0, 1, 0, &fp, rows, pitch, slice);
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = rb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = fp.Offset;
    dst.PlacedFootprint.Footprint.Format = fmt;   // pass-through like proxy CopyFormatFor for 0x1c/0x13
    dst.PlacedFootprint.Footprint.Width = fp.Footprint.Width;
    dst.PlacedFootprint.Footprint.Height = fp.Footprint.Height;
    dst.PlacedFootprint.Footprint.Depth = 1;
    dst.PlacedFootprint.Footprint.RowPitch = fp.Footprint.RowPitch;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = tex; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    src.SubresourceIndex = 0;
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter = fromState;
    cl->ResourceBarrier(1, &b);

    hr = cl->Close();
    rb->Release();
    return hr;
}

int main() {
    IDXGIFactory6* f = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) return 1;
    LUID luidA = {0x2A089, 0};
    ID3D12Device* dev = MakeDev(f, luidA);
    if (!dev) { printf("no GPU A\n"); f->Release(); return 1; }

    D3D12_RESOURCE_STATES psrNpsr = (D3D12_RESOURCE_STATES)(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    DXGI_FORMAT color = (DXGI_FORMAT)0x1c;   // R8G8B8A8_UNORM
    DXGI_FORMAT depth = (DXGI_FORMAT)0x13;   // R32G8X24_TYPELESS

    ID3D12CommandAllocator* ca = nullptr;
    dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ca));
    ID3D12GraphicsCommandList* cl = nullptr;
    dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca, nullptr, IID_PPV_ARGS(&cl));

    // P1: v8's successful FG path — color in RENDER_TARGET
    {
        ID3D12Resource* tex = MakeTex(dev, color, false, D3D12_RESOURCE_STATE_RENDER_TARGET);
        if (tex) {
            HRESULT hr = BounceAndClose(dev, ca, cl, tex, color, D3D12_RESOURCE_STATE_RENDER_TARGET);
            printf("P1 color RT bounce Close -> 0x%08X %s\n", (unsigned)hr, SUCCEEDED(hr) ? "OK" : "FAIL");
            tex->Release();
        }
    }
    // P2: depth with PSR|NPSR (current proxy behavior for FFX state 0xC)
    {
        ID3D12Resource* tex = MakeTex(dev, depth, true, psrNpsr);   // creation in PSR|NPSR may itself fail — that's informative too
        if (tex) {
            HRESULT hr = BounceAndClose(dev, ca, cl, tex, depth, psrNpsr);
            printf("P2 depth PSR|NPSR bounce Close -> 0x%08X %s\n", (unsigned)hr, SUCCEEDED(hr) ? "OK" : "FAIL");
            tex->Release();
        } else {
            // try creating in COMMON then barriering to PSR|NPSR? No — just note creation failed.
            printf("P2 depth create-in-PSR|NPSR failed (expected: illegal state for DS resource)\n");
        }
    }
    // P3: depth with DEPTH_READ (proposed fix)
    {
        ID3D12Resource* tex = MakeTex(dev, depth, true, D3D12_RESOURCE_STATE_DEPTH_READ);
        if (tex) {
            HRESULT hr = BounceAndClose(dev, ca, cl, tex, depth, D3D12_RESOURCE_STATE_DEPTH_READ);
            printf("P3 depth DEPTH_READ bounce Close -> 0x%08X %s\n", (unsigned)hr, SUCCEEDED(hr) ? "OK" : "FAIL");
            tex->Release();
        }
    }
    // P4: recovery — after the last (possibly failed) Close, Reset+record+Close again on same list
    {
        ID3D12Resource* tex = MakeTex(dev, color, false, D3D12_RESOURCE_STATE_RENDER_TARGET);
        if (tex) {
            HRESULT hr = BounceAndClose(dev, ca, cl, tex, color, D3D12_RESOURCE_STATE_RENDER_TARGET);
            printf("P4 post-failure recovery Close -> 0x%08X %s\n", (unsigned)hr, SUCCEEDED(hr) ? "OK" : "FAIL");
            tex->Release();
        }
    }

    cl->Release(); ca->Release(); dev->Release(); f->Release();
    return 0;
}

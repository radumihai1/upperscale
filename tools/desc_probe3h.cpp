// desc_probe3h: matrix for bouncing a DS-flagged depth-family texture (fmt 0x13) to a buffer on GPU A.
// Variables: from-state {DEPTH_READ, PSR|NPSR, COMMON(no barrier)} x dst-footprint fmt {0x13, R32_FLOAT(0x29), 0x14}
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

static void Cell(ID3D12Device* dev, const char* tag, D3D12_RESOURCE_STATES fromState, bool useBarrier, DXGI_FORMAT dstFmt) {
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = 600; td.Height = 1248; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = (DXGI_FORMAT)0x13; td.SampleDesc.Count = 1;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    ID3D12Resource* tex = nullptr;
    HRESULT hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, fromState, nullptr, IID_PPV_ARGS(&tex));
    if (FAILED(hr)) { printf("%-28s create FAIL 0x%08X\n", tag, (unsigned)hr); return; }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT rows[1]; UINT64 pitch[1], slice[1];
    dev->GetCopyableFootprints(&td, 0, 1, 0, &fp, rows, pitch, slice);

    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = (UINT)(slice[0] + 65536); bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; bd.SampleDesc.Count = 1;
    ID3D12Resource* rb = nullptr;
    hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb));
    if (FAILED(hr)) { printf("%-28s buf FAIL\n", tag); tex->Release(); return; }

    ID3D12CommandAllocator* ca = nullptr;
    dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ca));
    ID3D12GraphicsCommandList* cl = nullptr;
    dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca, nullptr, IID_PPV_ARGS(&cl));
    cl->Reset(ca, nullptr);   // fresh-list quirk hr ignored

    if (useBarrier) {
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = tex;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = fromState;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        cl->ResourceBarrier(1, &b);
    }

    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = rb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = fp.Offset;
    dst.PlacedFootprint.Footprint.Format = dstFmt;
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
        b.Transition.StateAfter = fromState;
        cl->ResourceBarrier(1, &b);
    }

    hr = cl->Close();
    printf("%-28s -> Close 0x%08X %s\n", tag, (unsigned)hr, SUCCEEDED(hr) ? "OK" : "FAIL");
    cl->Release(); ca->Release(); rb->Release(); tex->Release();
}

int main() {
    IDXGIFactory6* f = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) return 1;
    LUID luidA = {0x2A089, 0};
    ID3D12Device* dev = MakeDev(f, luidA);
    if (!dev) { printf("no GPU A\n"); f->Release(); return 1; }

    D3D12_RESOURCE_STATES psrNpsr = (D3D12_RESOURCE_STATES)(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    DXGI_FORMAT t0x13 = (DXGI_FORMAT)0x13;      // typeless depth family
    DXGI_FORMAT r32f  = DXGI_FORMAT_R32_FLOAT;  // 0x29 typed view
    DXGI_FORMAT t0x14 = (DXGI_FORMAT)0x14;      // D32_FLOAT_S8X24_UINT

    Cell(dev, "DEPTH_READ + dst=0x13",   D3D12_RESOURCE_STATE_DEPTH_READ, true,  t0x13);
    Cell(dev, "DEPTH_READ + dst=R32F",   D3D12_RESOURCE_STATE_DEPTH_READ, true,  r32f);
    Cell(dev, "DEPTH_READ + dst=0x14",   D3D12_RESOURCE_STATE_DEPTH_READ, true,  t0x14);
    Cell(dev, "PSR|NPSR + dst=R32F",     psrNpsr,                          true,  r32f);
    Cell(dev, "COMMON(nobar) + dst=0x13",D3D12_RESOURCE_STATE_COMMON,      false, t0x13);
    Cell(dev, "COMMON(nobar) + dst=R32F",D3D12_RESOURCE_STATE_COMMON,      false, r32f);

    dev->Release(); f->Release();
    return 0;
}

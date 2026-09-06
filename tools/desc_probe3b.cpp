// desc_probe3b: isolate the invalid CopyTextureRegion for Cyberpunk's typeless depth texture.
// Matrix: (source fmt, source state, dst-footprint fmt) -> Close() hr on GPU A and B.
#include <d3d12.h>
#include <dxgi1_6.h>
#include <stdio.h>

static ID3D12Device* MakeDev(IDXGIFactory6* f, LUID luid) {
    IDXGIAdapter1* a = nullptr;
    if (FAILED(f->EnumAdapterByLuid(luid, __uuidof(IDXGIAdapter1), (void**)&a)) ) return nullptr;
    ID3D12Device* dev = nullptr;
    HRESULT hr = D3D12CreateDevice(a, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev));
    a->Release();
    if (FAILED(hr)) return nullptr;
    return dev;
}

static HRESULT TestCopy(ID3D12Device* dev, const char* gpu, DXGI_FORMAT srcFmt, D3D12_RESOURCE_STATES fromState, DXGI_FORMAT dstFmt) {
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = 600; td.Height = 1248; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = srcFmt; td.SampleDesc.Count = 1;
    // >1MB textures MUST be ROW_MAJOR (D3D12 rule) — the game's resources are too.
    td.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    // depth-family needs DS flag; color needs RT+UAV (like the game's resources)
    bool isDepth = ((UINT)srcFmt >= 0x13 && (UINT)srcFmt <= 0x16) || (UINT)srcFmt == 40 || (UINT)srcFmt == 55;
    td.Flags = isDepth ? D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL
                       : (D3D12_RESOURCE_FLAGS)(D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS);
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    ID3D12Resource* tex = nullptr;
    HRESULT hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, fromState, nullptr, IID_PPV_ARGS(&tex));
    if (FAILED(hr)) { printf("%s src=0x%X state=%d create FAIL 0x%08X\n", gpu, (unsigned)srcFmt, (int)fromState, (unsigned)hr); return hr; }

    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = 600 * 1248 * 4 + 65536; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; bd.SampleDesc.Count = 1;
    ID3D12Resource* rb = nullptr;
    hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb));
    if (FAILED(hr)) { tex->Release(); return hr; }

    ID3D12CommandAllocator* ca = nullptr;
    dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ca));
    ID3D12GraphicsCommandList* cl = nullptr;
    dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca, nullptr, IID_PPV_ARGS(&cl));

    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = tex;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = fromState;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cl->ResourceBarrier(1, &b);

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
    UINT rows[1]; UINT64 pitch[1], slice[1];
    dev->GetCopyableFootprints(&td, 0, 1, 0, &fp, rows, pitch, slice);
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

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter = fromState;
    cl->ResourceBarrier(1, &b);

    hr = cl->Close();
    const char* stName = (fromState == (D3D12_RESOURCE_STATES)(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)) ? "PSR|NPSR" :
                         (fromState == D3D12_RESOURCE_STATE_DEPTH_READ) ? "DEPTH_RD" : "?";
    printf("%s src=0x%X %-8s dstFmt=0x%X -> Close 0x%08X %s\n", gpu, (unsigned)srcFmt, stName, (unsigned)dstFmt, (unsigned)hr, SUCCEEDED(hr) ? "OK" : "FAIL");

    cl->Release(); ca->Release(); rb->Release(); tex->Release();
    return hr;
}

int main() {
    IDXGIFactory6* f = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) return 1;
    LUID luidA = {0x2A089, 0};
    ID3D12Device* devA = MakeDev(f, luidA);

    // ---- MINIMAL CONTROLS on GPU A ----
    if (devA) {
        printf("=== controls ===\n");
        // C1: empty command list close
        {
            ID3D12CommandAllocator* ca = nullptr;
            devA->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ca));
            ID3D12GraphicsCommandList* cl = nullptr;
            HRESULT hr0 = devA->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca, nullptr, IID_PPV_ARGS(&cl));
            printf("C1 CreateCommandList 0x%08X\n", (unsigned)hr0);
            if (SUCCEEDED(hr0)) {
                HRESULT hr = cl->Close();
                printf("C1 empty Close -> 0x%08X %s\n", (unsigned)hr, SUCCEEDED(hr) ? "OK" : "FAIL");
                cl->Release();
            }
            ca->Release();
        }
        // C2: color tex created in COPY_SOURCE directly, copy to buffer, no barriers
        {
            D3D12_RESOURCE_DESC td{};
            td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            td.Width = 64; td.Height = 64; td.DepthOrArraySize = 1; td.MipLevels = 1;
            td.Format = (DXGI_FORMAT)0x1c; td.SampleDesc.Count = 1;
            td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
            D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
            ID3D12Resource* tex = nullptr;
            HRESULT hr = devA->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_SOURCE, nullptr, IID_PPV_ARGS(&tex));
            printf("C2 tex create 0x%08X\n", (unsigned)hr);
            if (SUCCEEDED(hr)) {
                D3D12_RESOURCE_DESC bd{};
                bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
                bd.Width = 64 * 64 * 4 + 65536; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
                bd.Format = DXGI_FORMAT_UNKNOWN; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; bd.SampleDesc.Count = 1;
                ID3D12Resource* rb = nullptr;
                hr = devA->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb));
                printf("C2 buf create 0x%08X\n", (unsigned)hr);
                if (SUCCEEDED(hr)) {
                    ID3D12CommandAllocator* ca = nullptr;
                    devA->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ca));
                    ID3D12GraphicsCommandList* cl = nullptr;
                    devA->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, ca, nullptr, IID_PPV_ARGS(&cl));
                    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp{};
                    UINT rows[1]; UINT64 pitch[1], slice[1];
                    devA->GetCopyableFootprints(&td, 0, 1, 0, &fp, rows, pitch, slice);
                    printf("C2 footprint fmt=0x%X %ux%u rowPitch=%llu offset=%llu\n", (unsigned)fp.Footprint.Format, fp.Footprint.Width, fp.Footprint.Height, (unsigned long long)fp.Footprint.RowPitch, (unsigned long long)fp.Offset);
                    D3D12_TEXTURE_COPY_LOCATION dst{};
                    dst.pResource = rb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                    dst.PlacedFootprint.Offset = fp.Offset;
                    dst.PlacedFootprint.Footprint.Format = (DXGI_FORMAT)0x1c;
                    dst.PlacedFootprint.Footprint.Width = fp.Footprint.Width;
                    dst.PlacedFootprint.Footprint.Height = fp.Footprint.Height;
                    dst.PlacedFootprint.Footprint.Depth = 1;
                    dst.PlacedFootprint.Footprint.RowPitch = fp.Footprint.RowPitch;
                    D3D12_TEXTURE_COPY_LOCATION src{};
                    src.pResource = tex; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                    src.SubresourceIndex = 0;
                    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                    hr = cl->Close();
                    printf("C2 no-barrier copy Close -> 0x%08X %s\n", (unsigned)hr, SUCCEEDED(hr) ? "OK" : "FAIL");
                    cl->Release(); ca->Release(); rb->Release(); tex->Release();
                }
            }
        }
    }

    D3D12_RESOURCE_STATES psrNpsr = (D3D12_RESOURCE_STATES)(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    DXGI_FORMAT c0x1c = (DXGI_FORMAT)0x1c;   // R8G8B8A8_UNORM_SRGB (color baseline)
    DXGI_FORMAT d0x13 = (DXGI_FORMAT)0x13;   // R32G8X24_TYPELESS (the game's depth)
    DXGI_FORMAT r32f  = DXGI_FORMAT_R32_FLOAT;

    if (devA) {
        printf("=== GPU A ===\n");
        TestCopy(devA, "A", c0x1c, psrNpsr, c0x1c);          // baseline color — must be OK
        TestCopy(devA, "A", d0x13, D3D12_RESOURCE_STATE_DEPTH_READ, d0x13);   // typeless dst fmt
        TestCopy(devA, "A", d0x13, D3D12_RESOURCE_STATE_DEPTH_READ, r32f);    // typed R32_FLOAT dst fmt
        TestCopy(devA, "A", d0x13, psrNpsr, r32f);           // PSR|NPSR + typed dst
    } else printf("GPU A unavailable\n");

    if (devA) devA->Release();
    f->Release();
    return 0;
}

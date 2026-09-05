// barrier_test4.cpp — replicate the EXACT input-bounce pattern that fails at Close:
// fresh list (Reset E_FAIL ignored), texture created in COPY_DEST, readback buffer in
// COPY_DEST, transition tex -> COPY_SOURCE, T2T copy into PLACED_FOOTPRINT, back.
//  (a) with StateBefore/After = 0xC0 (PIXEL_COMPUTE_READ — what FFX state 0xc maps to)
//  (b) same but GENERIC_READ
//  (c) same as (a) but texture created in COMMON(0) instead of COPY_DEST
#include <d3d12.h>
#include <dxgi1_6.h>
#include <stdio.h>
#include <stdlib.h>

static void runCase(ID3D12Device* dev, const char* name, D3D12_RESOURCE_STATES stVal,
                    D3D12_RESOURCE_STATES texInit) {
    ID3D12CommandAllocator* al = nullptr; ID3D12GraphicsCommandList* cl = nullptr;
    if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&al)))) return;
    if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, al, nullptr, IID_PPV_ARGS(&cl)))) { al->Release(); return; }

    // texture 512x288 RGBA16F in DEFAULT heap
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = 512; td.Height = 288; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R16G16B16A16_FLOAT; td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN; td.SampleDesc.Count = 1;
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    ID3D12Resource* tex = nullptr;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, texInit, nullptr, IID_PPV_ARGS(&tex)))) { cl->Release(); al->Release(); return; }

    // readback buffer 512*288*8 in COPY_DEST
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = (UINT64)512 * 288 * 8; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; bd.SampleDesc.Count = 1;
    D3D12_HEAP_PROPERTIES hpR{}; hpR.Type = D3D12_HEAP_TYPE_READBACK;
    ID3D12Resource* rb = nullptr;
    if (FAILED(dev->CreateCommittedResource(&hpR, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rb)))) { tex->Release(); cl->Release(); al->Release(); return; }

    HRESULT hrR = cl->Reset(al, nullptr);   // expect E_FAIL on fresh list — ignore (known quirk)
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = tex;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = stVal;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cl->ResourceBarrier(1, &b);

    // T2T copy: rb PLACED_FOOTPRINT <- tex subresource 0 (row pitch 512*8)
    D3D12_TEXTURE_COPY_LOCATION dst{};
    dst.pResource = rb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    dst.PlacedFootprint.Offset = 0;
    dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    dst.PlacedFootprint.Footprint.Width = 512; dst.PlacedFootprint.Footprint.Height = 288;
    dst.PlacedFootprint.Footprint.Depth = 1; dst.PlacedFootprint.Footprint.RowPitch = 512 * 8;
    D3D12_TEXTURE_COPY_LOCATION src{};
    src.pResource = tex; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);

    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b.Transition.StateAfter = stVal;
    cl->ResourceBarrier(1, &b);

    HRESULT hrC = cl->Close();
    printf("%s: Reset=0x%08X Close=0x%08X\n", name, (unsigned)hrR, (unsigned)hrC);
    rb->Release(); tex->Release(); cl->Release(); al->Release();
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    UINT idx = (argc > 1) ? (UINT)atoi(argv[1]) : 0;
    IDXGIFactory1* f = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) return 1;
    IDXGIAdapter1* a = nullptr;
    if (FAILED(f->EnumAdapters1(idx, &a))) { printf("no adapter %u\n", idx); return 1; }
    DXGI_ADAPTER_DESC1 ad{}; a->GetDesc1(&ad);
    char an[512]{}; WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, an, sizeof(an), nullptr, nullptr);
    printf("adapter %u: %s\n", idx, an);
    ID3D12Device* dev = nullptr;
    if (FAILED(D3D12CreateDevice(a, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev)))) return 1;

    runCase(dev, "(a) bounce pattern st=0xC0 texInit=COPY_DEST", (D3D12_RESOURCE_STATES)0xC0u, D3D12_RESOURCE_STATE_COPY_DEST);
    runCase(dev, "(b) bounce pattern st=GENERIC_READ            ", D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_COPY_DEST);
    runCase(dev, "(c) bounce pattern st=0xC0 texInit=COMMON(0)  ", (D3D12_RESOURCE_STATES)0xC0u, (D3D12_RESOURCE_STATES)0u);
    return 0;
}

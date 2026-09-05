// barrier_test2.cpp — isolate: does Close fail only when the SAME list is Reset multiple times?
// Each scenario gets its OWN allocator + command list (fresh, single Reset).
#include <d3d12.h>
#include <dxgi1_6.h>
#include <stdio.h>

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    IDXGIFactory1* f = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) return 1;
    IDXGIAdapter1* a = nullptr;
    if (FAILED(f->EnumAdapters1(0, &a))) return 1;
    ID3D12Device* dev = nullptr;
    if (FAILED(D3D12CreateDevice(a, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev)))) return 1;

    // one texture per scenario so state tracking is independent
    struct Tex { ID3D12Resource* t; };
    Tex texes[4];
    for (int i = 0; i < 4; ++i) {
        D3D12_RESOURCE_DESC td{};
        td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width = 64; td.Height = 64; td.DepthOrArraySize = 1; td.MipLevels = 1;
        td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
        td.SampleDesc.Count = 1;
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texes[i].t)))) return 1;
    }

    auto freshList = [](ID3D12Device* dev, ID3D12CommandAllocator** al, ID3D12GraphicsCommandList** cl) -> HRESULT {
        if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(al)))) return E_FAIL;
        HRESULT hr = dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, *al, nullptr, IID_PPV_ARGS(cl));
        if (FAILED(hr)) { (*al)->Release(); *al = nullptr; return hr; }
        return S_OK;
    };

    // (c) fresh list: plain transition COPY_DEST -> COPY_SOURCE + Close
    {
        ID3D12CommandAllocator* al = nullptr; ID3D12GraphicsCommandList* cl = nullptr;
        if (FAILED(freshList(dev, &al, &cl))) return 1;
        HRESULT hrR = cl->Reset(al, nullptr);
        D3D12_RESOURCE_BARRIER t{};
        t.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        t.Transition.pResource = texes[0].t;
        t.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        t.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        t.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        cl->ResourceBarrier(1, &t);
        HRESULT hrC = cl->Close();
        printf("(c) FRESH list plain transition: Reset=0x%08X Close=0x%08X\n", (unsigned)hrR, (unsigned)hrC);
        cl->Release(); al->Release();
    }

    // (b2) fresh list: UAV aliasing barrier + transition from COMMON(0) + Close
    {
        ID3D12CommandAllocator* al = nullptr; ID3D12GraphicsCommandList* cl = nullptr;
        if (FAILED(freshList(dev, &al, &cl))) return 1;
        HRESULT hrR = cl->Reset(al, nullptr);
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b.UAV.pResource = texes[1].t;
        cl->ResourceBarrier(1, &b);
        D3D12_RESOURCE_BARRIER t{};
        t.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        t.Transition.pResource = texes[1].t;
        t.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        t.Transition.StateBefore = (D3D12_RESOURCE_STATES)0u; // COMMON after aliasing barrier
        t.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        cl->ResourceBarrier(1, &t);
        HRESULT hrC = cl->Close();
        printf("(b) FRESH list UAV-alias + transition from COMMON: Reset=0x%08X Close=0x%08X\n", (unsigned)hrR, (unsigned)hrC);
        cl->Release(); al->Release();
    }

    // (d) fresh list: NO barriers at all, just Close (control)
    {
        ID3D12CommandAllocator* al = nullptr; ID3D12GraphicsCommandList* cl = nullptr;
        if (FAILED(freshList(dev, &al, &cl))) return 1;
        HRESULT hrR = cl->Reset(al, nullptr);
        HRESULT hrC = cl->Close();
        printf("(d) FRESH list empty: Reset=0x%08X Close=0x%08X\n", (unsigned)hrR, (unsigned)hrC);
        cl->Release(); al->Release();
    }

    // (e) fresh list: transition to a state the resource was NOT created in (COPY_DEST -> GENERIC_READ)
    {
        ID3D12CommandAllocator* al = nullptr; ID3D12GraphicsCommandList* cl = nullptr;
        if (FAILED(freshList(dev, &al, &cl))) return 1;
        HRESULT hrR = cl->Reset(al, nullptr);
        D3D12_RESOURCE_BARRIER t{};
        t.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        t.Transition.pResource = texes[3].t;
        t.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        t.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        t.Transition.StateAfter = D3D12_RESOURCE_STATE_GENERIC_READ;
        cl->ResourceBarrier(1, &t);
        HRESULT hrC = cl->Close();
        printf("(e) FRESH list transition to GENERIC_READ: Reset=0x%08X Close=0x%08X\n", (unsigned)hrR, (unsigned)hrC);
        cl->Release(); al->Release();
    }

    return 0;
}

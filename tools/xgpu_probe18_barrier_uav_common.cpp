// barrier_test3.cpp — isolate exactly what this driver rejects:
//  (f) fresh list: UAV barrier ONLY + Close
//  (g) fresh list: transition from StateBefore=COMMON(0), NO preceding UAV barrier + Close
//  (h) fresh list: transition with WRONG concrete StateBefore (GENERIC_READ, actual=COPY_DEST) + Close
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

    // (f) UAV barrier only
    {
        ID3D12CommandAllocator* al = nullptr; ID3D12GraphicsCommandList* cl = nullptr;
        if (FAILED(freshList(dev, &al, &cl))) return 1;
        cl->Reset(al, nullptr);
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b.UAV.pResource = texes[0].t;
        cl->ResourceBarrier(1, &b);
        HRESULT hrC = cl->Close();
        printf("(f) FRESH list UAV barrier only: Close=0x%08X\n", (unsigned)hrC);
        cl->Release(); al->Release();
    }

    // (g) transition from COMMON(0) with NO preceding UAV barrier
    {
        ID3D12CommandAllocator* al = nullptr; ID3D12GraphicsCommandList* cl = nullptr;
        if (FAILED(freshList(dev, &al, &cl))) return 1;
        cl->Reset(al, nullptr);
        D3D12_RESOURCE_BARRIER t{};
        t.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        t.Transition.pResource = texes[1].t;
        t.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        t.Transition.StateBefore = (D3D12_RESOURCE_STATES)0u; // COMMON
        t.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        cl->ResourceBarrier(1, &t);
        HRESULT hrC = cl->Close();
        printf("(g) FRESH list transition from COMMON (no UAV): Close=0x%08X\n", (unsigned)hrC);
        cl->Release(); al->Release();
    }

    // (h) WRONG concrete StateBefore: resource actually in COPY_DEST, claim GENERIC_READ
    {
        ID3D12CommandAllocator* al = nullptr; ID3D12GraphicsCommandList* cl = nullptr;
        if (FAILED(freshList(dev, &al, &cl))) return 1;
        cl->Reset(al, nullptr);
        D3D12_RESOURCE_BARRIER t{};
        t.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        t.Transition.pResource = texes[2].t;
        t.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        t.Transition.StateBefore = D3D12_RESOURCE_STATE_GENERIC_READ;   // WRONG (actual: COPY_DEST)
        t.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        cl->ResourceBarrier(1, &t);
        HRESULT hrC = cl->Close();
        printf("(h) FRESH list WRONG StateBefore GENERIC_READ: Close=0x%08X\n", (unsigned)hrC);
        cl->Release(); al->Release();
    }

    // (i) UAV barrier + transition from COMMON, but with Flags=NONE explicit and separate calls in ONE ResourceBarrier array of 2
    {
        ID3D12CommandAllocator* al = nullptr; ID3D12GraphicsCommandList* cl = nullptr;
        if (FAILED(freshList(dev, &al, &cl))) return 1;
        cl->Reset(al, nullptr);
        D3D12_RESOURCE_BARRIER arr[2]{};
        arr[0].Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        arr[0].UAV.pResource = texes[3].t;
        arr[1].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        arr[1].Transition.pResource = texes[3].t;
        arr[1].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        arr[1].Transition.StateBefore = (D3D12_RESOURCE_STATES)0u;
        arr[1].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        cl->ResourceBarrier(2, arr);
        HRESULT hrC = cl->Close();
        printf("(i) FRESH list UAV+transition in ONE call: Close=0x%08X\n", (unsigned)hrC);
        cl->Release(); al->Release();
    }

    return 0;
}

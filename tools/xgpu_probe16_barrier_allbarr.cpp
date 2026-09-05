// barrier_test.cpp - does this AMD driver accept:
//   (a) D3D12_RESOURCE_STATE_ALL_BARRIERS in a legacy transition?  [known: NO]
//   (b) UAV aliasing barrier + transition from COMMON(0)?          [needed for post-FFX mirrors]
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

    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = 64; td.Height = 64; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    td.SampleDesc.Count = 1;
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    ID3D12Resource* tex = nullptr;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex)))) return 1;

    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* q = nullptr; dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q));
    ID3D12CommandAllocator* al = nullptr; dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&al));
    ID3D12GraphicsCommandList* cl = nullptr;
    dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, al, nullptr, IID_PPV_ARGS(&cl));

    // (a) legacy transition with ALL_BARRIERS as StateBefore
    {
        cl->Reset(al, nullptr);
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = tex;
        b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = (D3D12_RESOURCE_STATES)0x80000000u; // ALL_BARRIERS
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        cl->ResourceBarrier(1, &b);
        HRESULT hr = cl->Close();
        printf("(a) legacy transition StateBefore=ALL_BARRIERS: Close hr=0x%08X\n", (unsigned)hr);
    }

    // (b) UAV aliasing barrier + transition from COMMON(0)
    {
        cl->Reset(al, nullptr);
        D3D12_RESOURCE_BARRIER b{};
        b.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        b.UAV.pResource = tex;
        cl->ResourceBarrier(1, &b);
        D3D12_RESOURCE_BARRIER t{};
        t.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        t.Transition.pResource = tex;
        t.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        t.Transition.StateBefore = (D3D12_RESOURCE_STATES)0u; // COMMON after aliasing barrier
        t.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        cl->ResourceBarrier(1, &t);
        HRESULT hr = cl->Close();
        printf("(b) UAV-alias + transition from COMMON: Close hr=0x%08X\n", (unsigned)hr);
    }

    // (c) plain transition from a concrete state (sanity control)
    {
        cl->Reset(al, nullptr);
        D3D12_RESOURCE_BARRIER t{};
        t.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        t.Transition.pResource = tex;
        t.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        t.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        t.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
        cl->ResourceBarrier(1, &t);
        HRESULT hr = cl->Close();
        printf("(c) plain transition COPY_DEST->COPY_SOURCE: Close hr=0x%08X\n", (unsigned)hr);
    }

    return 0;
}

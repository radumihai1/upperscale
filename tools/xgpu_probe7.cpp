// xgpu_probe7.cpp — Minimal: readback buffer COPY_DEST -> barrier GENERIC_READ -> Map.
#include <d3d12.h>
#include <dxgi1_6.h>
#include <combaseapi.h>
#include <stdio.h>

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    IDXGIFactory1* f = nullptr; CreateDXGIFactory1(IID_PPV_ARGS(&f));
    IDXGIAdapter1* a = nullptr; f->EnumAdapters1(1, &a);
    ID3D12Device* dev = nullptr; D3D12CreateDevice(a, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev));

    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* q = nullptr; dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q));
    ID3D12CommandAllocator* alloc = nullptr; dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
    ID3D12GraphicsCommandList* cl = nullptr; dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&cl));
    ID3D12Fence* fence = nullptr; dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    HANDLE ev = CreateEvent(nullptr, TRUE, FALSE, nullptr);

    D3D12_HEAP_PROPERTIES rb{}; rb.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = 4096; bd.Height=1; bd.DepthOrArraySize=1; bd.MipLevels=1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; bd.SampleDesc.Count = 1;

    ID3D12Resource* rbuf = nullptr;
    HRESULT hr = dev->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rbuf));
    printf("create readback COPY_DEST: 0x%08X\n", (unsigned)hr);

    // Try Map in COPY_DEST state first (expected to fail).
    void* p = nullptr; D3D12_RANGE rng{0,4096};
    HRESULT hm = rbuf->Map(0, &rng, &p);
    printf("map while COPY_DEST: 0x%08X\n", (unsigned)hm);

    // Barrier to GENERIC_READ, execute, wait.
    D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = rbuf; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
    b.Transition.StateAfter  = D3D12_RESOURCE_STATE_GENERIC_READ;
    cl->Reset(alloc, nullptr);
    cl->ResourceBarrier(1, &b);
    cl->Close();
    { ID3D12CommandList* lists[] = { cl }; q->ExecuteCommandLists(1, lists); }
    q->Signal(fence, 1); fence->SetEventOnCompletion(1, ev); WaitForSingleObject(ev, 5000);

    hm = rbuf->Map(0, &rng, &p);
    printf("map after barrier to GENERIC_READ: 0x%08X\n", (unsigned)hm);
    if (SUCCEEDED(hm)) { ((UINT*)p)[0] = 0xDEADBEEF; rbuf->Unmap(0, nullptr); }

    // Also try: create readback buffer directly in COMMON then barrier.
    ID3D12Resource* rbuf2 = nullptr;
    hr = dev->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&rbuf2));
    printf("create readback COMMON: 0x%08X\n", (unsigned)hr);
    if (SUCCEEDED(hr)) {
        D3D12_RESOURCE_BARRIER b2{}; b2.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b2.Transition.pResource = rbuf2; b2.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b2.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
        b2.Transition.StateAfter  = D3D12_RESOURCE_STATE_GENERIC_READ;
        cl->Reset(alloc, nullptr); cl->ResourceBarrier(1, &b2); cl->Close();
        { ID3D12CommandList* lists[] = { cl }; q->ExecuteCommandLists(1, lists); }
        q->Signal(fence, 2); fence->SetEventOnCompletion(2, ev); WaitForSingleObject(ev, 5000);
        hm = rbuf2->Map(0, &rng, &p);
        printf("map rbuf2 after barrier: 0x%08X\n", (unsigned)hm);
    }

    return 0;
}

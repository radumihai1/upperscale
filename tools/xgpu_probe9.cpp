// xgpu_probe9.cpp — READBACK Map-state matrix: which (initial state -> barrier) combos allow Map?
#include <d3d12.h>
#include <dxgi1_6.h>
#include <combaseapi.h>
#include <stdio.h>
#include <cstdint>

static void TestMap(ID3D12Device* dev, ID3D12CommandQueue* q, ID3D12CommandAllocator* alloc,
                    ID3D12GraphicsCommandList* cl, ID3D12Fence* fence, HANDLE ev, UINT64 fv) {
    D3D12_HEAP_PROPERTIES rb{}; rb.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = 589824; bd.Height=1; bd.DepthOrArraySize=1; bd.MipLevels=1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; bd.SampleDesc.Count = 1;

    // (a) create in COPY_DEST, Map directly
    ID3D12Resource* r1=nullptr;
    HRESULT hr = dev->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&r1));
    void* p=nullptr; D3D12_RANGE rng{0,(SIZE_T)-1};
    HRESULT hm = r1->Map(0,&rng,&p);
    printf("  (a) COPY_DEST -> Map direct        : create=%s map=0x%08X\n", SUCCEEDED(hr)?"ok":"FAIL",(unsigned)hm);
    if (r1) r1->Release();

    // (b) create in COMMON, barrier to GENERIC_READ, then Map
    ID3D12Resource* r2=nullptr;
    hr = dev->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&r2));
    if (SUCCEEDED(hr)) {
        D3D12_RESOURCE_BARRIER b{}; b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource=r2; b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore=D3D12_RESOURCE_STATE_COMMON; b.Transition.StateAfter=D3D12_RESOURCE_STATE_GENERIC_READ;
        cl->Reset(alloc,nullptr); cl->ResourceBarrier(1,&b); cl->Close();
        { ID3D12CommandList* lists[]={cl}; q->ExecuteCommandLists(1,lists); }
        q->Signal(fence,fv); fence->SetEventOnCompletion(fv,ev); WaitForSingleObject(ev,5000);
        hm = r2->Map(0,&rng,&p);
        printf("  (b) COMMON -> barrier GR -> Map : map=0x%08X\n",(unsigned)hm);
        if (SUCCEEDED(hm)) r2->Unmap(0,nullptr);
    } else {
        printf("  (b) create in COMMON FAILED 0x%08X\n",(unsigned)hr);
    }
    if (r2) r2->Release();

    // (c) create in COPY_DEST, barrier to GENERIC_READ, then Map
    ID3D12Resource* r3=nullptr;
    hr = dev->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&r3));
    if (SUCCEEDED(hr)) {
        D3D12_RESOURCE_BARRIER b{}; b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource=r3; b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_DEST; b.Transition.StateAfter=D3D12_RESOURCE_STATE_GENERIC_READ;
        cl->Reset(alloc,nullptr); cl->ResourceBarrier(1,&b); cl->Close();
        { ID3D12CommandList* lists[]={cl}; q->ExecuteCommandLists(1,lists); }
        fv++; q->Signal(fence,fv); fence->SetEventOnCompletion(fv,ev); WaitForSingleObject(ev,5000);
        hm = r3->Map(0,&rng,&p);
        printf("  (c) COPY_DEST -> barrier GR -> Map: map=0x%08X\n",(unsigned)hm);
        if (SUCCEEDED(hm)) r3->Unmap(0,nullptr);
    } else {
        printf("  (c) create in COPY_DEST FAILED 0x%08X\n",(unsigned)hr);
    }
    if (r3) r3->Release();

    // (d) UPLOAD buffer, Map direct (control — should always work)
    D3D12_HEAP_PROPERTIES up{}; up.Type=D3D12_HEAP_TYPE_UPLOAD;
    ID3D12Resource* r4=nullptr;
    hr = dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&r4));
    hm = r4->Map(0,&rng,&p);
    printf("  (d) UPLOAD GENERIC_READ -> Map     : create=%s map=0x%08X\n", SUCCEEDED(hr)?"ok":"FAIL",(unsigned)hm);
    if (SUCCEEDED(hm)) r4->Unmap(0,nullptr);
    if (r4) r4->Release();
}

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    IDXGIFactory1* f=nullptr; CreateDXGIFactory1(IID_PPV_ARGS(&f));
    IDXGIAdapter1* a=nullptr; f->EnumAdapters1(1,&a); // 7900 XTX
    ID3D12Device* dev=nullptr; D3D12CreateDevice(a, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev));

    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* q=nullptr; dev->CreateCommandQueue(&qd,IID_PPV_ARGS(&q));
    ID3D12CommandAllocator* alloc=nullptr; dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&alloc));
    ID3D12GraphicsCommandList* cl=nullptr; dev->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,alloc,nullptr,IID_PPV_ARGS(&cl));
    ID3D12Fence* fence=nullptr; dev->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence));
    HANDLE ev=CreateEvent(nullptr,TRUE,FALSE,nullptr);

    printf("== READBACK Map-state matrix (7900 XTX) ==\n");
    TestMap(dev,q,alloc,cl,fence,ev,1);
    return 0;
}

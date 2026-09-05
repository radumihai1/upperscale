// xgpu_probe12.cpp — Pinpoint EXACTLY which call removes the device on buffer->texture copy.
#include <d3d12.h>
#include <dxgi1_6.h>
#include <combaseapi.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <cstdint>

static void chk(ID3D12Device* d, const char* what) {
    HRESULT r = d->GetDeviceRemovedReason();
    printf("  after %-28s : removed=0x%08X %s\n", what, (unsigned)r, SUCCEEDED(r)?"":"<-- DEAD");
}

int main(int argc,char**argv) {
    setvbuf(stdout,NULL,_IONBF,0);
    int adpIdx=(argc>=2)?atoi(argv[1]):1;
    IDXGIFactory1* f=nullptr; CreateDXGIFactory1(IID_PPV_ARGS(&f));
    IDXGIAdapter1* a=nullptr; f->EnumAdapters1(adpIdx,&a);
    ID3D12Device* dev=nullptr; D3D12CreateDevice(a, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev));
    printf("adapter[%d]\n", adpIdx);

    const SIZE_T BYTES=64*64*4;
    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* q=nullptr; dev->CreateCommandQueue(&qd,IID_PPV_ARGS(&q));
    ID3D12CommandAllocator* alloc=nullptr; dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&alloc));
    ID3D12GraphicsCommandList* cl=nullptr; dev->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,alloc,nullptr,IID_PPV_ARGS(&cl));
    ID3D12Fence* fence=nullptr; dev->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence));
    HANDLE ev=CreateEvent(nullptr,TRUE,FALSE,nullptr);

    D3D12_HEAP_PROPERTIES up{};up.Type=D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd{};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;bd.Width=BYTES;bd.Height=1;bd.DepthOrArraySize=1;bd.MipLevels=1;bd.Format=DXGI_FORMAT_UNKNOWN;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;bd.SampleDesc.Count=1;
    ID3D12Resource* ub=nullptr; dev->CreateCommittedResource(&up,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&ub));
    D3D12_HEAP_PROPERTIES def{};def.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{};rd.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;rd.Width=64;rd.Height=64;rd.DepthOrArraySize=1;rd.MipLevels=1;rd.Format=DXGI_FORMAT_R8G8B8A8_UNORM;rd.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;rd.SampleDesc.Count=1;
    ID3D12Resource* tex=nullptr; dev->CreateCommittedResource(&def,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&tex));

    D3D12_TEXTURE_COPY_LOCATION dst{tex,D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,{0}};
    D3D12_TEXTURE_COPY_LOCATION src{ub, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,{0}};

    chk(dev,"setup (resources created)");
    cl->Reset(alloc,nullptr);            chk(dev,"cl->Reset");
    { D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition.pResource=ub;b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;b.Transition.StateBefore=D3D12_RESOURCE_STATE_GENERIC_READ;b.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;cl->ResourceBarrier(1,&b); }
    chk(dev,"barrier ub GR->COPY_SRC");
    cl->CopyTextureRegion(&dst,0,0,0,&src,nullptr);  chk(dev,"CopyTextureRegion buf->tex");
    { D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition.pResource=ub;b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;b.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_SOURCE;b.Transition.StateAfter=D3D12_RESOURCE_STATE_GENERIC_READ;cl->ResourceBarrier(1,&b); }
    chk(dev,"barrier ub COPY_SRC->GR");
    cl->Close();                          chk(dev,"cl->Close");
    { ID3D12CommandList* l[]={cl}; q->ExecuteCommandLists(1,l); }  chk(dev,"ExecuteCommandLists");
    q->Signal(fence,1); fence->SetEventOnCompletion(1,ev); WaitForSingleObject(ev,5000);  chk(dev,"fence wait");

    return 0;
}

// xgpu_probe10.cpp — Isolate GPU A only: upload buffer -> texture -> readback buffer, map+verify.
// No second device. Finds which op triggers DEVICE_REMOVED on this AMD driver.
#include <d3d12.h>
#include <dxgi1_6.h>
#include <combaseapi.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <cstdint>

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int adpIdx = (argc>=2)?atoi(argv[1]):1; // default adapter[1]=7900 XTX (headless)
    IDXGIFactory1* f=nullptr; CreateDXGIFactory1(IID_PPV_ARGS(&f));
    IDXGIAdapter1* a=nullptr; f->EnumAdapters1(adpIdx,&a);
    DXGI_ADAPTER_DESC1 ad{}; a->GetDesc1(&ad); char nm[256]{}; WideCharToMultiByte(CP_UTF8,0,ad.Description,-1,nm,sizeof nm,nullptr,nullptr);
    ID3D12Device* dev=nullptr; D3D12CreateDevice(a, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev));
    printf("adapter[%d] = %s\n", adpIdx, nm);

    const UINT W=512,H=288; const SIZE_T BYTES=(SIZE_T)W*H*4;
    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* q=nullptr; dev->CreateCommandQueue(&qd,IID_PPV_ARGS(&q));
    ID3D12CommandAllocator* alloc=nullptr; dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&alloc));
    ID3D12GraphicsCommandList* cl=nullptr; dev->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,alloc,nullptr,IID_PPV_ARGS(&cl));
    ID3D12Fence* fence=nullptr; dev->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&fence));
    HANDLE ev=CreateEvent(nullptr,TRUE,FALSE,nullptr);

    D3D12_HEAP_PROPERTIES def{}; def.Type=D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC rd{}; rd.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width=W;rd.Height=H;rd.DepthOrArraySize=1;rd.MipLevels=1;rd.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
    rd.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;rd.SampleDesc.Count=1;
    ID3D12Resource* tex=nullptr;
    HRESULT hr=dev->CreateCommittedResource(&def,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&tex));
    printf("create texture: 0x%08X\n",(unsigned)hr);

    D3D12_RESOURCE_DESC bd{}; bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width=BYTES;bd.Height=1;bd.DepthOrArraySize=1;bd.MipLevels=1;
    bd.Format=DXGI_FORMAT_UNKNOWN;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;bd.SampleDesc.Count=1;

    D3D12_HEAP_PROPERTIES up{}; up.Type=D3D12_HEAP_TYPE_UPLOAD;
    ID3D12Resource* ubuf=nullptr;
    dev->CreateCommittedResource(&up,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&ubuf));
    uint8_t* pat=new uint8_t[BYTES]; for(SIZE_T i=0;i<BYTES;i++) pat[i]=(uint8_t)(i&0xFF);
    void* mp=nullptr; ubuf->Map(0,nullptr,&mp); memcpy(mp,pat,BYTES); ubuf->Unmap(0,nullptr);

    D3D12_HEAP_PROPERTIES rb{}; rb.Type=D3D12_HEAP_TYPE_READBACK;
    ID3D12Resource* rbuf=nullptr;
    dev->CreateCommittedResource(&rb,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&rbuf));

    D3D12_TEXTURE_COPY_LOCATION dstTex{tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,{0}};
    D3D12_TEXTURE_COPY_LOCATION srcUp{ubuf,D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,{0}};
    D3D12_TEXTURE_COPY_LOCATION dstRb{rbuf, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,{0}};
    D3D12_TEXTURE_COPY_LOCATION srcTex{tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,{0}};

    // Step 1: upload -> texture (separate list). Buffer must be COPY_SOURCE, tex COPY_DEST.
    cl->Reset(alloc,nullptr);
    { D3D12_RESOURCE_BARRIER b{}; b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      b.Transition.pResource=ubuf;b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      b.Transition.StateBefore=D3D12_RESOURCE_STATE_GENERIC_READ;b.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
      cl->ResourceBarrier(1,&b); }
    cl->CopyTextureRegion(&dstTex,0,0,0,&srcUp,nullptr);
    { D3D12_RESOURCE_BARRIER b{}; b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      b.Transition.pResource=ubuf;b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      b.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_SOURCE;b.Transition.StateAfter=D3D12_RESOURCE_STATE_GENERIC_READ;
      cl->ResourceBarrier(1,&b); }
    cl->Close();
    { ID3D12CommandList* lists[]={cl}; q->ExecuteCommandLists(1,lists); }
    q->Signal(fence,1); fence->SetEventOnCompletion(1,ev); WaitForSingleObject(ev,5000);
    printf("after upload->tex: GetDeviceRemovedReason=0x%08X\n",(unsigned)dev->GetDeviceRemovedReason());

    // Step 2: texture -> readback buffer (separate list), with barrier tex COPY_DEST->COPY_SOURCE
    cl->Reset(alloc,nullptr);
    { D3D12_RESOURCE_BARRIER b{}; b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      b.Transition.pResource=tex;b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      b.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_DEST;b.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;
      cl->ResourceBarrier(1,&b); }
    cl->CopyTextureRegion(&dstRb,0,0,0,&srcTex,nullptr);
    cl->Close();
    { ID3D12CommandList* lists[]={cl}; q->ExecuteCommandLists(1,lists); }
    q->Signal(fence,2); fence->SetEventOnCompletion(2,ev); WaitForSingleObject(ev,5000);
    printf("after tex->readback: GetDeviceRemovedReason=0x%08X\n",(unsigned)dev->GetDeviceRemovedReason());

    void* p=nullptr; D3D12_RANGE rng{0,BYTES};
    HRESULT hm=rbuf->Map(0,&rng,&p);
    printf("map readback: 0x%08X\n",(unsigned)hm);
    if (SUCCEEDED(hm)) {
        int mm=0; for(SIZE_T i=0;i<BYTES;i++) if(((uint8_t*)p)[i]!=pat[i]) mm++;
        printf("verify: %d mismatches / %llu bytes\n",mm,(unsigned long long)BYTES);
        rbuf->Unmap(0,nullptr);
    }
    delete[] pat;
    return 0;
}

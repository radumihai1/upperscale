// xgpu_probe13.cpp — Isolated copy-type tests, each on a FRESH device with null checks.
//   T2T  : texture(COPY_SOURCE) -> texture(COPY_DEST)
//   B2T  : buffer(GR->COPY_SRC) -> texture(COPY_DEST)
//   T2B  : texture(COPY_SOURCE) -> readback buffer(COPY_DEST)
#include <d3d12.h>
#include <dxgi1_6.h>
#include <combaseapi.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <cstdint>

struct Ctx { ID3D12Device* dev=nullptr; ID3D12CommandQueue* q=nullptr;
             ID3D12CommandAllocator* alloc=nullptr; ID3D12GraphicsCommandList* cl=nullptr;
             ID3D12Fence* fence=nullptr; HANDLE ev=nullptr; };

static bool Init(Ctx& c, int adpIdx) {
    IDXGIFactory1* f=nullptr; if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) return false;
    IDXGIAdapter1* a=nullptr; if (FAILED(f->EnumAdapters1(adpIdx,&a))) return false;
    if (FAILED(D3D12CreateDevice(a, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&c.dev)))) { a->Release(); f->Release(); return false; }
    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    c.dev->CreateCommandQueue(&qd,IID_PPV_ARGS(&c.q));
    c.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&c.alloc));
    c.dev->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,c.alloc,nullptr,IID_PPV_ARGS(&c.cl));
    c.dev->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&c.fence));
    c.ev=CreateEvent(nullptr,TRUE,FALSE,nullptr);
    a->Release(); f->Release(); return true;
}
static void RunWait(Ctx& c) { ID3D12CommandList* l[]={c.cl}; c.q->ExecuteCommandLists(1,l);
    c.q->Signal(c.fence,1); c.fence->SetEventOnCompletion(1,c.ev); WaitForSingleObject(c.ev,5000); }

static void Barrier(Ctx& c, ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition.pResource=r;
    b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;b.Transition.StateBefore=from;b.Transition.StateAfter=to;
    c.cl->ResourceBarrier(1,&b);
}

int main(int argc,char**argv) {
    setvbuf(stdout,NULL,_IONBF,0);
    int adpIdx=(argc>=2)?atoi(argv[1]):1;
    const SIZE_T BYTES=64*64*4;

    // T2T
    { Ctx c; if(!Init(c,adpIdx)){printf("T2T init fail\n");return 1;}
      D3D12_HEAP_PROPERTIES def{};def.Type=D3D12_HEAP_TYPE_DEFAULT;
      D3D12_RESOURCE_DESC rd{};rd.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;rd.Width=64;rd.Height=64;rd.DepthOrArraySize=1;rd.MipLevels=1;rd.Format=DXGI_FORMAT_R8G8B8A8_UNORM;rd.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;rd.SampleDesc.Count=1;
      ID3D12Resource* ts=nullptr,*td=nullptr;
      c.dev->CreateCommittedResource(&def,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COPY_SOURCE,nullptr,IID_PPV_ARGS(&ts));
      c.dev->CreateCommittedResource(&def,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&td));
      D3D12_TEXTURE_COPY_LOCATION dst{td,D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,{0}};
      D3D12_TEXTURE_COPY_LOCATION src{ts,D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,{0}};
      c.cl->Reset(c.alloc,nullptr); c.cl->CopyTextureRegion(&dst,0,0,0,&src,nullptr); c.cl->Close(); RunWait(c);
      printf("T2T  texture->texture : removed=0x%08X\n",(unsigned)c.dev->GetDeviceRemovedReason());
    }

    // B2T
    { Ctx c; if(!Init(c,adpIdx)){printf("B2T init fail\n");return 1;}
      D3D12_HEAP_PROPERTIES up{};up.Type=D3D12_HEAP_TYPE_UPLOAD;
      D3D12_RESOURCE_DESC bd{};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;bd.Width=BYTES;bd.Height=1;bd.DepthOrArraySize=1;bd.MipLevels=1;bd.Format=DXGI_FORMAT_UNKNOWN;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;bd.SampleDesc.Count=1;
      ID3D12Resource* ub=nullptr; c.dev->CreateCommittedResource(&up,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&ub));
      D3D12_HEAP_PROPERTIES def{};def.Type=D3D12_HEAP_TYPE_DEFAULT;
      D3D12_RESOURCE_DESC rd{};rd.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;rd.Width=64;rd.Height=64;rd.DepthOrArraySize=1;rd.MipLevels=1;rd.Format=DXGI_FORMAT_R8G8B8A8_UNORM;rd.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;rd.SampleDesc.Count=1;
      ID3D12Resource* tex=nullptr; c.dev->CreateCommittedResource(&def,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&tex));
      D3D12_TEXTURE_COPY_LOCATION dst{tex,D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,{0}};
      D3D12_TEXTURE_COPY_LOCATION src{ub, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,{0}};
      c.cl->Reset(c.alloc,nullptr); Barrier(c,ub,D3D12_RESOURCE_STATE_GENERIC_READ,D3D12_RESOURCE_STATE_COPY_SOURCE);
      c.cl->CopyTextureRegion(&dst,0,0,0,&src,nullptr); Barrier(c,ub,D3D12_RESOURCE_STATE_COPY_SOURCE,D3D12_RESOURCE_STATE_GENERIC_READ);
      c.cl->Close(); RunWait(c);
      printf("B2T  buffer->texture  : removed=0x%08X\n",(unsigned)c.dev->GetDeviceRemovedReason());
    }

    // T2B
    { Ctx c; if(!Init(c,adpIdx)){printf("T2B init fail\n");return 1;}
      D3D12_HEAP_PROPERTIES def{};def.Type=D3D12_HEAP_TYPE_DEFAULT;
      D3D12_RESOURCE_DESC rd{};rd.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;rd.Width=64;rd.Height=64;rd.DepthOrArraySize=1;rd.MipLevels=1;rd.Format=DXGI_FORMAT_R8G8B8A8_UNORM;rd.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;rd.SampleDesc.Count=1;
      ID3D12Resource* tex=nullptr; c.dev->CreateCommittedResource(&def,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COPY_SOURCE,nullptr,IID_PPV_ARGS(&tex));
      D3D12_HEAP_PROPERTIES rb{};rb.Type=D3D12_HEAP_TYPE_READBACK;
      D3D12_RESOURCE_DESC bd{};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;bd.Width=BYTES;bd.Height=1;bd.DepthOrArraySize=1;bd.MipLevels=1;bd.Format=DXGI_FORMAT_UNKNOWN;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;bd.SampleDesc.Count=1;
      ID3D12Resource* rbuf=nullptr; c.dev->CreateCommittedResource(&rb,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&rbuf));
      D3D12_TEXTURE_COPY_LOCATION dst{rbuf,D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,{0}};
      D3D12_TEXTURE_COPY_LOCATION src{tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,{0}};
      c.cl->Reset(c.alloc,nullptr); c.cl->CopyTextureRegion(&dst,0,0,0,&src,nullptr); c.cl->Close(); RunWait(c);
      printf("T2B  texture->buffer   : removed=0x%08X\n",(unsigned)c.dev->GetDeviceRemovedReason());
    }

    return 0;
}

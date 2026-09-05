// xgpu_probe11.cpp — Which CopyTextureRegion source type works? Test each on a FRESH device.
//   A) buffer -> texture  (buffer as copy source)
//   B) texture -> texture
//   C) texture -> buffer   (readback; buffer as copy dest)
#include <d3d12.h>
#include <dxgi1_6.h>
#include <combaseapi.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <cstdint>

struct Ctx { ID3D12Device* dev; ID3D12CommandQueue* q; ID3D12CommandAllocator* alloc;
             ID3D12GraphicsCommandList* cl; ID3D12Fence* fence; HANDLE ev; };

static void Init(Ctx& c, int adpIdx) {
    IDXGIFactory1* f=nullptr; CreateDXGIFactory1(IID_PPV_ARGS(&f));
    IDXGIAdapter1* a=nullptr; f->EnumAdapters1(adpIdx,&a);
    D3D12CreateDevice(a, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&c.dev));
    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type=D3D12_COMMAND_LIST_TYPE_DIRECT;
    c.dev->CreateCommandQueue(&qd,IID_PPV_ARGS(&c.q));
    c.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,IID_PPV_ARGS(&c.alloc));
    c.dev->CreateCommandList(0,D3D12_COMMAND_LIST_TYPE_DIRECT,c.alloc,nullptr,IID_PPV_ARGS(&c.cl));
    c.dev->CreateFence(0,D3D12_FENCE_FLAG_NONE,IID_PPV_ARGS(&c.fence));
    c.ev=CreateEvent(nullptr,TRUE,FALSE,nullptr);
}
static void Run(Ctx& c) { ID3D12CommandList* l[]={c.cl}; c.q->ExecuteCommandLists(1,l); }
static void Wait(Ctx& c, UINT64 v){ c.q->Signal(c.fence,v); c.fence->SetEventOnCompletion(v,c.ev); WaitForSingleObject(c.ev,5000); }

int main(int argc,char**argv) {
    setvbuf(stdout,NULL,_IONBF,0);
    int adpIdx=(argc>=2)?atoi(argv[1]):1;
    const SIZE_T BYTES=64*64*4; // 16KB texture (64x64 RGBA)

    // ---- Test A: buffer -> texture ----
    { Ctx c{}; Init(c,adpIdx);
      D3D12_HEAP_PROPERTIES up{};up.Type=D3D12_HEAP_TYPE_UPLOAD;
      D3D12_RESOURCE_DESC bd{};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;bd.Width=BYTES;bd.Height=1;bd.DepthOrArraySize=1;bd.MipLevels=1;bd.Format=DXGI_FORMAT_UNKNOWN;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;bd.SampleDesc.Count=1;
      ID3D12Resource* ub=nullptr; c.dev->CreateCommittedResource(&up,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_GENERIC_READ,nullptr,IID_PPV_ARGS(&ub));
      D3D12_HEAP_PROPERTIES def{};def.Type=D3D12_HEAP_TYPE_DEFAULT;
      D3D12_RESOURCE_DESC rd{};rd.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;rd.Width=64;rd.Height=64;rd.DepthOrArraySize=1;rd.MipLevels=1;rd.Format=DXGI_FORMAT_R8G8B8A8_UNORM;rd.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;rd.SampleDesc.Count=1;
      ID3D12Resource* tex=nullptr; c.dev->CreateCommittedResource(&def,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&tex));
      D3D12_TEXTURE_COPY_LOCATION dst{tex,D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,{0}};
      D3D12_TEXTURE_COPY_LOCATION src{ub, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,{0}};
      c.cl->Reset(c.alloc,nullptr);
      { D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition.pResource=ub;b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;b.Transition.StateBefore=D3D12_RESOURCE_STATE_GENERIC_READ;b.Transition.StateAfter=D3D12_RESOURCE_STATE_COPY_SOURCE;c.cl->ResourceBarrier(1,&b); }
      c.cl->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
      { D3D12_RESOURCE_BARRIER b{};b.Type=D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;b.Transition.pResource=ub;b.Transition.Subresource=D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;b.Transition.StateBefore=D3D12_RESOURCE_STATE_COPY_SOURCE;b.Transition.StateAfter=D3D12_RESOURCE_STATE_GENERIC_READ;c.cl->ResourceBarrier(1,&b); }
      c.cl->Close(); Run(c); Wait(c,1);
      printf("A) buffer->texture : removed=0x%08X\n",(unsigned)c.dev->GetDeviceRemovedReason());
    }

    // ---- Test B: texture -> texture ----
    { Ctx c{}; Init(c,adpIdx);
      D3D12_HEAP_PROPERTIES def{};def.Type=D3D12_HEAP_TYPE_DEFAULT;
      D3D12_RESOURCE_DESC rd{};rd.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;rd.Width=64;rd.Height=64;rd.DepthOrArraySize=1;rd.MipLevels=1;rd.Format=DXGI_FORMAT_R8G8B8A8_UNORM;rd.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;rd.SampleDesc.Count=1;
      ID3D12Resource* tsrc=nullptr,*tdst=nullptr;
      c.dev->CreateCommittedResource(&def,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COPY_SOURCE,nullptr,IID_PPV_ARGS(&tsrc));
      c.dev->CreateCommittedResource(&def,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&tdst));
      D3D12_TEXTURE_COPY_LOCATION dst{tdst,D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,{0}};
      D3D12_TEXTURE_COPY_LOCATION src{tsrc,D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,{0}};
      c.cl->Reset(c.alloc,nullptr);
      c.cl->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
      c.cl->Close(); Run(c); Wait(c,1);
      printf("B) texture->texture: removed=0x%08X\n",(unsigned)c.dev->GetDeviceRemovedReason());
    }

    // ---- Test C: texture -> buffer (readback) ----
    { Ctx c{}; Init(c,adpIdx);
      D3D12_HEAP_PROPERTIES def{};def.Type=D3D12_HEAP_TYPE_DEFAULT;
      D3D12_RESOURCE_DESC rd{};rd.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;rd.Width=64;rd.Height=64;rd.DepthOrArraySize=1;rd.MipLevels=1;rd.Format=DXGI_FORMAT_R8G8B8A8_UNORM;rd.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;rd.SampleDesc.Count=1;
      ID3D12Resource* tex=nullptr; c.dev->CreateCommittedResource(&def,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COPY_SOURCE,nullptr,IID_PPV_ARGS(&tex));
      D3D12_HEAP_PROPERTIES rb{};rb.Type=D3D12_HEAP_TYPE_READBACK;
      D3D12_RESOURCE_DESC bd{};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;bd.Width=BYTES;bd.Height=1;bd.DepthOrArraySize=1;bd.MipLevels=1;bd.Format=DXGI_FORMAT_UNKNOWN;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;bd.SampleDesc.Count=1;
      ID3D12Resource* rbuf=nullptr; c.dev->CreateCommittedResource(&rb,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&rbuf));
      D3D12_TEXTURE_COPY_LOCATION dst{rbuf,D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,{0}};
      D3D12_TEXTURE_COPY_LOCATION src{tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,{0}};
      c.cl->Reset(c.alloc,nullptr);
      c.cl->CopyTextureRegion(&dst,0,0,0,&src,nullptr);
      c.cl->Close(); Run(c); Wait(c,1);
      printf("C) texture->buffer : removed=0x%08X\n",(unsigned)c.dev->GetDeviceRemovedReason());
    }

    return 0;
}

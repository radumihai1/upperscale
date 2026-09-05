// xgpu_probe14.cpp — Find the WORKING upload primitive for buffer->texture on this driver.
//   B2T-CopyTextureRegion : CopyTextureRegion(buffer src -> texture dst)  [known: INVALID_CALL]
//   B2T-PlacedFootprint   : CopyTextureRegion with PlacedFootprint source (buffer as placed subresource)
//   T2T-via-upload-tex    : fill a 2D UPLOAD texture via Map, then CopyTextureRegion(tex->tex)
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
    c.ev=CreateEvent(nullptr,TRUE,FALSE,nullptr); a->Release(); f->Release(); return true;
}
static void RunWait(Ctx& c) { ID3D12CommandList* l[]={c.cl}; c.q->ExecuteCommandLists(1,l);
    c.q->Signal(c.fence,1); c.fence->SetEventOnCompletion(1,c.ev); WaitForSingleObject(c.ev,5000); }

int main(int argc,char**argv) {
    setvbuf(stdout,NULL,_IONBF,0);
    int adpIdx=(argc>=2)?atoi(argv[1]):1;
    const UINT W=64,H=64; const SIZE_T BYTES=W*H*4;

    // --- Method 3: fill a 2D UPLOAD texture via Map, then CopyTextureRegion(tex->tex) ---
    { Ctx c; if(!Init(c,adpIdx)){printf("M3 init fail\n");return 1;}
      D3D12_HEAP_PROPERTIES up{};up.Type=D3D12_HEAP_TYPE_UPLOAD;
      D3D12_RESOURCE_DESC rd{};rd.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;rd.Width=W;rd.Height=H;rd.DepthOrArraySize=1;rd.MipLevels=1;rd.Format=DXGI_FORMAT_R8G8B8A8_UNORM;rd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;rd.SampleDesc.Count=1;
      ID3D12Resource* utex=nullptr; HRESULT hr=c.dev->CreateCommittedResource(&up,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COPY_SOURCE,nullptr,IID_PPV_ARGS(&utex));
      printf("M3 create UPLOAD 2D tex (ROW_MAJOR): 0x%08X\n",(unsigned)hr);
      if (SUCCEEDED(hr)) {
        D3D12_HEAP_PROPERTIES def{};def.Type=D3D12_HEAP_TYPE_DEFAULT;
        rd.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;
        ID3D12Resource* dtex=nullptr; c.dev->CreateCommittedResource(&def,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&dtex));
        // fill utex via Map (row-major upload texture is CPU-writable)
        void* mp=nullptr; D3D12_RANGE rng{0,BYTES};
        HRESULT hm=utex->Map(0,&rng,&mp);
        printf("M3 map upload tex: 0x%08X\n",(unsigned)hm);
        if (SUCCEEDED(hm)) { memset(mp,0x5A,BYTES); utex->Unmap(0,nullptr); }
        D3D12_TEXTURE_COPY_LOCATION dst{dtex,D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,{0}};
        D3D12_TEXTURE_COPY_LOCATION src{utex,D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,{0}};
        c.cl->Reset(c.alloc,nullptr); c.cl->CopyTextureRegion(&dst,0,0,0,&src,nullptr); c.cl->Close(); RunWait(c);
        printf("M3 tex(upload)->tex(default) : removed=0x%08X\n",(unsigned)c.dev->GetDeviceRemovedReason());
      }
    }

    // --- Method 2: CopyTextureRegion with PlacedFootprint buffer source ---
    { Ctx c; if(!Init(c,adpIdx)){printf("M2 init fail\n");return 1;}
      D3D12_HEAP_PROPERTIES up{};up.Type=D3D12_HEAP_TYPE_UPLOAD;
      D3D12_RESOURCE_DESC bd{};bd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;bd.Width=BYTES;bd.Height=1;bd.DepthOrArraySize=1;bd.MipLevels=1;bd.Format=DXGI_FORMAT_UNKNOWN;bd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR;bd.SampleDesc.Count=1;
      ID3D12Resource* ub=nullptr; c.dev->CreateCommittedResource(&up,D3D12_HEAP_FLAG_NONE,&bd,D3D12_RESOURCE_STATE_COPY_SOURCE,nullptr,IID_PPV_ARGS(&ub));
      D3D12_HEAP_PROPERTIES def{};def.Type=D3D12_HEAP_TYPE_DEFAULT;
      D3D12_RESOURCE_DESC rd{};rd.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;rd.Width=W;rd.Height=H;rd.DepthOrArraySize=1;rd.MipLevels=1;rd.Format=DXGI_FORMAT_R8G8B8A8_UNORM;rd.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;rd.SampleDesc.Count=1;
      ID3D12Resource* dtex=nullptr; c.dev->CreateCommittedResource(&def,D3D12_HEAP_FLAG_NONE,&rd,D3D12_RESOURCE_STATE_COPY_DEST,nullptr,IID_PPV_ARGS(&dtex));
      D3D12_TEXTURE_COPY_LOCATION dst{dtex,D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX,{0}};
      // PlacedFootprint source: buffer at offset 0, row-major RGBA8 WxH
      D3D12_TEXTURE_COPY_LOCATION src{};
      src.pResource=ub; src.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
      src.PlacedFootprint.Offset=0; src.PlacedFootprint.Footprint.Format=DXGI_FORMAT_R8G8B8A8_UNORM;
      src.PlacedFootprint.Footprint.Width=W; src.PlacedFootprint.Footprint.Height=H;
      src.PlacedFootprint.Footprint.Depth=1; src.PlacedFootprint.Footprint.RowPitch=W*4;
      c.cl->Reset(c.alloc,nullptr); c.cl->CopyTextureRegion(&dst,0,0,0,&src,nullptr); c.cl->Close(); RunWait(c);
      printf("M2 buffer(PlacedFootprint)->tex : removed=0x%08X\n",(unsigned)c.dev->GetDeviceRemovedReason());
    }

    return 0;
}

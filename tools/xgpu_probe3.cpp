// xgpu_probe3.cpp — Definitive cross-node sharing capability probe.
// Queries D3D12_FEATURE_DATA_D3D12_OPTIONS (CrossNodeSharingTier,
// CrossAdapterRowMajorTextureSupported) on BOTH GPUs, then tests whether a
// plain committed resource with ALLOW_CROSS_ADAPTER can even be created.
// This tells us if raw cross-adapter shared heaps are viable at all here.
#include <d3d12.h>
#include <dxgi1_6.h>
#include <combaseapi.h>
#include <cstdio>
#include <cstdint>

static void PrintOpts(ID3D12Device* dev, const char* name) {
    D3D12_FEATURE_DATA_D3D12_OPTIONS o{};
    if (SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS, &o, sizeof(o))) ) {
        printf("  %-14s CrossNodeSharingTier=%d  CrossAdapterRowMajorTextureSupported=%d\n",
               name, (int)o.CrossNodeSharingTier, (int)o.CrossAdapterRowMajorTextureSupported);
    } else {
        printf("  %-14s CheckFeatureSupport(D3D12_OPTIONS) FAILED\n", name);
    }
}

int main() {
    printf("=== xgpu_probe3: cross-node sharing capability ===\n");
    IDXGIFactory1* f = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) { printf("no factory\n"); return 1; }
    IDXGIAdapter1* adp[8]; int n=0;
    for (int i=0;;i++) { IDXGIAdapter1* a=nullptr; if (f->EnumAdapters1(i,&a)!=S_OK) break; adp[n++]=a; if(n>=8)break; }

    // Find the two discrete AMD adapters by description.
    int ai=-1, bi=-1;
    for (int i=0;i<n;i++){
        DXGI_ADAPTER_DESC1 d{}; adp[i]->GetDesc1(&d);
        char s[256]; wcstombs(s,d.Description,255); s[254]=0;
        printf("  adapter[%d] LUID=%08x:%08x  %s\n", i, d.AdapterLuid.HighPart, (unsigned)d.AdapterLuid.LowPart, s);
    }

    // We'll just test the first two adapters that look like real GPUs.
    ID3D12Device* devA=nullptr,*devB=nullptr;
    int aIdx=-1,bIdx=-1;
    for (int i=0;i<n && (aIdx<0||bIdx<0);i++){
        DXGI_ADAPTER_DESC1 d{}; adp[i]->GetDesc1(&d);
        if (d.DedicatedVideoMemory < 2ULL*1024*1024*1024) continue; // skip iGPU/low-mem
        ID3D12Device* dv=nullptr;
        HRESULT hr = D3D12CreateDevice((IUnknown*)adp[i], D3D_FEATURE_LEVEL_12_0, __uuidof(ID3D12Device), (void**)&dv);
        if (FAILED(hr)) continue;
        if (aIdx<0){ aIdx=i; devA=dv; } else { bIdx=i; devB=dv; }
    }
    if (!devA || !devB){ printf("need 2 discrete GPUs\n"); return 1; }

    PrintOpts(devA, "GPU_A");
    PrintOpts(devB, "GPU_B");

    // Test: can we create a plain committed resource with ALLOW_CROSS_ADAPTER?
    for (int which=0; which<2; ++which) {
        ID3D12Device* dev = which==0?devA:devB;
        const char* nm = which==0?"GPU_A":"GPU_B";
        D3D12_HEAP_PROPERTIES hp{}; hp.Type=D3D12_HEAP_TYPE_DEFAULT;
        D3D12_RESOURCE_DESC rd{}; rd.Dimension=D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width=4096; rd.Height=1; rd.DepthOrArraySize=1; rd.MipLevels=1;
        rd.Layout=D3D12_TEXTURE_LAYOUT_ROW_MAJOR; rd.SampleDesc.Count=1;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
        ID3D12Resource* r=nullptr;
        HRESULT hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&r));
        printf("  %s CreateCommittedResource(ALLOW_CROSS_ADAPTER) -> 0x%08X\n", nm, (unsigned)hr);
        if (r) r->Release();

        // Also test a texture with the flag.
        D3D12_RESOURCE_DESC td{}; td.Dimension=D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        td.Width=64; td.Height=64; td.DepthOrArraySize=1; td.MipLevels=1;
        td.Format=DXGI_FORMAT_R8G8B8A8_UNORM; td.Layout=D3D12_TEXTURE_LAYOUT_UNKNOWN;
        td.SampleDesc.Count=1; td.Flags=D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
        ID3D12Resource* t=nullptr;
        HRESULT hr2 = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                        D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&t));
        printf("  %s CreateCommittedResource(TEXTURE+ALLOW_CROSS_ADAPTER) -> 0x%08X\n", nm, (unsigned)hr2);
        if (t) t->Release();
    }

    // Affinity factory (IDXGIAffinityFactory) is NOT in this SDK version — noted separately.
    printf("  NOTE: IDXGIAffinityFactory not present in Windows SDK 10.0.26100 headers.\n");

    devA->Release(); devB->Release(); f->Release();
    printf("=== done ===\n");
    return 0;
}

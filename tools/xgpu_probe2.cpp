// xgpu_probe2.cpp - isolate which cross-adapter configurations work on this AMD dual-GPU box
#include <d3d12.h>
#include <dxgi1_6.h>
#include <stdio.h>
#include <cstdint>

static ID3D12Device* CreateDeviceByLuid(ULONG hi, ULONG lo) {
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return nullptr;
    for (UINT i = 0; ; ++i) {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapters1(i, &adapter) != S_OK) break;
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (desc.AdapterLuid.HighPart == hi && desc.AdapterLuid.LowPart == lo) {
            ID3D12Device* dev = nullptr;
            D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev));
            adapter->Release(); factory->Release();
            return dev;
        }
        adapter->Release();
    }
    factory->Release();
    return nullptr;
}

static void TryHeap(ID3D12Device* dev, const char* label, D3D12_HEAP_DESC hd) {
    ID3D12Heap* heap = nullptr;
    HRESULT hr = dev->CreateHeap(&hd, IID_PPV_ARGS(&heap));
    printf("  %-45s CreateHeap: %s\n", label, SUCCEEDED(hr) ? "OK" : "FAIL");
    if (FAILED(hr)) { printf("      hr=0x%08X\n", (unsigned)hr); return; }

    // buffer on heap (MUST have ALLOW_CROSS_ADAPTER to be placed on cross-adapter heaps)
    D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = 4096; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
    ID3D12Resource* buf = nullptr;
    hr = dev->CreatePlacedResource(heap, 0, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&buf));
    printf("      placed BUFFER (xadapter flag): %s\n", SUCCEEDED(hr) ? "OK" : "FAIL");
    if (FAILED(hr)) printf("      hr=0x%08X\n", (unsigned)hr);
    if (buf) buf->Release();

    // texture on heap
    D3D12_RESOURCE_DESC td{}; td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = 512; td.Height = 288; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
    ID3D12Resource* tex = nullptr;
    hr = dev->CreatePlacedResource(heap, 0, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&tex));
    printf("      placed TEXTURE2D RGBA8 (xadapter flag): %s\n", SUCCEEDED(hr) ? "OK" : "FAIL");
    if (FAILED(hr)) printf("      hr=0x%08X\n", (unsigned)hr);
    if (tex) tex->Release();

    // committed cross-adapter texture (row-major 2D allowed per docs)
    D3D12_RESOURCE_DESC td2 = td;
    ID3D12Resource* ctex = nullptr;
    hr = dev->CreateCommittedResource(&hd.Properties, hd.Flags & ~D3D12_HEAP_FLAG_ALLOW_ALL_BUFFERS_AND_TEXTURES, &td2, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&ctex));
    printf("      committed TEXTURE2D (xadapter): %s\n", SUCCEEDED(hr) ? "OK" : "FAIL");
    if (FAILED(hr)) printf("      hr=0x%08X\n", (unsigned)hr);
    if (ctex) ctex->Release();

    heap->Release();
}

int main() {
    ID3D12Device* dev = CreateDeviceByLuid(0, 0x2A089); // 7900 XTX
    if (!dev) { printf("no device\n"); return 1; }

    D3D12_HEAP_DESC hd{};
    hd.SizeInBytes = 8 * 1024 * 1024;

    printf("== Cross-adapter heap tests (7900 XTX) ==\n");
    hd.Properties.Type = D3D12_HEAP_TYPE_CUSTOM;
    hd.Properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE;
    hd.Properties.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
    hd.Flags = D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER | D3D12_HEAP_FLAG_ALLOW_ALL_BUFFERS_AND_TEXTURES;
    TryHeap(dev, "CUSTOM L0 + SHARED_XADAPTER", hd);

    printf("== Control: plain shared heap (same adapter) ==\n");
    hd.Properties = {}; // DEFAULT type must not carry explicit pool/page props
    hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    hd.Flags = D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_ALLOW_ALL_BUFFERS_AND_TEXTURES;
    TryHeap(dev, "DEFAULT + SHARED (no xadapter)", hd);

    printf("== Control: default shared heap ==\n");
    hd.Properties = {};
    hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    hd.Flags = D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_ALLOW_ALL_BUFFERS_AND_TEXTURES;
    TryHeap(dev, "DEFAULT + SHARED", hd);

    printf("== Variant: DEFAULT + SHARED_CROSS_ADAPTER ==\n");
    hd.Properties = {};
    hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
    hd.Flags = D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER | D3D12_HEAP_FLAG_ALLOW_ALL_BUFFERS_AND_TEXTURES;
    TryHeap(dev, "DEFAULT + SHARED_XADAPTER", hd);

    printf("== Variant: CUSTOM L0 WC (write-combine) + SHARED_CROSS_ADAPTER ==\n");
    hd.Properties.Type = D3D12_HEAP_TYPE_CUSTOM;
    hd.Properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_COMBINE;
    hd.Properties.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
    hd.Flags = D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER | D3D12_HEAP_FLAG_ALLOW_ALL_BUFFERS_AND_TEXTURES;
    TryHeap(dev, "CUSTOM L0 WC + SHARED_XADAPTER", hd);

    dev->Release();
    return 0;
}

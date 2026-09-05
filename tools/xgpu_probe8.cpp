// xgpu_probe8.cpp — Why does readback-buffer Map fail in probe4 but not probe7?
// Replicates probe4's exact device-creation path (CreateDeviceByLuid) and tests
// Map on a READBACK buffer at several points: right after create, after a GPU write.
#include <d3d12.h>
#include <dxgi1_6.h>
#include <combaseapi.h>
#include <stdio.h>
#include <string.h>
#include <cstdint>

static ID3D12Device* CreateDeviceByLuid(ULONG hi, ULONG lo) {
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return nullptr;
    for (UINT i = 0; ; ++i) {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapters1(i, &adapter) != S_OK) break;
        DXGI_ADAPTER_DESC1 desc{}; adapter->GetDesc1(&desc);
        if (desc.AdapterLuid.HighPart == hi && desc.AdapterLuid.LowPart == lo) {
            ID3D12Device* dev = nullptr;
            D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev));
            adapter->Release(); factory->Release(); return dev;
        }
        adapter->Release();
    }
    factory->Release(); return nullptr;
}

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    ID3D12Device* dev = CreateDeviceByLuid(0, 0x2A089); // 7900 XTX via LUID (probe4 path)
    if (!dev) { printf("no device\n"); return 1; }

    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* q = nullptr; dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q));
    ID3D12CommandAllocator* alloc = nullptr; dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
    ID3D12GraphicsCommandList* cl = nullptr; dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&cl));
    ID3D12Fence* fence = nullptr; dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    HANDLE ev = CreateEvent(nullptr, TRUE, FALSE, nullptr);

    const SIZE_T BYTES = 589824; // same as probe4 (512*288*4)
    D3D12_HEAP_PROPERTIES rb{}; rb.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = BYTES; bd.Height=1; bd.DepthOrArraySize=1; bd.MipLevels=1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; bd.SampleDesc.Count = 1;

    ID3D12Resource* rbuf = nullptr;
    HRESULT hr = dev->CreateCommittedResource(&rb, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rbuf));
    printf("create readback %llu B COPY_DEST: 0x%08X\n", (unsigned long long)BYTES, (unsigned)hr);

    void* p = nullptr; D3D12_RANGE rng{0,(SIZE_T)-1};
    HRESULT hm = rbuf->Map(0, &rng, &p);
    printf("map right after create (COPY_DEST), range={0,-1}: 0x%08X\n", (unsigned)hm);

    // Retry with an EXPLICIT byte range (probe7 used {0,4096}).
    D3D12_RANGE rng2{0, BYTES};
    hm = rbuf->Map(0, &rng2, &p);
    printf("map right after create (COPY_DEST), range={0,BYTES}: 0x%08X\n", (unsigned)hm);

    // Now do a GPU write: upload buffer -> this readback? No—readback can't be copy src.
    // Instead: create an UPLOAD buffer, fill it, CopyBufferRegion into rbuf, wait, then Map.
    D3D12_HEAP_PROPERTIES up{}; up.Type = D3D12_HEAP_TYPE_UPLOAD;
    ID3D12Resource* ubuf = nullptr;
    dev->CreateCommittedResource(&up, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&ubuf));
    void* mp = nullptr; ubuf->Map(0,nullptr,&mp); memset(mp, 0xAB, BYTES); ubuf->Unmap(0,nullptr);

    cl->Reset(alloc, nullptr);
    cl->CopyBufferRegion(rbuf, 0, ubuf, 0, BYTES);
    cl->Close();
    { ID3D12CommandList* lists[] = { cl }; q->ExecuteCommandLists(1, lists); }
    q->Signal(fence, 1); fence->SetEventOnCompletion(1, ev); WaitForSingleObject(ev, 5000);

    hm = rbuf->Map(0, &rng2, &p);
    printf("map after GPU CopyBufferRegion write: 0x%08X\n", (unsigned)hm);
    if (SUCCEEDED(hm)) { printf("  first byte = %02X (expect AB)\n", ((uint8_t*)p)[0]); rbuf->Unmap(0,nullptr); }

    return 0;
}

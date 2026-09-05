// reset_test.cpp — instrumented probe4 replica: check every HRESULT, verify copy still works
// even if Reset returns E_FAIL (suspected AMD driver quirk on this box).
#include <d3d12.h>
#include <dxgi1_6.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

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
    ID3D12Device* devA = CreateDeviceByLuid(0, 0x2A089);
    ID3D12Device* devB = CreateDeviceByLuid(0, 0x27214);
    if (!devA || !devB) { printf("no devices\n"); return 1; }

    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* queueA = nullptr, *queueB = nullptr;
    devA->CreateCommandQueue(&qd, IID_PPV_ARGS(&queueA));
    devB->CreateCommandQueue(&qd, IID_PPV_ARGS(&queueB));

    const UINT W = 512, H = 288; SIZE_T BYTES = (SIZE_T)W*H*4;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = W; rd.Height = H; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM; rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN; rd.SampleDesc.Count = 1;
    D3D12_HEAP_PROPERTIES defProps{}; defProps.Type = D3D12_HEAP_TYPE_DEFAULT;
    ID3D12Resource* texA = nullptr, *texB = nullptr;
    devA->CreateCommittedResource(&defProps, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texA));
    devB->CreateCommittedResource(&defProps, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texB));

    D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = (UINT)BYTES; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; bd.SampleDesc.Count = 1;
    D3D12_HEAP_PROPERTIES upProps{}; upProps.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_HEAP_PROPERTIES rbProps{}; rbProps.Type = D3D12_HEAP_TYPE_READBACK;
    ID3D12Resource* uploadA = nullptr, *stagingA = nullptr, *uploadB = nullptr, *readbackB = nullptr;
    devA->CreateCommittedResource(&upProps, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadA));
    devA->CreateCommittedResource(&rbProps, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&stagingA));
    devB->CreateCommittedResource(&upProps, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadB));
    devB->CreateCommittedResource(&rbProps, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readbackB));

    uint8_t* pat = new uint8_t[BYTES];
    for (UINT y = 0; y < H; ++y) for (UINT x = 0; x < W; ++x) {
        uint8_t* p = pat + ((SIZE_T)(y*W+x))*4;
        p[0] = (uint8_t)x; p[1] = (uint8_t)y; p[2] = (uint8_t)(x^y); p[3] = 0xFF;
    }

    ID3D12CommandAllocator* allocA = nullptr, *allocB = nullptr;
    devA->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocA));
    devB->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocB));
    ID3D12GraphicsCommandList* clA = nullptr, *clB = nullptr;
    devA->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocA, nullptr, IID_PPV_ARGS(&clA));
    devB->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocB, nullptr, IID_PPV_ARGS(&clB));
    ID3D12Fence* fenceA = nullptr, *fenceB = nullptr;
    devA->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fenceA));
    devB->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fenceB));
    HANDLE evA = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    HANDLE evB = CreateEvent(nullptr, TRUE, FALSE, nullptr);

    void* mUploadA = nullptr; uploadA->Map(0, nullptr, &mUploadA); memcpy(mUploadA, pat, BYTES); uploadA->Unmap(0, nullptr);

    HRESULT hr = clA->Reset(allocA, nullptr);
    printf("clA->Reset: 0x%08X\n", (unsigned)hr);
    auto barrier = [](ID3D12GraphicsCommandList* c, ID3D12Resource* r, D3D12_RESOURCE_STATES f, D3D12_RESOURCE_STATES t){
        D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = r; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = f; b.Transition.StateAfter = t; c->ResourceBarrier(1, &b); };
    auto bufLoc = [&](ID3D12Resource* b){ D3D12_TEXTURE_COPY_LOCATION l{}; l.pResource=b; l.Type=D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        l.PlacedFootprint.Footprint.Format=DXGI_FORMAT_R8G8B8A8_UNORM; l.PlacedFootprint.Footprint.Width=W;
        l.PlacedFootprint.Footprint.Height=H; l.PlacedFootprint.Footprint.Depth=1; l.PlacedFootprint.Footprint.RowPitch=(UINT)(W*4); return l; };
    auto texLoc = [&](ID3D12Resource* t){ D3D12_TEXTURE_COPY_LOCATION l{}; l.pResource=t; l.Type=D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; l.SubresourceIndex=0; return l; };

    barrier(clA, uploadA, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_COPY_SOURCE);
    clA->CopyTextureRegion(&texLoc(texA), 0,0,0, &bufLoc(uploadA), nullptr);
    barrier(clA, uploadA, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_GENERIC_READ);
    barrier(clA, texA, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    clA->CopyTextureRegion(&bufLoc(stagingA), 0,0,0, &texLoc(texA), nullptr);
    barrier(clA, texA, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    hr = clA->Close(); printf("clA->Close: 0x%08X\n", (unsigned)hr);
    ID3D12CommandList* ls[] = { clA }; queueA->ExecuteCommandLists(1, ls);
    queueA->Signal(fenceA, 1); fenceA->SetEventOnCompletion(1, evA); WaitForSingleObject(evA, 30000);

    D3D12_RANGE r{0, BYTES}; void* mStg = nullptr, *mUpB = nullptr;
    stagingA->Map(0, &r, &mStg); uploadB->Map(0, nullptr, &mUpB); memcpy(mUpB, mStg, BYTES);
    uploadB->Unmap(0, nullptr); stagingA->Unmap(0, nullptr);

    hr = clB->Reset(allocB, nullptr); printf("clB->Reset: 0x%08X\n", (unsigned)hr);
    barrier(clB, uploadB, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_COPY_SOURCE);
    clB->CopyTextureRegion(&texLoc(texB), 0,0,0, &bufLoc(uploadB), nullptr);
    barrier(clB, uploadB, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_GENERIC_READ);
    barrier(clB, texB, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    clB->CopyTextureRegion(&bufLoc(readbackB), 0,0,0, &texLoc(texB), nullptr);
    barrier(clB, texB, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    hr = clB->Close(); printf("clB->Close: 0x%08X\n", (unsigned)hr);
    ID3D12CommandList* ls2[] = { clB }; queueB->ExecuteCommandLists(1, ls2);
    queueB->Signal(fenceB, 1); fenceB->SetEventOnCompletion(1, evB); WaitForSingleObject(evB, 30000);

    void* mRb = nullptr; readbackB->Map(0, &r, &mRb);
    int mismatches = 0;
    for (SIZE_T i = 0; i < BYTES; ++i) if (((uint8_t*)mRb)[i] != pat[i]) { if (mismatches<3) printf("MISMATCH %llu\n",(unsigned long long)i); mismatches++; }
    readbackB->Unmap(0, nullptr);
    printf(mismatches == 0 ? "PASS: copy worked despite Reset hr\n" : "FAIL: %d mismatches\n", mismatches);
    return mismatches == 0 ? 0 : 2;
}

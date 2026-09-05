// xgpu_test.cpp - CRITICAL FEASIBILITY TEST
// Creates D3D12 devices on two different AMD adapters (by LUID), writes a known
// pattern to a texture on GPU A, transfers it cross-adapter via shared heap +
// CreateSharedHandle/OpenSharedResource, reads back on GPU B and verifies pixels.
#include <d3d12.h>
#include <dxgi1_6.h>
#include <stdio.h>
#include <string.h>
#include <cstdint>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

static ID3D12Device* CreateDeviceByLuid(ULONG longHigh, ULONG longLow) {
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return nullptr;
    for (UINT i = 0; ; ++i) {
        IDXGIAdapter1* adapter = nullptr;
        if (factory->EnumAdapters1(i, &adapter) != S_OK) break;
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        if (desc.AdapterLuid.HighPart == longHigh && desc.AdapterLuid.LowPart == longLow) {
            ID3D12Device* dev = nullptr;
            HRESULT hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev));
            char nameA[512]{};
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, nameA, sizeof(nameA), nullptr, nullptr);
            printf("  matched adapter %u: %s -> D3D12 device %s\n", i, nameA, SUCCEEDED(hr) ? "OK" : "FAILED");
            adapter->Release();
            factory->Release();
            return dev;
        }
        adapter->Release();
    }
    factory->Release();
    return nullptr;
}

int main(int argc, char** argv) {
    // LUIDs from device_probe (user's machine): 9060 XT = 27214, 7900 XTX = 2A089
    ULONG luidB_hi = 0, luidB_lo = 0x27214; // upscale GPU: 9060 XT (default)
    ULONG luidA_hi = 0, luidA_lo = 0x2A089; // raster GPU: 7900 XTX (default)
    if (argc >= 3) { sscanf(argv[1], "%lx", &luidB_lo); sscanf(argv[2], "%lx", &luidA_lo); }

    const UINT W = 512, H = 288; // typical low-res render target size

    printf("== Creating devices ==\n");
    ID3D12Device* devA = CreateDeviceByLuid(luidA_hi, luidA_lo); // raster (7900 XTX)
    ID3D12Device* devB = CreateDeviceByLuid(luidB_hi, luidB_lo); // upscale (9060 XT)
    if (!devA || !devB) { printf("FAILED to create devices\n"); return 1; }

    // Queues: A renders+copies out, B copies in + reads back
    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* queueA = nullptr, *queueB = nullptr;
    devA->CreateCommandQueue(&qd, IID_PPV_ARGS(&queueA));
    devB->CreateCommandQueue(&qd, IID_PPV_ARGS(&queueB));

    // ---- GPU A: create texture on a CROSS-ADAPTER shared heap (L0 system memory) ----
    printf("== Creating cross-adapter shared heap on GPU A ==\n");
    D3D12_HEAP_DESC hd{};
    hd.SizeInBytes = 64 * 1024 * 1024; // plenty for test + real frames (1080p RGBA ~9MB)
    hd.Properties.Type = D3D12_HEAP_TYPE_CUSTOM;
    hd.Properties.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_NOT_AVAILABLE;
    hd.Properties.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
    hd.Flags = D3D12_HEAP_FLAG_SHARED | D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER | D3D12_HEAP_FLAG_ALLOW_ALL_BUFFERS_AND_TEXTURES;

    ID3D12Heap* heapA = nullptr;
    HRESULT hr = devA->CreateHeap(&hd, IID_PPV_ARGS(&heapA));
    if (FAILED(hr)) {
        printf("CreateHeap CUSTOM/L0 cross-adapter FAILED 0x%08X - trying DEFAULT\n", (unsigned)hr);
        hd.Properties = {}; // zeroed: let driver pick pool for DEFAULT type
        hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        hr = devA->CreateHeap(&hd, IID_PPV_ARGS(&heapA));
    }
    if (FAILED(hr)) { printf("CreateHeap cross-adapter FAILED 0x%08X\n", (unsigned)hr); return 1; }
    printf("  heap created OK\n");

    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = W; rd.Height = H; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER; // no RT flag: cross-adapter heaps may restrict layouts

    ID3D12Resource* texA = nullptr;
    // Try placed resource first, fall back to committed cross-adapter (row-major 2D only)
    hr = devA->CreatePlacedResource(heapA, 0, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texA));
    if (FAILED(hr)) {
        printf("  CreatePlacedResource FAILED 0x%08X - trying committed cross-adapter\n", (unsigned)hr);
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER;
        hr = devA->CreateCommittedResource(&hd.Properties, hd.Flags & ~D3D12_HEAP_FLAG_ALLOW_ALL_BUFFERS_AND_TEXTURES, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texA));
    }
    if (FAILED(hr)) { printf("CreatePlacedResource FAILED 0x%08X\n", (unsigned)hr); return 1; }
    printf("  texture on GPU A OK (%ux%u RGBA8)\n", W, H);

    // ---- Write a known pattern via CPU-visible staging buffer on A ----
    D3D12_HEAP_PROPERTIES cpuProps{}; cpuProps.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = W * H * 4; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    ID3D12Resource* uploadA = nullptr;
    devA->CreateCommittedResource(&cpuProps, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadA));

    // Pattern: gradient + checkerboard (deterministic)
    uint8_t* pat = new uint8_t[W * H * 4];
    for (UINT y = 0; y < H; ++y)
        for (UINT x = 0; x < W; ++x) {
            uint8_t* p = pat + (y * W + x) * 4;
            p[0] = (uint8_t)(x & 0xFF);
            p[1] = (uint8_t)(y & 0xFF);
            p[2] = (uint8_t)((x ^ y) & 0xFF);
            p[3] = ((x / 64 + y / 64) % 2) ? 0xFF : 0x55; // checkerboard alpha
        }

    ID3D12CommandAllocator* allocA = nullptr, *allocB = nullptr;
    devA->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocA));
    devB->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocB));
    ID3D12GraphicsCommandList* clA = nullptr, *clB = nullptr;
    devA->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocA, nullptr, IID_PPV_ARGS(&clA));
    devB->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocB, nullptr, IID_PPV_ARGS(&clB));

    void* mapped = nullptr;
    uploadA->Map(0, nullptr, &mapped);
    memcpy(mapped, pat, W * H * 4);
    uploadA->Unmap(0, nullptr);

    D3D12_TEXTURE_COPY_LOCATION dstA{texA, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}};
    D3D12_TEXTURE_COPY_LOCATION srcBufA{uploadA, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}};
    clA->CopyTextureRegion(&dstA, 0, 0, 0, &srcBufA, nullptr);
    clA->Close();
    ID3D12Fence* fenceA = nullptr; devA->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fenceA));
    { ID3D12CommandList* lists[] = { clA }; queueA->ExecuteCommandLists(1, lists); }
    queueA->Signal(fenceA, 1);
    HANDLE evA = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    fenceA->SetEventOnCompletion(1, evA);
    WaitForSingleObject(evA, 30000);
    CloseHandle(evA);

    // ---- Share the texture with GPU B ----
    printf("== Sharing handle A -> B ==\n");
    HANDLE shared = nullptr;
    hr = devA->CreateSharedHandle(texA, nullptr, GENERIC_ALL, nullptr, &shared);
    if (FAILED(hr)) { printf("CreateSharedHandle FAILED 0x%08X\n", (unsigned)hr); return 1; }

    ID3D12Resource* texB = nullptr;
    hr = devB->OpenSharedHandle(shared, IID_PPV_ARGS(&texB));
    if (FAILED(hr)) { printf("OpenSharedResource on GPU B FAILED 0x%08X\n", (unsigned)hr); return 1; }
    CloseHandle(shared);
    printf("  opened on GPU B OK\n");

    // ---- Read back from GPU B via its own staging buffer ----
    ID3D12Resource* readbackB = nullptr;
    D3D12_HEAP_PROPERTIES rbProps{}; rbProps.Type = D3D12_HEAP_TYPE_READBACK;
    devB->CreateCommittedResource(&rbProps, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readbackB));

    D3D12_TEXTURE_COPY_LOCATION dstB{readbackB, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}};
    D3D12_TEXTURE_COPY_LOCATION srcTexB{texB, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {0}};
    clB->CopyTextureRegion(&dstB, 0, 0, 0, &srcTexB, nullptr);
    clB->Close();
    ID3D12Fence* fenceB = nullptr; devB->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fenceB));
    { ID3D12CommandList* lists[] = { clB }; queueB->ExecuteCommandLists(1, lists); }
    queueB->Signal(fenceB, 1);
    HANDLE ev3 = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    fenceB->SetEventOnCompletion(1, ev3);
    WaitForSingleObject(ev3, 30000);
    CloseHandle(ev3);

    uint8_t* out = new uint8_t[W * H * 4];
    D3D12_RANGE range{0, W*H*4};
    readbackB->Map(0, &range, (void**)&out);

    // ---- Verify ----
    int mismatches = 0;
    for (size_t i = 0; i < (size_t)W * H * 4; ++i)
        if (out[i] != pat[i]) {
            if (mismatches < 5) printf("  MISMATCH at byte %zu: got %02X want %02X\n", i, out[i], pat[i]);
            mismatches++;
        }
    readbackB->Unmap(0, nullptr);

    printf("== RESULT ==\n");
    if (mismatches == 0) {
        printf("PASS: all %u bytes identical across GPUs (%ux%u RGBA8)\n", W*H*4, W, H);
    } else {
        printf("FAIL: %d / %u bytes mismatched\n", mismatches, W*H*4);
    }

    delete[] pat; delete[] out;
    return mismatches == 0 ? 0 : 2;
}

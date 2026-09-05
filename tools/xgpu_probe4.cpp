// xgpu_probe4.cpp — VALIDATE THE CROSS-GPU TRANSFER PATH (CPU RAM bounce).
// Cross-adapter shared heaps are NOT supported on this hardware (probe3: tier 0),
// so the real design is: GPU A texture -> readback buffer (A) -> Map/memcpy to CPU
// -> upload buffer (B, mapped) -> GPU B texture. This test proves a pixel-perfect
// round-trip A->B and measures each stage's latency at 720p/1080p sizes.
//
// DRIVER QUIRKS DISCOVERED ON THIS BOX (AMD RDNA3/RDNA4, Win11 26200):
//   * CopyTextureRegion with a BUFFER as source via SUBRESOURCE_INDEX -> INVALID_CALL.
//     MUST use PLACED_FOOTPRINT for buffer<->texture copies (probe13/14).
//   * Map() range {0, SIZE_MAX} is rejected; pass an explicit byte count or NULL (probe8).
//   * READBACK buffers can be created in COPY_DEST and still Map fine (no barrier needed).
#include <d3d12.h>
#include <dxgi1_6.h>
#include <combaseapi.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <windows.h>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

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
            HRESULT hr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev));
            char nameA[512]{};
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, nameA, sizeof(nameA), nullptr, nullptr);
            printf("  matched adapter %u: %s -> D3D12 device %s\n", i, nameA, SUCCEEDED(hr) ? "OK" : "FAILED");
            adapter->Release(); factory->Release();
            return dev;
        }
        adapter->Release();
    }
    factory->Release();
    return nullptr;
}

static double NowMs() {
    LARGE_INTEGER f, c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)f.QuadPart;
}

// Buffer <-> texture copy location using PLACED_FOOTPRINT (required on this driver).
static D3D12_TEXTURE_COPY_LOCATION BufLoc(ID3D12Resource* buf, UINT w, UINT h) {
    D3D12_TEXTURE_COPY_LOCATION loc{};
    loc.pResource = buf;
    loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    loc.PlacedFootprint.Offset = 0;
    loc.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    loc.PlacedFootprint.Footprint.Width = w;
    loc.PlacedFootprint.Footprint.Height = h;
    loc.PlacedFootprint.Footprint.Depth = 1;
    loc.PlacedFootprint.Footprint.RowPitch = (UINT)(w * 4); // RGBA8, no padding needed at these widths
    return loc;
}

static D3D12_TEXTURE_COPY_LOCATION TexLoc(ID3D12Resource* tex) {
    D3D12_TEXTURE_COPY_LOCATION loc{};
    loc.pResource = tex;
    loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    loc.SubresourceIndex = 0;
    return loc;
}

static void Barrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* r,
                    D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from; b.Transition.StateAfter = to;
    cl->ResourceBarrier(1, &b);
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    ULONG luidA_hi = 0, luidA_lo = 0x2A089; // raster GPU: 7900 XTX
    ULONG luidB_hi = 0, luidB_lo = 0x27214; // upscale GPU: 9060 XT
    if (argc >= 3) { sscanf(argv[1], "%lx", &luidA_lo); sscanf(argv[2], "%lx", &luidB_lo); }

    const UINT W = 512, H = 288; // low-res render target size
    const SIZE_T BYTES = (SIZE_T)W * H * 4;

    printf("== xgpu_probe4: RAM-bounce transfer A(7900XTX)->B(9060XT) ==\n");
    ID3D12Device* devA = CreateDeviceByLuid(luidA_hi, luidA_lo);
    ID3D12Device* devB = CreateDeviceByLuid(luidB_hi, luidB_lo);
    if (!devA || !devB) { printf("FAILED to create devices\n"); return 1; }

    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* queueA = nullptr, *queueB = nullptr;
    devA->CreateCommandQueue(&qd, IID_PPV_ARGS(&queueA));
    devB->CreateCommandQueue(&qd, IID_PPV_ARGS(&queueB));

    // ---- GPU A: texture + upload buffer (pattern source) + readback/staging buffer ----
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = W; rd.Height = H; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    rd.SampleDesc.Count = 1;

    D3D12_HEAP_PROPERTIES defProps{}; defProps.Type = D3D12_HEAP_TYPE_DEFAULT;
    ID3D12Resource* texA = nullptr;
    if (FAILED(devA->CreateCommittedResource(&defProps, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texA)))) { printf("texA fail\n"); return 1; }

    D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = BYTES; bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bd.SampleDesc.Count = 1; // REQUIRED — zero-initialized desc has Count=0 -> E_INVALIDARG

    D3D12_HEAP_PROPERTIES upProps{}; upProps.Type = D3D12_HEAP_TYPE_UPLOAD;
    ID3D12Resource* uploadA = nullptr, *stagingA = nullptr;
    devA->CreateCommittedResource(&upProps, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadA));
    D3D12_HEAP_PROPERTIES rbProps{}; rbProps.Type = D3D12_HEAP_TYPE_READBACK;
    devA->CreateCommittedResource(&rbProps, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&stagingA));

    // ---- GPU B: upload buffer (CPU-writable) + texture + readback buffer ----
    ID3D12Resource* uploadB = nullptr, *texB = nullptr, *readbackB = nullptr;
    devB->CreateCommittedResource(&upProps, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&uploadB));
    if (FAILED(devB->CreateCommittedResource(&defProps, D3D12_HEAP_FLAG_NONE, &rd,
            D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&texB)))) { printf("texB fail\n"); return 1; }
    devB->CreateCommittedResource(&rbProps, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&readbackB));

    // ---- Pattern: deterministic gradient + checkerboard ----
    uint8_t* pat = new uint8_t[BYTES];
    for (UINT y = 0; y < H; ++y)
        for (UINT x = 0; x < W; ++x) {
            uint8_t* p = pat + ((SIZE_T)(y * W + x)) * 4;
            p[0] = (uint8_t)(x & 0xFF);
            p[1] = (uint8_t)(y & 0xFF);
            p[2] = (uint8_t)((x ^ y) & 0xFF);
            p[3] = ((x / 64 + y / 64) % 2) ? 0xFF : 0x55;
        }

    // ---- Command allocators/lists/fences/events for both devices ----
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

    // Write the pattern into GPU A's upload buffer (source of truth for texA).
    void* mUploadA = nullptr;
    uploadA->Map(0, nullptr, &mUploadA);
    memcpy(mUploadA, pat, BYTES);
    uploadA->Unmap(0, nullptr);

    // ---- Single-pass transfer with per-stage timing ----
    double tA = NowMs();
    clA->Reset(allocA, nullptr);
    Barrier(clA, uploadA, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_COPY_SOURCE);
    clA->CopyTextureRegion(&TexLoc(texA), 0, 0, 0, &BufLoc(uploadA, W, H), nullptr); // buf -> texA (PLACED_FOOTPRINT)
    Barrier(clA, uploadA, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_GENERIC_READ);
    Barrier(clA, texA, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    clA->CopyTextureRegion(&BufLoc(stagingA, W, H), 0, 0, 0, &TexLoc(texA), nullptr); // texA -> staging (readback)
    Barrier(clA, texA, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    clA->Close();
    { ID3D12CommandList* lists[] = { clA }; queueA->ExecuteCommandLists(1, lists); }
    queueA->Signal(fenceA, 1); fenceA->SetEventOnCompletion(1, evA); WaitForSingleObject(evA, 30000); // wait for A
    double tB = NowMs();

    // CPU hop: read stagingA, write into uploadB. (explicit byte range — driver rejects {0,-1})
    void* mStgA = nullptr, *mUpB = nullptr; D3D12_RANGE r{0, BYTES};
    HRESULT hms = stagingA->Map(0, &r, &mStgA);
    if (FAILED(hms)) { printf("stagingA Map FAIL 0x%08X\n",(unsigned)hms); return 3; }
    HRESULT hmup = uploadB->Map(0, nullptr, &mUpB);
    if (FAILED(hmup)) { printf("uploadB Map FAIL 0x%08X\n",(unsigned)hmup); return 3; }
    memcpy(mUpB, mStgA, BYTES);   // <-- the actual cross-GPU transfer (via system RAM)
    uploadB->Unmap(0, nullptr);
    stagingA->Unmap(0, nullptr);
    double tC = NowMs();

    clB->Reset(allocB, nullptr);
    Barrier(clB, uploadB, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_COPY_SOURCE);
    clB->CopyTextureRegion(&TexLoc(texB), 0, 0, 0, &BufLoc(uploadB, W, H), nullptr); // buf -> texB (PLACED_FOOTPRINT)
    Barrier(clB, uploadB, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_GENERIC_READ);
    Barrier(clB, texB, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COPY_SOURCE);
    clB->CopyTextureRegion(&BufLoc(readbackB, W, H), 0, 0, 0, &TexLoc(texB), nullptr); // texB -> readback (verify)
    Barrier(clB, texB, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COPY_DEST);
    clB->Close();
    { ID3D12CommandList* lists[] = { clB }; queueB->ExecuteCommandLists(1, lists); }
    queueB->Signal(fenceB, 1); fenceB->SetEventOnCompletion(1, evB); WaitForSingleObject(evB, 30000); // wait for B
    double tD = NowMs();

    void* mRbB = nullptr;
    HRESULT hmr = readbackB->Map(0, &r, &mRbB);
    if (FAILED(hmr)) { printf("readbackB Map FAIL 0x%08X\n",(unsigned)hmr); return 3; }

    int mismatches = 0;
    for (SIZE_T i = 0; i < BYTES; ++i) {
        if (((uint8_t*)mRbB)[i] != pat[i]) {
            if (mismatches < 5) printf("  MISMATCH at byte %llu: got %02X want %02X\n",
                (unsigned long long)i, ((uint8_t*)mRbB)[i], pat[i]);
            mismatches++;
        }
    }
    readbackB->Unmap(0, nullptr);

    printf("== RESULT ==\n");
    if (mismatches == 0) {
        printf("PASS: all %llu bytes identical GPU A -> CPU RAM -> GPU B (%ux%u RGBA8)\n",
            (unsigned long long)BYTES, W, H);
    } else {
        printf("FAIL: %d / %llu bytes mismatched\n", mismatches, (unsigned long long)BYTES);
    }

    double gpuA_ms = tB - tA;      // A: upload + readback copy (GPU time)
    double cpu_ms  = tC - tB;      // CPU memcpy hop (the cross-GPU transfer)
    double gpuB_ms = tD - tC;      // B: upload + verify copy (GPU time)
    printf("Timing (one frame, %llu bytes):\n", (unsigned long long)BYTES);
    printf("  GPU A readback path : %7.3f ms\n", gpuA_ms);
    printf("  CPU memcpy hop      : %7.3f ms   (~%.1f GB/s effective)\n", cpu_ms, BYTES/cpu_ms/1e6);
    printf("  GPU B upload path   : %7.3f ms\n", gpuB_ms);
    printf("  TOTAL one-way       : %7.3f ms\n", tD - tA);

    double mb720 = 1280.0*720*4/1e6, mb1080 = 1920.0*1080*4/1e6;
    printf("Extrapolated per frame: 720p=%.1f MB, 1080p=%.1f MB (CPU hop scales linearly)\n", mb720, mb1080);

    delete[] pat;
    CloseHandle(evA); CloseHandle(evB);
    return mismatches == 0 ? 0 : 2;
}

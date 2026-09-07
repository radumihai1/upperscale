// probe_generate_list.cpp — replicates the EXACT GPU-B command list Cyberpunk's first
// FRAMEGENERATION dispatch produces, WITHOUT any pipeline (no root signature needed):
//   1. input bounce for presentColor: upB -> mirrorIn [typeless B8G8R8A8 RTV|UAV], left in PSR|NPSR
//      (the PRESENT-state rewrite target)
//   2. FFX-sim write to output mirror: transition COMMON->UAV + ClearUnorderedView (leaves UAV)
//   3. our readback block: Barrier(mirrorOut, COMMON->COPY_SOURCE), CopyTextureRegion -> rbB footprint UNORM,
//      Barrier(COPY_SOURCE->UAV)
// If the device is removed here we've reproduced it; then bisect with skip flags.
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <stdio.h>

static ID3D12Device* g_dev = nullptr;

static void CheckRemoved(const char* what) {
    HRESULT r = g_dev->GetDeviceRemovedReason();
    printf("  after %-26s removed=0x%08X %s\n", what, (unsigned)r, r == S_OK ? "OK" : "<== DEVICE REMOVED");
}

static bool MakeDev(ID3D12Device** out) {
    IDXGIFactory4* f = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) return false;
    IDXGIAdapter1* adp = nullptr;
    for (UINT i = 0; SUCCEEDED(f->EnumAdapters1(i, &adp)); ++i) {
        DXGI_ADAPTER_DESC1 d; adp->GetDesc1(&d);
        if (_wcsicmp(d.Description, L"AMD Radeon RX 9060 XT") == 0) break;
        adp->Release(); adp = nullptr;
    }
    if (!adp) { printf("GPU B not found\n"); f->Release(); return false; }
    HRESULT hr = D3D12CreateDevice(adp, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(out));
    adp->Release(); f->Release();
    if (FAILED(hr)) { printf("device create failed 0x%08X\n", (unsigned)hr); return false; }
    g_dev = *out;
    return true;
}

static void Barrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* r, D3D12_RESOURCE_STATES before, D3D12_RESOURCE_STATES after) {
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = before;
    b.Transition.StateAfter = after;
    cl->ResourceBarrier(1, &b);
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    int skipIn = (argc > 1 && atoi(argv[1]) == 1);
    int skipFx = (argc > 2 && atoi(argv[2]) == 1);
    int skipRb = (argc > 3 && atoi(argv[3]) == 1);

    ID3D12Device* dev = nullptr;
    printf("step: MakeDev\n");
    if (!MakeDev(&dev)) return 1;
    printf("=== GENERATE-list replica on GPU B (skipIn=%d skipFx=%d skipRb=%d) ===\n", skipIn, skipFx, skipRb);

    const UINT W = 1024, H = 768;

    // upB: upload heap with "presentColor" pixels
    D3D12_RESOURCE_DESC bd{}; bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; bd.Width = (UINT64)W*H*4;
    bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1; bd.Format = DXGI_FORMAT_UNKNOWN; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; bd.SampleDesc.Count = 1;   // driver quirk: zero-init Count/Layout -> E_INVALIDARG
    D3D12_HEAP_PROPERTIES hpUp{}; hpUp.Type = D3D12_HEAP_TYPE_UPLOAD;
    ID3D12Resource* upB = nullptr;
    {
        HRESULT hr = dev->CreateCommittedResource(&hpUp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&upB));
        printf("step: upB create hr=0x%08X ptr=%p\n", (unsigned)hr, (void*)upB);
    }
    { void* p = nullptr; HRESULT hm = upB->Map(0, nullptr, &p); printf("step: upB map hr=0x%08X ptr=%p\n", (unsigned)hm, p); if (SUCCEEDED(hm)) { memset(p, 0x80, W*H*4); upB->Unmap(0, nullptr); } }

    // mirrorIn / mirrorOut: typeless B8G8R8A8 with RTV|UAV — exactly as GetOrCreateMirror makes them
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D; td.Width = W; td.Height = H;
    td.DepthOrArraySize = 1; td.MipLevels = 1; td.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS;
    td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource* mirrorIn = nullptr, *mirrorOut = nullptr;
    {
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        HRESULT h1 = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&mirrorIn));
        printf("step: mirrorIn create hr=0x%08X ptr=%p\n", (unsigned)h1, (void*)mirrorIn);
        HRESULT h2 = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&mirrorOut));
        printf("step: mirrorOut create hr=0x%08X ptr=%p\n", (unsigned)h2, (void*)mirrorOut);
    }

    // rbB: readback buffer created in COPY_DEST (exactly as GrowBuffer does)
    D3D12_RESOURCE_DESC brb{}; brb.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; brb.Width = (UINT64)W*H*4;
    brb.Height = 1; brb.DepthOrArraySize = 1; brb.MipLevels = 1; brb.Format = DXGI_FORMAT_UNKNOWN; brb.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; brb.SampleDesc.Count = 1;
    D3D12_HEAP_PROPERTIES hpRb{}; hpRb.Type = D3D12_HEAP_TYPE_READBACK;
    ID3D12Resource* rbB = nullptr;
    { HRESULT hrb = dev->CreateCommittedResource(&hpRb, D3D12_HEAP_FLAG_NONE, &brb, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&rbB)); printf("step: rbB create hr=0x%08X ptr=%p\n", (unsigned)hrb, (void*)rbB); }

    // descriptor heap kept for parity with in-game layout (FFX uses its own; we don't need views here)
    ID3D12DescriptorHeap* dh = nullptr;
    D3D12_DESCRIPTOR_HEAP_DESC hd{}; hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV; hd.NumDescriptors = 4;
    { HRESULT hdh = dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&dh)); printf("step: desc heap hr=0x%08X ptr=%p\n", (unsigned)hdh, (void*)dh); }

    ID3D12CommandQueue* q = nullptr;
    D3D12_COMMAND_QUEUE_DESC qd{ D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0 };
    { HRESULT hq = dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q)); printf("step: queue hr=0x%08X ptr=%p\n", (unsigned)hq, (void*)q); }
    ID3D12GraphicsCommandList* cl = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    { HRESULT ha = dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc)); printf("step: allocator hr=0x%08X ptr=%p\n", (unsigned)ha, (void*)alloc); }
    { HRESULT hc = dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&cl)); printf("step: cmdlist hr=0x%08X ptr=%p\n", (unsigned)hc, (void*)cl); }

    // ================= the list (mirrors xgpuInterceptDispatch's clB contents) =================
    if (!skipIn) {
        // --- input bounce for presentColor (as xgpuBounceInputs Phase 3 does) ---
        Barrier(cl, upB, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(cl, mirrorIn, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);   // m->lastState == COMMON (fresh)
        {
            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = upB; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint.Offset = 0;
            src.PlacedFootprint.Footprint.Format = DXGI_FORMAT_B8G8R8A8_UNORM;   // CopyFormatFor(typeless)
            src.PlacedFootprint.Footprint.Width = W; src.PlacedFootprint.Footprint.Height = H;
            src.PlacedFootprint.Footprint.Depth = 1; src.PlacedFootprint.Footprint.RowPitch = W*4;
            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = mirrorIn; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; dst.SubresourceIndex = 0;
            cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        Barrier(cl, mirrorIn, D3D12_RESOURCE_STATE_COPY_DEST, (D3D12_RESOURCE_STATES)(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE | D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE)); // effState rewrite target
        Barrier(cl, upB, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_GENERIC_READ);
    }

    if (!skipFx) {
        // --- FFX-sim: transition output to UAV (real FFX writes via compute; the write itself
        //     doesn't change tracked state, so a barrier alone replicates its final state) ---
        Barrier(cl, mirrorOut, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    if (!skipRb) {
        // --- our readback block (exactly as in xgpuInterceptDispatch) ---
        Barrier(cl, mirrorOut, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);   // "FFX may have left it anywhere"
        {
            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = rbB; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint.Offset = 0;
            dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_B8G8R8A8_UNORM;   // CopyFormatFor(typeless)
            dst.PlacedFootprint.Footprint.Width = W; dst.PlacedFootprint.Footprint.Height = H;
            dst.PlacedFootprint.Footprint.Depth = 1; dst.PlacedFootprint.Footprint.RowPitch = W*4;
            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = mirrorOut; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
            cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        Barrier(cl, mirrorOut, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);   // FfxStateToD3d(declared=UAV)
    }

    if (FAILED(cl->Close())) { printf("close failed\n"); return 1; }
    ID3D12CommandList* lists[] = { cl };
    q->ExecuteCommandLists(1, lists);
    CheckRemoved("full list execute");

    // fence wait like the proxy does
    ID3D12Fence* fence = nullptr; dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    q->Signal(fence, 1);
    fence->SetEventOnCompletion(1, ev);
    WaitForSingleObject(ev, 30000);
    CheckRemoved("after fence wait");

    printf("done\n");
    return 0;
}

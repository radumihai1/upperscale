// probe_common_barrier.cpp — does this AMD driver accept D3D12_RESOURCE_STATE_COMMON (0) as a
// transition before-state on GPU B? Replicates the exact in-game scenario:
//   texture 1024x768 fmt=B8G8R8A8_TYPELESS(0x1c) flags=RTV|UAV, written via UAV, then read back.
// Variant A: before-state = UNORDERED_ACCESS (the state FFX actually left it in)
// Variant B: before-state = COMMON (what the proxy currently does)
// Prints which variant removes the device (0x887A0006).
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <stdio.h>

static ID3D12Device* g_dev = nullptr;

static void CheckRemoved(const char* what) {
    HRESULT r = g_dev->GetDeviceRemovedReason();
    printf("  after %-28s removed=0x%08X %s\n", what, (unsigned)r, r == S_OK ? "OK" : "<== DEVICE REMOVED");
}

static bool MakeDev(ID3D12Device** out) {
    IDXGIFactory4* f = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&f)))) { printf("no factory\n"); return false; }
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

static bool RunVariant(const char* name, D3D12_RESOURCE_STATES beforeState) {
    ID3D12Device* dev = nullptr;
    if (!MakeDev(&dev)) return false;
    printf("variant %s (before-state=0x%X):\n", name, (unsigned)beforeState);

    // texture: 1024x768 B8G8R8A8_TYPELESS with RTV|UAV — same as the in-game FG output mirror
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = 1024; td.Height = 768; td.DepthOrArraySize = 1; td.MipLevels = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_TYPELESS;
    td.SampleDesc.Count = 1;
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET | D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    ID3D12Resource* tex = nullptr;
    D3D12_HEAP_PROPERTIES hp{};
    hp.Type = D3D12_HEAP_TYPE_DEFAULT;
    if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
            D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&tex)))) {
        printf("  texture create failed\n"); return false;
    }

    ID3D12CommandQueue* q = nullptr;
    D3D12_COMMAND_QUEUE_DESC qd{ D3D12_COMMAND_LIST_TYPE_DIRECT, 0, D3D12_COMMAND_QUEUE_FLAG_NONE, 0 };
    dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q));
    ID3D12GraphicsCommandList* cl = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc));
    dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&cl));

    // step 1: transition to UAV (as FFX would before writing) — from COMMON is legal at creation
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = tex;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = D3D12_RESOURCE_STATE_COMMON;
    b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    cl->ResourceBarrier(1, &b);
    cl->Close();
    ID3D12CommandList* lists[] = { cl };
    q->ExecuteCommandLists(1, lists);
    CheckRemoved("UAV transition");

    // step 2: the readback barrier — THE QUESTION
    D3D12_RESOURCE_BARRIER b2{};
    b2.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b2.Transition.pResource = tex;
    b2.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b2.Transition.StateBefore = beforeState;   // UAV (variant A) or COMMON (variant B)
    b2.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    cl->Reset(alloc, nullptr);
    cl->ResourceBarrier(1, &b2);
    cl->Close();
    q->ExecuteCommandLists(1, lists);
    CheckRemoved("readback transition");

    // step 3: back to UAV (as the proxy does after copying)
    D3D12_RESOURCE_BARRIER b3{};
    b3.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b3.Transition.pResource = tex;
    b3.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b3.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
    b3.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    cl->Reset(alloc, nullptr);
    cl->ResourceBarrier(1, &b3);
    cl->Close();
    q->ExecuteCommandLists(1, lists);
    CheckRemoved("back-to-UAV transition");

    tex->Release(); alloc->Release(); cl->Release(); q->Release(); dev->Release();
    g_dev = nullptr;
    return true;
}

int main() {
    printf("=== COMMON-before-state probe on GPU B (9060 XT) ===\n");
    RunVariant("A: UAV", D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    // fresh device for variant B (device may be removed after A if A was illegal — it shouldn't be, but be safe)
    RunVariant("B: COMMON", D3D12_RESOURCE_STATE_COMMON);
    printf("done\n");
    return 0;
}

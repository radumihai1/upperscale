// ffx_dispatchtest.cpp — END-TO-END TEST OF THE CROSS-GPU DISPATCH PATH (task 5).
//
// Simulates a game on GPU A (7900 XTX) that has FSR enabled:
//   * creates D3D12 device + textures on GPU A, fills them with deterministic patterns,
//   * loads OUR proxy DLL (build/smoke/amd_fidelityfx_dx12.dll),
//   * calls ffxCreateContext through the proxy (ACTIVE mode swaps the backend device to B),
//   * builds a real ffxDispatchDescUpscale and calls ffxDispatch through the proxy,
//     which must: bounce inputs A->B via RAM, run FSR4 on GPU B, capture the output back,
//     and record the copy-back into the game's command list,
//   * executes the game's command list on A, reads the output texture back, saves it.
//
// Usage: ffx_dispatchtest.exe [A|B] <savefile.bin>
//   Run twice to compare:
//     UPPERSCALE_ENABLE=0 ... ffx_dispatchtest.exe A ref_passthrough.bin   (FSR4 native on A)
//     UPPERSCALE_ENABLE=1 ... ffx_dispatchtest.exe A out_active.bin        (FSR4 via B, our path)
//   Then diff the two files — they should be near-identical (RDNA3 vs RDNA4 may differ by a ULP).

#include <d3d12.h>
#include <dxgi1_6.h>
#include <combaseapi.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <windows.h>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

// Real SDK headers (ABI-guaranteed match with the signed DLLs)
#include "../third_party/FidelityFX-SDK/Kits/FidelityFX/api/include/dx12/ffx_api_dx12.h"
#include "../third_party/FidelityFX-SDK/Kits/FidelityFX/upscalers/include/ffx_upscale.h"

// ---- sizes (render 512x288 -> upscale 768x432, ~1.5x like FSR Quality) ----
static const UINT RW = 512, RH = 288;      // render resolution
static const UINT UW = 768, UH = 432;      // upscale (presentation) resolution

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
            printf("matched adapter %u: %s -> device %s\n", i, nameA, SUCCEEDED(hr) ? "OK" : "FAILED");
            adapter->Release(); factory->Release();
            return dev;
        }
        adapter->Release();
    }
    factory->Release();
    return nullptr;
}

static void FfxMsg(uint32_t type, const wchar_t* msg) {
    char buf[1024]{};
    int n = WideCharToMultiByte(CP_UTF8, 0, msg, -1, buf, sizeof(buf)-1, nullptr, nullptr);
    printf("  [FFX %s] %.*s\n", type == FFX_API_MESSAGE_TYPE_ERROR ? "ERROR" : "WARN", n, buf);
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    ULONG luidA_lo = 0x2A089u;   // 7900 XTX (game GPU)
    const char* saveFile = "out.bin";
    if (argc > 2) saveFile = argv[2];

    ID3D12Device* devA = CreateDeviceByLuid(0, luidA_lo);
    if (!devA) { printf("FAIL: no GPU A device\n"); return 1; }

    // ---- load OUR proxy DLL (must sit next to this exe in build/smoke/) ----
    HMODULE hProxy = LoadLibraryA("amd_fidelityfx_dx12.dll");   // resolved from EXE dir first
    if (!hProxy) { printf("FAIL: LoadLibrary(our proxy) err=%lu\n", GetLastError()); return 1; }
    typedef ffxReturnCode_t (*PfnCC)(ffxContext*, ffxApiHeader*, const void*);
    typedef ffxReturnCode_t (*PfnDC)(ffxContext*, const void*);
    typedef ffxReturnCode_t (*PfnQ)(ffxContext*, ffxApiHeader*);
    PfnCC pCreate = (PfnCC)(void*)GetProcAddress(hProxy, "ffxCreateContext");
    PfnDC pDestroy= (PfnDC)(void*)GetProcAddress(hProxy, "ffxDestroyContext");
    PfnQ  pQuery  = (PfnQ)(void*)GetProcAddress(hProxy, "ffxQuery");
    typedef ffxReturnCode_t (*PfnD)(ffxContext*, const ffxApiHeader*);
    PfnD  pDispatch=(PfnD)(void*)GetProcAddress(hProxy, "ffxDispatch");
    if (!pCreate || !pDestroy || !pQuery || !pDispatch) { printf("FAIL: proxy missing exports\n"); return 1; }
    printf("proxy loaded from EXE dir OK\n");

    // ---- game-side D3D12 objects on GPU A ----
    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandQueue* queueA = nullptr; devA->CreateCommandQueue(&qd, IID_PPV_ARGS(&queueA));
    ID3D12CommandAllocator* allocA = nullptr; devA->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocA));
    ID3D12GraphicsCommandList* clA = nullptr; devA->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocA, nullptr, IID_PPV_ARGS(&clA));
    ID3D12Fence* fenceA = nullptr; devA->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fenceA));
    HANDLE evA = CreateEvent(nullptr, TRUE, FALSE, nullptr);

    auto makeTex = [&](UINT w, UINT h, DXGI_FORMAT fmt) -> ID3D12Resource* {
        D3D12_RESOURCE_DESC rd{};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = w; rd.Height = h; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
        rd.Format = fmt; rd.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN; rd.SampleDesc.Count = 1;
        // FFX records its own UAV transitions into these (compute writes) — real games set these flags.
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;
        ID3D12Resource* t = nullptr;
        if (FAILED(devA->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&t)))) { printf("tex create fail %ux%u fmt=0x%x\n", w, h, (unsigned)fmt); return nullptr; }
        return t;
    };

    ID3D12Resource* colorIn = makeTex(RW, RH, DXGI_FORMAT_R16G16B16A16_FLOAT);
    ID3D12Resource* depthIn = makeTex(RW, RH, DXGI_FORMAT_D32_FLOAT);
    ID3D12Resource* mvIn    = makeTex(RW, RH, DXGI_FORMAT_R16G16_FLOAT);
    ID3D12Resource* outTex  = makeTex(UW, UH, DXGI_FORMAT_R16G16B16A16_FLOAT);
    if (!colorIn || !depthIn || !mvIn || !outTex) { printf("FAIL: texture creation\n"); return 1; }

    // ---- fill inputs with deterministic patterns (via upload buffers on A) ----
    auto makeBuf = [&](SIZE_T bytes, D3D12_HEAP_TYPE ht, D3D12_RESOURCE_STATES st) -> ID3D12Resource* {
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = (UINT64)bytes < 0xFFFFFFFFull ? (UINT)bytes : 0xFFFFFFFFu;
        bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.Format = DXGI_FORMAT_UNKNOWN; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        bd.SampleDesc.Count = 1;
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = ht;
        ID3D12Resource* b = nullptr;
        devA->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, st, nullptr, IID_PPV_ARGS(&b));
        return b;
    };

    // color: gradient in half-float (x/511, y/287, 0.5, 1)
    {
        SIZE_T bytes = (SIZE_T)RW * RH * 8;
        ID3D12Resource* up = makeBuf(bytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        void* m = nullptr; up->Map(0, nullptr, &m);
        uint16_t* p = (uint16_t*)m;
        for (UINT y = 0; y < RH; ++y)
            for (UINT x = 0; x < RW; ++x) {
                // float -> half conversion via bit trick is overkill; use a simple lookup-free approach:
                auto f2h = [](float v) -> uint16_t {
                    union { float f; uint32_t u; } in{v};
                    uint32_t sign = (in.u >> 16) & 0x8000;
                    int32_t exp = ((int32_t)(in.u >> 23) & 0xFF) - 127 + 15;
                    if (exp <= 0) return (uint16_t)sign;                 // ~0 (fine for our range)
                    if (exp >= 31) exp = 30;
                    uint32_t mant = in.u & 0x7FFFFF;
                    return (uint16_t)(sign | ((uint32_t)exp << 10) | (mant >> 13));
                };
                p[0] = f2h((float)x / RW); p[1] = f2h((float)y / RH); p[2] = f2h(0.5f); p[3] = f2h(1.0f);
                p += 4;
            }
        up->Unmap(0, nullptr);

        // depth: gradient 0..1 in float (stored as D32_FLOAT bytes)
        ID3D12Resource* upD = makeBuf((SIZE_T)RW * RH * 4, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        void* mD = nullptr; upD->Map(0, nullptr, &mD);
        float* pd = (float*)mD;
        for (UINT y = 0; y < RH; ++y)
            for (UINT x = 0; x < RW; ++x) *pd++ = ((float)(x + y)) / (RW + RH - 2);
        upD->Unmap(0, nullptr);

        // MV: zeros (no motion) — buffer already zeroed by driver? No guarantee: write explicitly.
        ID3D12Resource* upM = makeBuf((SIZE_T)RW * RH * 4, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        void* mM = nullptr; upM->Map(0, nullptr, &mM);
        memset(mM, 0, (SIZE_T)RW * RH * 4);
        upM->Unmap(0, nullptr);

        // record: buf -> tex copies (PLACED_FOOTPRINT), execute on A so data is ready before dispatch
        auto barrier = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES f, D3D12_RESOURCE_STATES t) {
            D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = r; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            b.Transition.StateBefore = f; b.Transition.StateAfter = t; clA->ResourceBarrier(1, &b);
        };
        auto bufLoc = [&](ID3D12Resource* b, UINT w, UINT h, DXGI_FORMAT fmt) {
            D3D12_TEXTURE_COPY_LOCATION loc{};
            loc.pResource = b; loc.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            loc.PlacedFootprint.Offset = 0;
            loc.PlacedFootprint.Footprint.Format = fmt;
            loc.PlacedFootprint.Footprint.Width = w; loc.PlacedFootprint.Footprint.Height = h;
            loc.PlacedFootprint.Footprint.Depth = 1;
            loc.PlacedFootprint.Footprint.RowPitch = (UINT)(w * ((fmt == DXGI_FORMAT_R16G16B16A16_FLOAT) ? 8 : (fmt == DXGI_FORMAT_D32_FLOAT || fmt == DXGI_FORMAT_R16G16_FLOAT) ? 4 : 0));
            return loc;
        };
        auto texLoc = [&](ID3D12Resource* t) {
            D3D12_TEXTURE_COPY_LOCATION loc{}; loc.pResource = t;
            loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; loc.SubresourceIndex = 0; return loc;
        };

        clA->Reset(allocA, nullptr);
        barrier(up,   D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_COPY_SOURCE);
        clA->CopyTextureRegion(&texLoc(colorIn), 0, 0, 0, &bufLoc(up, RW, RH, DXGI_FORMAT_R16G16B16A16_FLOAT), nullptr);
        barrier(up,   D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_GENERIC_READ);
        barrier(upD,  D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_COPY_SOURCE);
        clA->CopyTextureRegion(&texLoc(depthIn), 0, 0, 0, &bufLoc(upD, RW, RH, DXGI_FORMAT_D32_FLOAT), nullptr);
        barrier(upD,  D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_GENERIC_READ);
        barrier(upM,  D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_COPY_SOURCE);
        clA->CopyTextureRegion(&texLoc(mvIn),    0, 0, 0, &bufLoc(upM, RW, RH, DXGI_FORMAT_R16G16_FLOAT), nullptr);
        barrier(upM,  D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_GENERIC_READ);
        clA->Close();
        ID3D12CommandList* ls[] = { clA }; queueA->ExecuteCommandLists(1, ls);
        queueA->Signal(fenceA, 1);
        fenceA->SetEventOnCompletion(1, evA);
        WaitForSingleObject(evA, 30000);
        up->Release(); upD->Release(); upM->Release();
        printf("input textures filled on GPU A\n");
    }

    // ---- FFX version query (two-phase) through the proxy ----
    uint64_t versionIds[16] = {0}; const char* versionNames[16] = {}; int nVersions = 0;
    {
        ffxQueryDescGetVersions q{};
        q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
        q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
        q.device = devA;
        uint64_t count = 0; q.outputCount = &count;
        pQuery(nullptr, &q.header);
        if (count > 0 && count <= 16) {
            nVersions = (int)count;
            for (int i = 0; i < nVersions; ++i) versionNames[i] = nullptr;
            q.versionIds = versionIds; q.versionNames = versionNames;
            pQuery(nullptr, &q.header);
        }
    }
    printf("versions available: %d\n", nVersions);

    // ---- create FFX context through the proxy (ACTIVE mode swaps device to B) ----
    static ffxCreateContextDescUpscale up{};
    up.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    up.flags = 0;
    up.maxRenderSize.width = RW;  up.maxRenderSize.height = RH;
    up.maxUpscaleSize.width = UW; up.maxUpscaleSize.height = UH;
    up.fpMessage = FfxMsg;

    static ffxOverrideVersion ov{};
    ov.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    if (nVersions > 0) ov.versionId = versionIds[0];

    static ffxCreateBackendDX12Desc be{};
    be.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    be.device = devA;   // the proxy swaps this to GPU B in ACTIVE mode

    if (nVersions > 0) { up.header.pNext = &ov.header; ov.header.pNext = &be.header; }
    else { up.header.pNext = &be.header; }
    be.header.pNext = nullptr;

    ffxContext ctx = nullptr;
    uint32_t rc = pCreate(&ctx, &up.header, nullptr);
    printf("ffxCreateContext via proxy: rc=%u ctx=%p\n", rc, (void*)ctx);
    if (rc != FFX_API_RETURN_OK) { printf("FAIL: context creation\n"); return 1; }

    // provider version check
    ffxQueryGetProviderVersion pv{};
    pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    pQuery(&ctx, &pv.header);
    printf("provider: id=0x%llx name=%s\n", (unsigned long long)pv.versionId, pv.versionName ? pv.versionName : "(null)");

    // ---- build the dispatch desc exactly like a game does ----
    // A real game resets its command list every frame before recording — do the same here,
    // otherwise FFX (passive) / our copy-back (active) would record into an already-executed list.
    { HRESULT hrR = clA->Reset(allocA, nullptr); printf("pre-dispatch CL Reset=0x%08X\n", (unsigned)hrR); }
    static ffxDispatchDescUpscale d{};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = clA;   // the GAME's command list on GPU A (still open)
    d.color  = ffxApiGetResourceDX12(colorIn, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    d.depth  = ffxApiGetResourceDX12(depthIn, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    d.motionVectors = ffxApiGetResourceDX12(mvIn, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    // exposure / reactive / transparencyAndComposition: leave zeroed (null resources)
    d.output = ffxApiGetResourceDX12(outTex, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);

    d.jitterOffset.x = 0.f; d.jitterOffset.y = 0.f;
    d.motionVectorScale.x = (float)RW; d.motionVectorScale.y = (float)RH;
    d.renderSize.width = RW; d.renderSize.height = RH;
    d.upscaleSize.width = UW; d.upscaleSize.height = UH;
    d.enableSharpening = false; d.sharpness = 0.f;
    d.frameTimeDelta = 16.6f;
    d.preExposure = 1.0f;
    d.reset = true;   // first frame: reset temporal state
    d.cameraNear = 0.1f; d.cameraFar = 100.f;
    d.cameraFovAngleVertical = 1.0f;
    d.viewSpaceToMetersFactor = 1.0f;
    d.flags = 0;

    printf(">> calling ffxDispatch via proxy (this is the cross-GPU path in ACTIVE mode)...\n");
    uint32_t drc = pDispatch(&ctx, &d.header);
    printf("ffxDispatch rc=%u\n", drc);
    if (drc != FFX_API_RETURN_OK) { printf("FAIL: dispatch returned %u\n", drc); return 1; }

    // ---- execute the game's command list on A (runs our recorded copy-back) ----
    {
        HRESULT hrC = clA->Close();
        printf("game CL Close=0x%08X\n", (unsigned)hrC);
        if (FAILED(hrC)) return 1;
    }
    ID3D12CommandList* ls[] = { clA }; queueA->ExecuteCommandLists(1, ls);
    queueA->Signal(fenceA, 2);
    fenceA->SetEventOnCompletion(2, evA);
    if (WaitForSingleObject(evA, 30000) != WAIT_OBJECT_0) { printf("FAIL: timeout executing game CL\n"); return 1; }
    printf("game command list executed on A (copy-back done)\n");

    // ---- read the output texture back from A and save it ----
    SIZE_T outBytes = (SIZE_T)UW * UH * 8;   // RGBA16F
    ID3D12Resource* rb = makeBuf(outBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    { HRESULT hrR = clA->Reset(allocA, nullptr); printf("readback CL Reset=0x%08X\n", (unsigned)hrR); }
    {
        D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = outTex; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE; clA->ResourceBarrier(1, &b);
    }
    {
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = rb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset = 0;
        dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        dst.PlacedFootprint.Footprint.Width = UW; dst.PlacedFootprint.Footprint.Height = UH;
        dst.PlacedFootprint.Footprint.Depth = 1; dst.PlacedFootprint.Footprint.RowPitch = (UINT)(UW * 8);
        D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = outTex;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
        clA->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    {
        D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = outTex; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE; clA->ResourceBarrier(1, &b);
    }
    clA->Close();
    ID3D12CommandList* ls2[] = { clA }; queueA->ExecuteCommandLists(1, ls2);
    queueA->Signal(fenceA, 3);
    fenceA->SetEventOnCompletion(3, evA);
    WaitForSingleObject(evA, 30000);

    D3D12_RANGE rng{0, outBytes};
    void* m = nullptr; rb->Map(0, &rng, &m);
    FILE* f = fopen(saveFile, "wb");
    if (!f) { printf("FAIL: cannot open %s\n", saveFile); return 1; }
    fwrite(m, 1, outBytes, f); fclose(f);

    // quick sanity stats on the output (count non-zero pixels)
    uint16_t* px = (uint16_t*)m;
    SIZE_T npx = (SIZE_T)UW * UH;
    SIZE_T nonzero = 0;
    for (SIZE_T i = 0; i < npx; ++i) {
        uint32_t v = px[i*4] | px[i*4+1] | px[i*4+2];
        if (v) nonzero++;
    }
    rb->Unmap(0, nullptr);
    printf("output saved to %s (%llu bytes), non-zero pixels: %llu/%llu\n", saveFile,
           (unsigned long long)outBytes, (unsigned long long)nonzero, (unsigned long long)npx);

    // ---- cleanup ----
    pDestroy(&ctx, nullptr);
    rb->Release(); outTex->Release(); colorIn->Release(); depthIn->Release(); mvIn->Release();
    clA->Release(); allocA->Release(); queueA->Release(); fenceA->Release(); CloseHandle(evA);
    devA->Release();
    FreeLibrary(hProxy);

    if (nonzero > npx / 4) { printf("PASS: dispatch path produced a plausible upscaled image\n"); return 0; }
    printf("FAIL: output looks empty (%llu non-zero)\n", (unsigned long long)nonzero);
    return 2;
}

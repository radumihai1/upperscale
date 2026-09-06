// ffx_fgtest.cpp — END-TO-END FRAME GENERATION TEST (task 6a).
// Simulates a game on GPU A with FSR4 frame generation enabled:
//   * creates an FG context through the proxy (ACTIVE mode swaps device to GPU B)
//   * per frame: ffxConfigure(FG config, NO_SWAPCHAIN_CONTEXT_NOTIFY) +
//     ffxDispatch(PREPARE_V2 {depth, MV}) + ffxDispatch(FRAMEGENERATION {presentColor -> outputs[0]})
//   * the proxy intercepts both dispatches: bounces depth/MV/presentColor A->B via RAM, runs real FG on B,
//     copies the generated frame back into the game's output texture.
// Verifies the generated output is non-zero (FG actually ran) and prints per-frame timing from upperscale.log.
//
// Build: cl /nologo /EHsc /O2 /TP tools/ffx_fgtest.cpp /Fe:build/smoke/ffx_fgtest.exe "/link" d3d12.lib dxgi.lib user32.lib
// Run  : cd build/smoke && UPPERSCALE_ENABLE=1 UPPERSCALE_GPU_LUID=0x27214 UPPERSCALE_LOG=3 ./ffx_fgtest.exe out_fg.bin [frames]

#include <d3d12.h>
#include <dxgi1_6.h>
#include <combaseapi.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>
#include <windows.h>

// Real SDK headers (ABI-guaranteed match with the signed DLLs) — same pattern as ffx_dispatchtest.cpp.
#include "../third_party/FidelityFX-SDK/Kits/FidelityFX/api/include/dx12/ffx_api_dx12.h"
#include "../third_party/FidelityFX-SDK/Kits/FidelityFX/upscalers/include/ffx_upscale.h"
#include "../third_party/FidelityFX-SDK/Kits/FidelityFX/framegeneration/include/ffx_framegeneration.h"

// ---- sizes (render 512x288, display 768x432) ----
static const UINT RW = 512, RH = 288;      // render resolution (depth/MV)
static const UINT DW = 768, DH = 432;      // display resolution (presentColor + FG output)

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
    const char* saveFile = (argc > 1) ? argv[1] : "out_fg.bin";
    int frames = (argc > 2) ? atoi(argv[2]) : 5;

    ID3D12Device* devA = CreateDeviceByLuid(0, 0x2A089u);   // 7900 XTX (game GPU)
    if (!devA) { printf("FAIL: no GPU A device\n"); return 1; }

    // ---- load OUR proxy DLL (must sit next to this exe in build/smoke/) ----
    HMODULE hProxy = LoadLibraryA("amd_fidelityfx_dx12.dll");   // resolved from EXE dir first
    if (!hProxy) { printf("FAIL: LoadLibrary(our proxy) err=%lu\n", GetLastError()); return 1; }
    typedef ffxReturnCode_t (*PfnCC)(ffxContext*, ffxApiHeader*, const void*);
    typedef ffxReturnCode_t (*PfnDC)(ffxContext*, const void*);
    typedef ffxReturnCode_t (*PfnQ)(ffxContext*, ffxApiHeader*);
    typedef ffxReturnCode_t (*PfnCfg)(ffxContext*, const ffxApiHeader*);
    PfnCC pCreate = (PfnCC)(void*)GetProcAddress(hProxy, "ffxCreateContext");
    PfnDC pDestroy= (PfnDC)(void*)GetProcAddress(hProxy, "ffxDestroyContext");
    PfnQ  pQuery  = (PfnQ)(void*)GetProcAddress(hProxy, "ffxQuery");
    PfnCfg pConfigure = (PfnCfg)(void*)GetProcAddress(hProxy, "ffxConfigure");
    typedef ffxReturnCode_t (*PfnD)(ffxContext*, const ffxApiHeader*);
    PfnD  pDispatch=(PfnD)(void*)GetProcAddress(hProxy, "ffxDispatch");
    if (!pCreate || !pDestroy || !pQuery || !pConfigure || !pDispatch) { printf("FAIL: proxy missing exports\n"); return 1; }
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

    ID3D12Resource* depthIn = makeTex(RW, RH, DXGI_FORMAT_D32_FLOAT);
    ID3D12Resource* mvIn    = makeTex(RW, RH, DXGI_FORMAT_R16G16_FLOAT);
    ID3D12Resource* presentIn = makeTex(DW, DH, DXGI_FORMAT_R16G16B16A16_FLOAT);  // "backbuffer" (HUD-less)
    ID3D12Resource* fgOut     = makeTex(DW, DH, DXGI_FORMAT_R16G16B16A16_FLOAT);   // generated frame output
    if (!depthIn || !mvIn || !presentIn || !fgOut) { printf("FAIL: texture creation\n"); return 1; }

    auto f2h = [](float v) -> uint16_t {
        union { float f; uint32_t u; } in{v};
        uint32_t sign = (in.u >> 16) & 0x8000;
        int32_t exp = ((int32_t)(in.u >> 23) & 0xFF) - 127 + 15;
        if (exp <= 0) return (uint16_t)sign;
        if (exp >= 31) exp = 30;
        uint32_t mant = in.u & 0x7FFFFF;
        return (uint16_t)(sign | ((uint32_t)exp << 10) | (mant >> 13));
    };

    // fill: depth gradient, MV small constant motion, presentColor moving gradient (so FG has something to interpolate)
    auto makeBuf = [&](SIZE_T bytes, D3D12_HEAP_TYPE ht, D3D12_RESOURCE_STATES st) -> ID3D12Resource* {
        D3D12_RESOURCE_DESC bd{};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = (UINT64)bytes < 0xFFFFFFFFull ? (UINT)bytes : 0xFFFFFFFFu;
        bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
        bd.Format = DXGI_FORMAT_UNKNOWN; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR; bd.SampleDesc.Count = 1;
        D3D12_HEAP_PROPERTIES hp{}; hp.Type = ht;
        ID3D12Resource* b = nullptr;
        devA->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, st, nullptr, IID_PPV_ARGS(&b));
        return b;
    };
    {
        SIZE_T dBytes = (SIZE_T)RW * RH * 4;
        ID3D12Resource* up = makeBuf(dBytes, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);
        void* m = nullptr; up->Map(0, nullptr, &m);
        float* pd = (float*)m;
        for (UINT y = 0; y < RH; ++y) for (UINT x = 0; x < RW; ++x) *pd++ = ((float)(x + y)) / (RW + RH - 2);
        up->Unmap(0, nullptr);

        ID3D12Resource* upM = makeBuf((SIZE_T)RW * RH * 4, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);   // MV: small constant motion (pixels)
        void* mM = nullptr; upM->Map(0, nullptr, &mM);
        uint16_t* pm = (uint16_t*)mM;
        for (UINT i = 0; i < (SIZE_T)RW * RH; ++i) { pm[0] = f2h(1.5f); pm[1] = f2h(0.75f); pm += 2; }
        upM->Unmap(0, nullptr);

        ID3D12Resource* upP = makeBuf((SIZE_T)DW * DH * 8, D3D12_HEAP_TYPE_UPLOAD, D3D12_RESOURCE_STATE_GENERIC_READ);   // presentColor: gradient
        void* mP = nullptr; upP->Map(0, nullptr, &mP);
        uint16_t* pp = (uint16_t*)mP;
        for (UINT y = 0; y < DH; ++y)
            for (UINT x = 0; x < DW; ++x) {
                pp[0] = f2h((float)x / DW); pp[1] = f2h((float)y / DH); pp[2] = f2h(0.5f); pp[3] = f2h(1.0f);
                pp += 4;
            }
        upP->Unmap(0, nullptr);

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
            loc.PlacedFootprint.Footprint.RowPitch = (UINT)(w * ((fmt == DXGI_FORMAT_R16G16B16A16_FLOAT) ? 8 : 4));
            return loc;
        };
        auto texLoc = [&](ID3D12Resource* t) {
            D3D12_TEXTURE_COPY_LOCATION loc{}; loc.pResource = t;
            loc.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; loc.SubresourceIndex = 0; return loc;
        };

        clA->Reset(allocA, nullptr);
        barrier(up,   D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_COPY_SOURCE);
        clA->CopyTextureRegion(&texLoc(depthIn), 0, 0, 0, &bufLoc(up, RW, RH, DXGI_FORMAT_D32_FLOAT), nullptr);
        barrier(up,   D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_GENERIC_READ);
        barrier(upM,  D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_COPY_SOURCE);
        clA->CopyTextureRegion(&texLoc(mvIn),    0, 0, 0, &bufLoc(upM, RW, RH, DXGI_FORMAT_R16G16_FLOAT), nullptr);
        barrier(upM,  D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_GENERIC_READ);
        barrier(upP,  D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_COPY_SOURCE);
        clA->CopyTextureRegion(&texLoc(presentIn), 0, 0, 0, &bufLoc(upP, DW, DH, DXGI_FORMAT_R16G16B16A16_FLOAT), nullptr);
        barrier(upP,  D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_GENERIC_READ);
        clA->Close();
        ID3D12CommandList* ls[] = { clA }; queueA->ExecuteCommandLists(1, ls);
        queueA->Signal(fenceA, 1);
        fenceA->SetEventOnCompletion(1, evA);
        WaitForSingleObject(evA, 30000);
        up->Release(); upM->Release(); upP->Release();
        printf("input textures filled on GPU A\n");
    }

    // ---- FFX version query (two-phase) through the proxy — for the override node ----
    // Query with the FRAMEGENERATION createDescType so we get FG-provider version IDs (an upscaler
    // version ID in an FG context -> NO_PROVIDER).
    uint64_t versionIds[16] = {0}; const char* versionNames[16] = {}; int nVersions = 0;
    {
        ffxQueryDescGetVersions q{};
        q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
        q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
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
    printf("FG versions available: %d\n", nVersions);

    // ---- create FG context through the proxy (ACTIVE mode swaps device to B) ----
    static ffxCreateContextDescFrameGeneration fg{};
    fg.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_FRAMEGENERATION;
    fg.flags = FFX_FRAMEGENERATION_ENABLE_HIGH_DYNAMIC_RANGE;   // RGBA16F backbuffer
    fg.displaySize.width = DW;  fg.displaySize.height = DH;
    fg.maxRenderSize.width = RW; fg.maxRenderSize.height = RH;
    fg.backBufferFormat = FFX_API_SURFACE_FORMAT_R16G16B16A16_FLOAT;

    static ffxOverrideVersion ov{};
    ov.header.type = FFX_API_DESC_TYPE_OVERRIDE_VERSION;
    if (nVersions > 0) ov.versionId = versionIds[0];

    static ffxCreateBackendDX12Desc be{};
    be.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    be.device = devA;   // the proxy swaps this to GPU B in ACTIVE mode

    if (nVersions > 0) { fg.header.pNext = &ov.header; ov.header.pNext = &be.header; }
    else { fg.header.pNext = &be.header; }
    be.header.pNext = nullptr;

    ffxContext ctx = nullptr;
    ffxReturnCode_t rc = pCreate(&ctx, &fg.header, nullptr);
    printf("ffxCreateContext(FG) via proxy: rc=%u ctx=%p\n", rc, (void*)ctx);
    if (rc != FFX_API_RETURN_OK) { printf("FAIL: FG context creation\n"); return 1; }

    // provider version check
    ffxQueryGetProviderVersion pv{};
    pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    pQuery(&ctx, &pv.header);
    printf("provider: id=0x%llx name=%s\n", (unsigned long long)pv.versionId, pv.versionName ? pv.versionName : "(null)");

    // ---- per-frame loop: configure + PREPARE_V2 + FRAMEGENERATION through the proxy ----
    static ffxConfigureDescFrameGeneration cfg{};
    cfg.header.type = FFX_API_CONFIGURE_DESC_TYPE_FRAMEGENERATION;
    cfg.swapChain = nullptr;   // non-swapchain mode (NO_SWAPCHAIN_CONTEXT_NOTIFY)
    cfg.frameGenerationEnabled = true;
    cfg.allowAsyncWorkloads = false;
    cfg.flags = FFX_FRAMEGENERATION_FLAG_NO_SWAPCHAIN_CONTEXT_NOTIFY;
    cfg.onlyPresentGenerated = false;
    cfg.generationRect.left = 0; cfg.generationRect.top = 0;
    cfg.generationRect.width = DW; cfg.generationRect.height = DH;

    static ffxDispatchDescFrameGenerationPrepareV2 prep{};
    prep.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2;
    prep.renderSize.width = RW; prep.renderSize.height = RH;
    prep.jitterOffset.x = 0.f; prep.jitterOffset.y = 0.f;
    prep.motionVectorScale.x = (float)RW; prep.motionVectorScale.y = (float)RH;   // sample uses render dims for UV-space MVs
    prep.frameTimeDelta = 16.6f;
    prep.cameraNear = 0.1f; prep.cameraFar = 100.f; prep.cameraFovAngleVertical = 1.0f;
    prep.viewSpaceToMetersFactor = 1.0f;
    prep.depth         = ffxApiGetResourceDX12(depthIn, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    prep.motionVectors = ffxApiGetResourceDX12(mvIn,    FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);

    static ffxDispatchDescFrameGeneration gen{};
    gen.header.type = FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION;
    gen.presentColor = ffxApiGetResourceDX12(presentIn, FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    gen.outputs[0]   = ffxApiGetResourceDX12(fgOut,     FFX_API_RESOURCE_STATE_PIXEL_COMPUTE_READ);
    gen.numGeneratedFrames = 1;
    gen.backbufferTransferFunction = 0;   // SDR-ish default; HDR flag set at context level
    gen.minMaxLuminance[0] = 0.05f; gen.minMaxLuminance[1] = 1000.f;
    gen.generationRect.left = 0; gen.generationRect.top = 0;
    gen.generationRect.width = DW; gen.generationRect.height = DH;

    uint64_t frameID = 0;
    int lastFrameRcPrep = -1, lastFrameRcGen = -1;
    for (int f = 0; f < frames; ++f) {
        cfg.frameID = frameID;
        prep.frameID = frameID;
        gen.frameID = frameID;
        prep.reset = (f == 0);
        gen.reset = (f == 0);

        // game resets its command list every frame before recording FFX work
        clA->Reset(allocA, nullptr);
        prep.commandList = clA;
        gen.commandList = clA;

        ffxReturnCode_t crc = pConfigure(&ctx, &cfg.header);
        if (crc != FFX_API_RETURN_OK) { printf("frame %d: ffxConfigure rc=%u\n", f, crc); }

        lastFrameRcPrep = pDispatch(&ctx, &prep.header);
        if (lastFrameRcPrep != FFX_API_RETURN_OK) { printf("frame %d: PREPARE_V2 dispatch rc=%u\n", f, lastFrameRcPrep); break; }

        lastFrameRcGen = pDispatch(&ctx, &gen.header);
        if (lastFrameRcGen != FFX_API_RETURN_OK) { printf("frame %d: FRAMEGENERATION dispatch rc=%u\n", f, lastFrameRcGen); break; }

        // execute the game's command list on A (runs our recorded copy-back of the generated frame)
        clA->Close();
        ID3D12CommandList* ls[] = { clA }; queueA->ExecuteCommandLists(1, ls);
        queueA->Signal(fenceA, 10 + f);
        fenceA->SetEventOnCompletion(10 + f, evA);
        if (WaitForSingleObject(evA, 30000) != WAIT_OBJECT_0) { printf("frame %d: timeout executing game CL\n", f); break; }
        frameID++;
    }
    printf("last frame: PREPARE rc=%d FRAMEGENERATION rc=%d (after %llu frames)\n", lastFrameRcPrep, lastFrameRcGen, (unsigned long long)frameID);
    if (frameID == 0 || lastFrameRcGen != FFX_API_RETURN_OK) { printf("FAIL: FG dispatch path\n"); return 1; }

    // ---- read the generated output back from A and save it ----
    SIZE_T outBytes = (SIZE_T)DW * DH * 8;
    ID3D12Resource* rb = makeBuf(outBytes, D3D12_HEAP_TYPE_READBACK, D3D12_RESOURCE_STATE_COPY_DEST);
    clA->Reset(allocA, nullptr);
    {
        D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = fgOut; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE; clA->ResourceBarrier(1, &b);
    }
    {
        D3D12_TEXTURE_COPY_LOCATION dst{};
        dst.pResource = rb; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        dst.PlacedFootprint.Offset = 0;
        dst.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
        dst.PlacedFootprint.Footprint.Width = DW; dst.PlacedFootprint.Footprint.Height = DH;
        dst.PlacedFootprint.Footprint.Depth = 1; dst.PlacedFootprint.Footprint.RowPitch = (UINT)(DW * 8);
        D3D12_TEXTURE_COPY_LOCATION src{}; src.pResource = fgOut;
        src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX; src.SubresourceIndex = 0;
        clA->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    }
    {
        D3D12_RESOURCE_BARRIER b{}; b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        b.Transition.pResource = fgOut; b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_SOURCE;
        b.Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE; clA->ResourceBarrier(1, &b);
    }
    clA->Close();
    ID3D12CommandList* ls2[] = { clA }; queueA->ExecuteCommandLists(1, ls2);
    queueA->Signal(fenceA, 90);
    fenceA->SetEventOnCompletion(90, evA);
    WaitForSingleObject(evA, 30000);

    D3D12_RANGE rng{0, outBytes};
    void* m = nullptr; rb->Map(0, &rng, &m);
    FILE* fo = fopen(saveFile, "wb");
    if (!fo) { printf("FAIL: cannot open %s\n", saveFile); return 1; }
    fwrite(m, 1, outBytes, fo); fclose(fo);

    uint16_t* px = (uint16_t*)m;
    SIZE_T npx = (SIZE_T)DW * DH;
    SIZE_T nonzero = 0;
    for (SIZE_T i = 0; i < npx; ++i) {
        uint32_t v = px[i*4] | px[i*4+1] | px[i*4+2];
        if (v) nonzero++;
    }
    rb->Unmap(0, nullptr);
    printf("generated frame saved to %s (%llu bytes), non-zero pixels: %llu/%llu\n", saveFile,
           (unsigned long long)outBytes, (unsigned long long)nonzero, (unsigned long long)npx);

    // cleanup
    pDestroy(&ctx, nullptr);
    rb->Release(); fgOut->Release(); presentIn->Release(); mvIn->Release(); depthIn->Release();
    clA->Release(); allocA->Release(); queueA->Release(); fenceA->Release(); CloseHandle(evA);
    devA->Release();
    FreeLibrary(hProxy);

    if (nonzero > npx / 4) { printf("PASS: FG path produced a plausible generated frame\n"); return 0; }
    printf("FAIL: generated output looks empty (%llu non-zero)\n", (unsigned long long)nonzero);
    return 2;
}

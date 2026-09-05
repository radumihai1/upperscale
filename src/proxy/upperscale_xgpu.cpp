// upperscale_xgpu.cpp — cross-GPU RAM-bounce transfer hub + dispatch interception.
//
// Reuses the exact patterns validated in tools/xgpu_probe4.cpp on this hardware:
//   * PLACED_FOOTPRINT for every buffer<->texture copy (buffer-as-SUBRESOURCE_INDEX fails)
//   * explicit byte ranges on Map() ({0,SIZE_MAX} is rejected by the AMD driver)
//   * READBACK buffers created in COPY_DEST state, no barrier needed to Map them
//   * SampleDesc.Count = 1 on every buffer desc (zero-init gives Count=0 -> E_INVALIDARG)
//
// FFX dispatch semantics: ffxDispatch RECORDS compute into the provided command list; it does
// not execute. xgpuInterceptDispatch therefore:
//   1. bounces each input A->B synchronously (our own queues + fences on both devices),
//   2. hands FFX our GPU-B command list, calls real ffxDispatch (records only),
//   3. appends an output readback to the same open list, closes+executes it on GPU B, waits,
//   4. memcpys the result into a rotating A-side UPLOAD ring slot,
//   5. records the copy-back (uploadA -> game's original output texture) into the GAME's
//      command list with symmetric barriers around its declared state,
//   6. restores every field of the desc exactly as the game passed it.

#include "upperscale_xgpu.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>

// Real SDK headers — ABI-guaranteed match with the signed effect DLLs (proven by ffx_bindtest).
// Included here only (NOT in the header) because ffx_api.h declares its entry points with
// __declspec(dllexport), which would collide with our own definitions in the proxy DLL.
#include "../../third_party/FidelityFX-SDK/Kits/FidelityFX/api/include/ffx_api.h"
#include "../../third_party/FidelityFX-SDK/Kits/FidelityFX/upscalers/include/ffx_upscale.h"

// ---------------------------------------------------------------------------
// Hub definition (opaque in the header)
// ---------------------------------------------------------------------------
#define XGPU_OUT_SLOTS 3   // output ring: game may still be executing N-1's copy-back

typedef struct XGpuMirror {
    ID3D12Resource* resB;      // B-side mirror (texture or buffer), DEFAULT heap, same desc as A
    UINT            width, height;   // textures
    uint64_t        sizeBytes;       // buffers
    DXGI_FORMAT     format;          // textures (DXGI_FORMAT_UNKNOWN for buffers)
    D3D12_RESOURCE_STATES lastState; // tracked state we last left it in (this driver rejects ALL_BARRIERS)
} XGpuMirror;

typedef struct OutSlot { ID3D12Resource* upA; uint64_t size; } OutSlot;

struct XGpuHub {
    ID3D12Device*         devA;      // AddRef'd by us (game's original device)
    ID3D12Device*         devB;      // NOT owned (proxy owns it)
    ID3D12CommandQueue*   qA, *qB;
    ID3D12CommandAllocator* allocA, *allocB;
    ID3D12GraphicsCommandList* clA, *clB;
    ID3D12Fence*          fenceA, *fenceB;
    HANDLE                evA, evB;
    uint64_t              fenceValA, fenceValB;

    ID3D12Resource*       rbA;   uint64_t rbASize;   // A readback buffer (grown on demand)
    ID3D12Resource*       upB;   uint64_t upBSize;   // B upload buffer  (grown on demand)
    ID3D12Resource*       rbB;   uint64_t rbBSize;   // B readback buffer (for output capture)

    OutSlot               outSlots[XGPU_OUT_SLOTS];
    int                   outNext;

    XGpuMirror            mirrors[64];
    int                   mirrorCount;

    uint64_t              dispatches;
    double                msInputs, msFfxRecord, msCapture;  // running sums (ms)
};

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
static int g_xgpuLogOn = -1; // -1 undetermined, 0 off, 1 on (UPPERSCALE_LOG >= 2)
static void XLog(const char* fmt, ...) {
    if (g_xgpuLogOn == -1) {
        char b[8] = {};
        GetEnvironmentVariableA("UPPERSCALE_LOG", b, sizeof(b));
        g_xgpuLogOn = atoi(b) >= 2 ? 1 : 0;
    }
    if (!g_xgpuLogOn) return;
    FILE* f = fopen("upperscale.log", "a");
    if (!f) return;
    fprintf(f, "[xgpu] ");
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fclose(f);
}

static double NowMs() {
    LARGE_INTEGER fr, c; QueryPerformanceFrequency(&fr); QueryPerformanceCounter(&c);
    return (double)c.QuadPart * 1000.0 / (double)fr.QuadPart;
}

static void Barrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* r,
                    D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to) {
    if (!r || from == to) return;
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    cl->ResourceBarrier(1, &b);
}

// NOTE: no aliasing-barrier helper here — SDK 10.0.26100's D3D12_RESOURCE_ALIASING_BARRIER has
// no State member, and this driver accepts transitions from COMMON(0) with NO preceding barrier
// (verified tools/xgpu_probe18_barrier_uav_common.cpp case g: Close=0). For "unknown current state" use:
//   Barrier(cl, r, D3D12_RESOURCE_STATE_COMMON, target);

// FFX resource state -> D3D12 tracked state (values from ffx_api_types.h enum)
static D3D12_RESOURCE_STATES FfxStateToD3d(uint32_t s) {
    switch (s) {
        case 0x1:  return D3D12_RESOURCE_STATE_COMMON;                    // COMMON
        case 0x2:  return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;          // UAV
        case 0x4:  return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE; // COMPUTE_READ
        case 0x8:  return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;     // PIXEL_READ
        case 0xC:  return (D3D12_RESOURCE_STATES)(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                                  D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE); // PIXEL_COMPUTE_READ
        case 0x10: return D3D12_RESOURCE_STATE_COPY_SOURCE;               // COPY_SRC
        case 0x20: return D3D12_RESOURCE_STATE_COPY_DEST;                 // COPY_DEST
        case 0x14: return (D3D12_RESOURCE_STATES)(D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                                  D3D12_RESOURCE_STATE_COPY_SOURCE);           // GENERIC_READ
        default:   return D3D12_RESOURCE_STATE_GENERIC_READ;              // safe for 0/unknown
    }
}

// Depth/stencil textures must be copied to buffers using their shader-readable view format.
static DXGI_FORMAT CopyFormatFor(DXGI_FORMAT f) {
    switch (f) {
        case DXGI_FORMAT_D32_FLOAT:               return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_R32G8X24_TYPELESS:       return DXGI_FORMAT_R32G8X24_TYPELESS;
        case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:    return DXGI_FORMAT_R32G8X24_TYPELESS; // no _UINT variant in this SDK
        case DXGI_FORMAT_D16_UNORM:               return DXGI_FORMAT_R16_UNORM;
        default:                                  return f;
    }
}

static HRESULT MakeBuffer(ID3D12Device* dev, D3D12_HEAP_TYPE ht, uint64_t bytes,
                          D3D12_RESOURCE_STATES st, ID3D12Resource** out) {
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = (UINT)(bytes > 0xFFFFFFFFull ? 0xFFFFFFFFu : bytes); // committed max 4GB
    bd.Height = 1; bd.DepthOrArraySize = 1; bd.MipLevels = 1;
    bd.Format = DXGI_FORMAT_UNKNOWN; bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    bd.SampleDesc.Count = 1;   // REQUIRED on this driver (zero-init gives Count=0)
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = ht;
    return dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, st, nullptr, IID_PPV_ARGS(out));
}

static void GrowBuffer(XGpuHub* h, ID3D12Resource** slot, uint64_t* sizeSlot,
                       ID3D12Device* dev, D3D12_HEAP_TYPE ht, D3D12_RESOURCE_STATES st,
                       const char* name, uint64_t need) {
    if (*slot && *sizeSlot >= need) return;
    if (*slot) { (*slot)->Release(); *slot = nullptr; }
    uint64_t sz = need < (1u << 20) ? (1u << 20) : need;   // min 1MB, else exact
    HRESULT hr = MakeBuffer(dev, ht, sz, st, slot);
    if (SUCCEEDED(hr)) { *sizeSlot = sz; XLog("grew %s buffer to %llu bytes", name, (unsigned long long)sz); }
    else XLog("ERROR: grow %s buffer failed hr=0x%08X need=%llu", name, (unsigned)hr, (unsigned long long)need);
}

// GetCopyableFootprints wrapper for this SDK's signature (UINT* rows, UINT64* rowSize/total).
static void Footprints(ID3D12Device* dev, const D3D12_RESOURCE_DESC* d, UINT mips,
                       D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[16], UINT rows[16],
                       UINT64 pitch[16], UINT64 slice[16]) {
    dev->GetCopyableFootprints(d, 0, mips, 0, fp, rows, pitch, slice);
}

// This SDK predates the named ALL_BARRIERS constant; define it (value is stable: 0x80000000).
#ifndef D3D12_RESOURCE_STATE_ALL_BARRIERS
#define D3D12_RESOURCE_STATE_ALL_BARRIERS ((D3D12_RESOURCE_STATES)0x80000000u)
#endif

// AMD driver quirk (Win11 26200, RDNA3/RDNA4): Reset() on a FRESH command list returns E_FAIL
// (0x80004005) but the list is still fully usable — Close succeeds and commands execute.
// Verified with tools/xgpu_probe15_reset_efail.cpp (instrumented probe4 replica). So: never treat a failed
// Reset as fatal; log it once per list instead.
static int g_resetWarnedA = 0, g_resetWarnedB = 0;
static HRESULT SafeReset(ID3D12GraphicsCommandList* cl, ID3D12CommandAllocator* alloc, int* warned) {
    HRESULT hr = cl->Reset(alloc, nullptr);
    if (FAILED(hr) && !*warned) { *warned = 1; XLog("note: Reset returned 0x%08X on fresh list — known driver quirk, continuing", (unsigned)hr); }
    return hr;   // caller must NOT bail on failure
}

// ---------------------------------------------------------------------------
// Hub lifecycle
// ---------------------------------------------------------------------------
static XGpuHub* g_hubs[8] = {};
static int      g_nHubs = 0;

XGpuHub* xgpuGetOrCreateHub(ID3D12Device* devA, ID3D12Device* devB) {
    for (int i = 0; i < g_nHubs; ++i)
        if (g_hubs[i]->devA == devA && g_hubs[i]->devB == devB) return g_hubs[i];

    XGpuHub* h = (XGpuHub*)calloc(1, sizeof(XGpuHub));
    if (!h) return nullptr;
    h->devA = devA; devA->AddRef();
    h->devB = devB;

    D3D12_COMMAND_QUEUE_DESC qd{}; qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    HRESULT hr = devA->CreateCommandQueue(&qd, IID_PPV_ARGS(&h->qA));
    if (SUCCEEDED(hr)) hr = devB->CreateCommandQueue(&qd, IID_PPV_ARGS(&h->qB));
    if (FAILED(hr)) { XLog("ERROR: queue create 0x%08X", (unsigned)hr); goto fail; }
    hr = devA->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&h->allocA));
    if (SUCCEEDED(hr)) hr = devB->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&h->allocB));
    if (FAILED(hr)) goto fail;
    hr = devA->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, h->allocA, nullptr, IID_PPV_ARGS(&h->clA));
    if (SUCCEEDED(hr)) hr = devB->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, h->allocB, nullptr, IID_PPV_ARGS(&h->clB));
    if (FAILED(hr)) goto fail;
    hr = devA->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&h->fenceA));
    if (SUCCEEDED(hr)) hr = devB->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&h->fenceB));
    if (FAILED(hr)) goto fail;
    h->evA = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    h->evB = CreateEvent(nullptr, TRUE, FALSE, nullptr);
    XLog("hub created: devA=%p devB=%p queues+lists+fences OK", (void*)devA, (void*)devB);
    if (g_nHubs < 8) g_hubs[g_nHubs++] = h;
    return h;

fail:
    if (h->evA) CloseHandle(h->evA);
    if (h->evB) CloseHandle(h->evB);
    if (h->fenceA) h->fenceA->Release();
    if (h->fenceB) h->fenceB->Release();
    if (h->clA) h->clA->Release();
    if (h->clB) h->clB->Release();
    if (h->allocA) h->allocA->Release();
    if (h->allocB) h->allocB->Release();
    if (h->qA) h->qA->Release();
    if (h->qB) h->qB->Release();
    h->devA->Release();
    free(h);
    return nullptr;
}

void xgpuReleaseHub(XGpuHub* h) {
    if (!h) return;
    for (int i = 0; i < XGPU_OUT_SLOTS; ++i) if (h->outSlots[i].upA) h->outSlots[i].upA->Release();
    for (int i = 0; i < h->mirrorCount; ++i) if (h->mirrors[i].resB) h->mirrors[i].resB->Release();
    if (h->rbA) h->rbA->Release();
    if (h->upB) h->upB->Release();
    if (h->rbB) h->rbB->Release();
    if (h->evA) CloseHandle(h->evA);
    if (h->evB) CloseHandle(h->evB);
    if (h->fenceA) h->fenceA->Release();
    if (h->fenceB) h->fenceB->Release();
    if (h->clA) h->clA->Release();
    if (h->clB) h->clB->Release();
    if (h->allocA) h->allocA->Release();
    if (h->allocB) h->allocB->Release();
    if (h->qA) h->qA->Release();
    if (h->qB) h->qB->Release();
    h->devA->Release();
    for (int i = 0; i < g_nHubs; ++i) if (g_hubs[i] == h) { g_hubs[i] = nullptr; --g_nHubs; }
    free(h);
}

// ---------------------------------------------------------------------------
// Mirror cache (B-side copies of game resources, created on demand with the SAME desc —
// including Flags — so FFX can build identical SRV/UAVs on them)
// ---------------------------------------------------------------------------
static XGpuMirror* GetOrCreateMirror(XGpuHub* h, ID3D12Resource* srcA) {
    D3D12_RESOURCE_DESC d = srcA->GetDesc();
    bool isBuf = (d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER);

    for (int i = 0; i < h->mirrorCount; ++i) {
        XGpuMirror* m = &h->mirrors[i];
        if (!m->resB) continue;
        if (isBuf) {
            if (m->format == DXGI_FORMAT_UNKNOWN && m->sizeBytes == d.Width) return m;
        } else {
            if (m->format != DXGI_FORMAT_UNKNOWN &&
                m->width == (UINT)d.Width && m->height == (UINT)d.Height && m->format == d.Format) return m;
        }
    }

    if (h->mirrorCount >= 64) { XLog("ERROR: mirror cache full"); return nullptr; }
    XGpuMirror* m = &h->mirrors[h->mirrorCount];
    memset(m, 0, sizeof(*m));
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = D3D12_HEAP_TYPE_DEFAULT;

    // FFX records its own state transitions into our command list and WILL use these mirrors as
    // UAVs (compute writes). Without ALLOW_UNORDERED_ACCESS the driver rejects any transition
    // into/out of UAV state at Close with E_INVALIDARG — so set it on every mirror.
    d.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    HRESULT hr;
    if (isBuf) {
        m->sizeBytes = d.Width;
        hr = h->devB->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m->resB));
    } else {
        m->width = (UINT)d.Width; m->height = (UINT)d.Height; m->format = d.Format;
        hr = h->devB->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
               D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m->resB));
    }
    if (FAILED(hr)) {
        XLog("ERROR: mirror create failed hr=0x%08X (%s %ux%u fmt=0x%x)", (unsigned)hr,
             isBuf ? "buf" : "tex", m->width, m->height, (unsigned)m->format);
        return nullptr;
    }
    m->lastState = D3D12_RESOURCE_STATE_GENERIC_READ;   // creation state
    h->mirrorCount++;
    XLog("new mirror #%d: %s %ux%u fmt=0x%x size=%llu", h->mirrorCount - 1, isBuf ? "buf" : "tex",
         m->width, m->height, (unsigned)m->format, (unsigned long long)(isBuf ? m->sizeBytes : 0));
    return m;
}

// ---------------------------------------------------------------------------
// A -> B input bounce (synchronous)
// ---------------------------------------------------------------------------
bool xgpuBounceInput(XGpuHub* h, FfxApiResource* res) {
    ID3D12Resource* srcA = (ID3D12Resource*)res->resource;
    if (!srcA) return true;   // null optional input — nothing to do

    XGpuMirror* m = GetOrCreateMirror(h, srcA);
    if (!m) return false;
    D3D12_RESOURCE_DESC d = srcA->GetDesc();
    bool isBuf = (d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER);

    // ---- Stage 1: read A into rbA on our queue A ----
    uint64_t need = 0;
    UINT mips = 1;
    if (isBuf) {
        need = d.Width;
    } else {
        mips = d.MipLevels ? d.MipLevels : 1;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[16]; UINT rows[16]; UINT64 pitch[16], slice[16];
        if (mips > 16) { XLog("ERROR: too many mips %u", mips); return false; }
        Footprints(h->devA, &d, mips, fp, rows, pitch, slice);
        for (UINT i = 0; i < mips; ++i) need += slice[i];
    }
    GrowBuffer(h, &h->rbA, &h->rbASize, h->devA, D3D12_HEAP_TYPE_READBACK,
               D3D12_RESOURCE_STATE_COPY_DEST, "A-readback", need);
    if (!h->rbA) return false;

    // ALL_BARRIERS as StateBefore: robust to whatever tracked state the resource actually has.
    HRESULT hr = SafeReset(h->clA, h->allocA, &g_resetWarnedA);   // E_FAIL on fresh list is a known quirk — continue

    if (isBuf) {
        // Game resource: declared state == what the game left it in (we're mid-recording on its CL).
        Barrier(h->clA, srcA, FfxStateToD3d(res->state), D3D12_RESOURCE_STATE_COPY_SOURCE);
        h->clA->CopyBufferRegion(h->rbA, 0, srcA, 0, d.Width);
        Barrier(h->clA, srcA, D3D12_RESOURCE_STATE_COPY_SOURCE, FfxStateToD3d(res->state));
    } else {
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[16]; UINT rows[16]; UINT64 pitch[16], slice[16];
        Footprints(h->devA, &d, mips, fp, rows, pitch, slice);
        XLog("bounce in: footprints mips=%u fp0.Offset=%llu fp0.RowPitch=%llu slice0=%llu",
             mips, (unsigned long long)fp[0].Offset, (unsigned long long)fp[0].Footprint.RowPitch,
             (unsigned long long)slice[0]);
        Barrier(h->clA, srcA, FfxStateToD3d(res->state), D3D12_RESOURCE_STATE_COPY_SOURCE);
        for (UINT i = 0; i < mips; ++i) {
            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = h->rbA; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            dst.PlacedFootprint.Offset = fp[i].Offset;
            dst.PlacedFootprint.Footprint.Format = CopyFormatFor(d.Format); // depth-view mapping
            dst.PlacedFootprint.Footprint.Width = fp[i].Footprint.Width;
            dst.PlacedFootprint.Footprint.Height = fp[i].Footprint.Height;
            dst.PlacedFootprint.Footprint.Depth = 1;
            dst.PlacedFootprint.Footprint.RowPitch = fp[i].Footprint.RowPitch;
            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = srcA; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.SubresourceIndex = i;
            h->clA->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        Barrier(h->clA, srcA, D3D12_RESOURCE_STATE_COPY_SOURCE, FfxStateToD3d(res->state));
    }
    {
        HRESULT hrC = h->clA->Close();
        if (FAILED(hrC)) { XLog("ERROR: clA close hr=0x%08X", (unsigned)hrC); return false; }
    }
    ID3D12CommandList* lists[] = { h->clA };
    h->qA->ExecuteCommandLists(1, lists);
    h->fenceValA++;
    h->qA->Signal(h->fenceA, h->fenceValA);
    if (FAILED(h->fenceA->SetEventOnCompletion(h->fenceValA, h->evA))) { XLog("ERROR: fence A event"); return false; }
    if (WaitForSingleObject(h->evA, 30000) != WAIT_OBJECT_0) { XLog("ERROR: timeout waiting GPU-A readback"); return false; }

    // ---- Stage 2: CPU hop into B upload buffer ----
    D3D12_RANGE r{0, (SIZE_T)need};
    void* mRb = nullptr;
    if (FAILED(h->rbA->Map(0, &r, &mRb))) { XLog("ERROR: rbA map"); return false; }

    GrowBuffer(h, &h->upB, &h->upBSize, h->devB, D3D12_HEAP_TYPE_UPLOAD,
               D3D12_RESOURCE_STATE_GENERIC_READ, "B-upload", need);
    if (!h->upB) { h->rbA->Unmap(0, nullptr); return false; }
    void* mUp = nullptr;
    if (FAILED(h->upB->Map(0, nullptr, &mUp))) { XLog("ERROR: upB map"); h->rbA->Unmap(0, nullptr); return false; }
    memcpy(mUp, mRb, need);
    h->upB->Unmap(0, nullptr);
    h->rbA->Unmap(0, nullptr);

    // ---- Stage 3: upload into B mirror on our queue B (mirror must be COPY_DEST for the copy) ----
    hr = SafeReset(h->clB, h->allocB, &g_resetWarnedB);   // E_FAIL on fresh list is a known quirk — continue
    if (isBuf) {
        Barrier(h->clB, h->upB, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_COPY_SOURCE);
        h->clB->CopyBufferRegion(m->resB, 0, h->upB, 0, d.Width);
        Barrier(h->clB, h->upB, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_GENERIC_READ);
    } else {
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[16]; UINT rows[16]; UINT64 pitch[16], slice[16];
        Footprints(h->devB, &d, mips, fp, rows, pitch, slice);
        Barrier(h->clB, h->upB, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_COPY_SOURCE);
        // Our own mirror: use the tracked state (this driver rejects ALL_BARRIERS in transitions).
        Barrier(h->clB, m->resB, m->lastState, D3D12_RESOURCE_STATE_COPY_DEST);
        for (UINT i = 0; i < mips; ++i) {
            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = h->upB; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint.Offset = fp[i].Offset;
            src.PlacedFootprint.Footprint.Format = CopyFormatFor(d.Format);
            src.PlacedFootprint.Footprint.Width = fp[i].Footprint.Width;
            src.PlacedFootprint.Footprint.Height = fp[i].Footprint.Height;
            src.PlacedFootprint.Footprint.Depth = 1;
            src.PlacedFootprint.Footprint.RowPitch = fp[i].Footprint.RowPitch;
            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = m->resB; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = i;
            h->clB->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        Barrier(h->clB, m->resB, D3D12_RESOURCE_STATE_COPY_DEST, FfxStateToD3d(res->state));
        Barrier(h->clB, h->upB, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_GENERIC_READ);
    }
    if (FAILED(h->clB->Close())) { XLog("ERROR: clB close"); return false; }
    ID3D12CommandList* listsB[] = { h->clB };
    h->qB->ExecuteCommandLists(1, listsB);
    h->fenceValB++;
    h->qB->Signal(h->fenceB, h->fenceValB);
    if (FAILED(h->fenceB->SetEventOnCompletion(h->fenceValB, h->evB))) { XLog("ERROR: fence B event"); return false; }
    if (WaitForSingleObject(h->evB, 30000) != WAIT_OBJECT_0) { XLog("ERROR: timeout waiting GPU-B upload"); return false; }

    res->resource = m->resB;   // swap in the B-side mirror for FFX (state field stays as declared)
    return true;
}

// ---------------------------------------------------------------------------
// Output copy-back into the game's command list (symmetric barriers around declared state)
// ---------------------------------------------------------------------------
void xgpuRecordOutputCopyBack(XGpuHub* h, ID3D12GraphicsCommandList* clGame,
                              FfxApiResource* resOut, ID3D12Resource* uploadA) {
    if (!clGame || !resOut || !uploadA) return;
    ID3D12Resource* gameTex = (ID3D12Resource*)resOut->resource;   // original A-side output
    if (!gameTex) return;
    D3D12_RESOURCE_DESC d = gameTex->GetDesc();
    bool isBuf = (d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER);
    D3D12_RESOURCE_STATES declared = FfxStateToD3d(resOut->state);

    if (isBuf) {
        Barrier(clGame, uploadA, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_COPY_SOURCE);
        clGame->CopyBufferRegion(gameTex, 0, uploadA, 0, d.Width);
        Barrier(clGame, uploadA, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_GENERIC_READ);
    } else {
        UINT mips = d.MipLevels ? d.MipLevels : 1;
        if (mips > 16) return;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[16]; UINT rows[16]; UINT64 pitch[16], slice[16];
        Footprints(h->devA, &d, mips, fp, rows, pitch, slice);
        Barrier(clGame, uploadA, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Barrier(clGame, gameTex, declared, D3D12_RESOURCE_STATE_COPY_DEST);
        for (UINT i = 0; i < mips; ++i) {
            D3D12_TEXTURE_COPY_LOCATION src{};
            src.pResource = uploadA; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint.Offset = fp[i].Offset;
            src.PlacedFootprint.Footprint.Format = CopyFormatFor(d.Format);
            src.PlacedFootprint.Footprint.Width = fp[i].Footprint.Width;
            src.PlacedFootprint.Footprint.Height = fp[i].Footprint.Height;
            src.PlacedFootprint.Footprint.Depth = 1;
            src.PlacedFootprint.Footprint.RowPitch = fp[i].Footprint.RowPitch;
            D3D12_TEXTURE_COPY_LOCATION dst{};
            dst.pResource = gameTex; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            dst.SubresourceIndex = i;
            clGame->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        }
        Barrier(clGame, gameTex, D3D12_RESOURCE_STATE_COPY_DEST, declared);
        Barrier(clGame, uploadA, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_GENERIC_READ);
    }
}

// ---------------------------------------------------------------------------
// Dispatch interception (the whole cross-GPU FFX dispatch in one call)
// Uses the SDK's own PfnFfxDispatch / ffxContext / ffxApiHeader types from ffx_api.h.
// ---------------------------------------------------------------------------
ffxReturnCode_t xgpuInterceptDispatch(XGpuHub* h, void* ctx, const void* descIn, void* realDispatch) {
    if (!h || !descIn || !realDispatch) return FFX_API_RETURN_ERROR_PARAMETER;
    ffxContext c = (ffxContext)ctx;
    PfnFfxDispatch rd = (PfnFfxDispatch)realDispatch;
    const ffxApiHeader* desc = (const ffxApiHeader*)descIn;

    // ---- locate the effect dispatch node in the chain ----
    uint64_t nodeType = 0;
    ffxApiHeader* node = nullptr;
    for (const ffxApiHeader* n = desc; n; n = n->pNext) {
        if (n->type == FFX_API_DISPATCH_DESC_TYPE_UPSCALE ||
            n->type == FFX_API_DISPATCH_DESC_TYPE_UPSCALE_GENERATEREACTIVEMASK) {
            nodeType = n->type; node = (ffxApiHeader*)n; break;
        }
    }
    if (!node) { XLog("intercept: no upscale node in chain (top=0x%llx)", (unsigned long long)desc->type); return FFX_API_RETURN_ERROR_PARAMETER; }

    // ---- collect input/output field pointers for this node type ----
    FfxApiResource* inputs[8] = {}; int nIn = 0;
    FfxApiResource* outRes = nullptr;
    ID3D12GraphicsCommandList** pCmdList = nullptr;
    if (nodeType == FFX_API_DISPATCH_DESC_TYPE_UPSCALE) {
        ffxDispatchDescUpscale* u = (ffxDispatchDescUpscale*)node;
        inputs[nIn++] = &u->color;
        inputs[nIn++] = &u->depth;
        inputs[nIn++] = &u->motionVectors;
        inputs[nIn++] = &u->exposure;
        inputs[nIn++] = &u->reactive;
        inputs[nIn++] = &u->transparencyAndComposition;
        outRes = &u->output;
        pCmdList = (ID3D12GraphicsCommandList**)&u->commandList;
    } else { // GENERATEREACTIVEMASK
        ffxDispatchDescUpscaleGenerateReactiveMask* u = (ffxDispatchDescUpscaleGenerateReactiveMask*)node;
        inputs[nIn++] = &u->colorOpaqueOnly;
        inputs[nIn++] = &u->colorPreUpscale;
        outRes = &u->outReactive;
        pCmdList = (ID3D12GraphicsCommandList**)&u->commandList;
    }

    // ---- save originals for restore (per field index — robust to partial failure) ----
    FfxApiResource origByField[8]; bool wasSwapped[8] = {};
    ID3D12GraphicsCommandList* origCmdList = *pCmdList;
    FfxApiResource origOut = *outRes;
    bool outSwapped = false;
    ffxReturnCode_t rc = FFX_API_RETURN_ERROR;

    double t0 = NowMs();

    // ---- bounce every non-null input A -> B (synchronous) ----
    for (int i = 0; i < nIn; ++i) {
        if (!inputs[i]->resource) continue;
        origByField[i] = *inputs[i];
        XLog("bounce in[%d]: %s %ux%u fmt=0x%x state=0x%x", i,
             inputs[i]->description.type == FFX_API_RESOURCE_TYPE_BUFFER ? "buf" : "tex",
             inputs[i]->description.width, inputs[i]->description.height,
             (unsigned)inputs[i]->description.format, (unsigned)inputs[i]->state);
        if (!xgpuBounceInput(h, inputs[i])) { XLog("ERROR: input bounce %d failed — aborting dispatch", i); goto restore; }
        wasSwapped[i] = true;
    }

    // ---- output: point FFX at a B-side mirror of the game's output texture ----
    if (outRes->resource) {
        XGpuMirror* mOut = GetOrCreateMirror(h, (ID3D12Resource*)outRes->resource);
        if (!mOut) goto restore;
        outRes->resource = mOut->resB;
        outSwapped = true;
    }

    // ---- hand FFX our GPU-B command list and record (E_FAIL on fresh list is a known quirk) ----
    HRESULT hr = SafeReset(h->clB, h->allocB, &g_resetWarnedB);
    *pCmdList = h->clB;

    double t1 = NowMs();
    rc = rd(&c, desc);   // SDK signature takes ffxContext* (pointer to handle)
    double t2 = NowMs();
    if (rc != FFX_API_RETURN_OK) { XLog("real ffxDispatch failed rc=%u", rc); goto restore; }

    // ---- append output readback to the SAME open list FFX recorded into, execute on B ----
    ID3D12Resource* upAUsed = nullptr;
    if (outSwapped && outRes->resource) {
        ID3D12Resource* srcB = (ID3D12Resource*)outRes->resource;   // our B-side mirror
        D3D12_RESOURCE_DESC d = srcB->GetDesc();
        bool isBuf = (d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER);

        uint64_t need = 0;
        UINT mips = 1;
        if (isBuf) {
            need = d.Width;
        } else {
            mips = d.MipLevels ? d.MipLevels : 1;
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[16]; UINT rows[16]; UINT64 pitch[16], slice[16];
            if (mips <= 16) {
                Footprints(h->devB, &d, mips, fp, rows, pitch, slice);
                for (UINT i = 0; i < mips; ++i) need += slice[i];
            } else need = 0;
        }

        if (need > 0) {
            GrowBuffer(h, &h->rbB, &h->rbBSize, h->devB, D3D12_HEAP_TYPE_READBACK,
                       D3D12_RESOURCE_STATE_COPY_DEST, "B-readback", need);
            if (h->rbB) {
                bool ok = true;
                if (isBuf) {
                    // FFX may have left the mirror in any state after its passes — transition from COMMON(0).
                    Barrier(h->clB, srcB, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
                    h->clB->CopyBufferRegion(h->rbB, 0, srcB, 0, d.Width);
                    Barrier(h->clB, srcB, D3D12_RESOURCE_STATE_COPY_SOURCE, FfxStateToD3d(origOut.state));
                } else {
                    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[16]; UINT rows[16]; UINT64 pitch[16], slice[16];
                    Footprints(h->devB, &d, mips, fp, rows, pitch, slice);
                    // FFX may have left the mirror in any state after its passes — transition from COMMON(0)
                    // (this driver rejects ALL_BARRIERS and UAV barriers; verified xgpu_probe18 case g).
                    Barrier(h->clB, srcB, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
                    for (UINT i = 0; i < mips; ++i) {
                        D3D12_TEXTURE_COPY_LOCATION dst{};
                        dst.pResource = h->rbB; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                        dst.PlacedFootprint.Offset = fp[i].Offset;
                        dst.PlacedFootprint.Footprint.Format = CopyFormatFor(d.Format);
                        dst.PlacedFootprint.Footprint.Width = fp[i].Footprint.Width;
                        dst.PlacedFootprint.Footprint.Height = fp[i].Footprint.Height;
                        dst.PlacedFootprint.Footprint.Depth = 1;
                        dst.PlacedFootprint.Footprint.RowPitch = fp[i].Footprint.RowPitch;
                        D3D12_TEXTURE_COPY_LOCATION src{};
                        src.pResource = srcB; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                        src.SubresourceIndex = i;
                        h->clB->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                    }
                    Barrier(h->clB, srcB, D3D12_RESOURCE_STATE_COPY_SOURCE, FfxStateToD3d(origOut.state));
                }

                if (FAILED(h->clB->Close())) { XLog("ERROR: clB close (output) failed"); ok = false; }
                if (ok) {
                    ID3D12CommandList* lists[] = { h->clB };
                    h->qB->ExecuteCommandLists(1, lists);
                    h->fenceValB++;
                    h->qB->Signal(h->fenceB, h->fenceValB);
                    if (FAILED(h->fenceB->SetEventOnCompletion(h->fenceValB, h->evB))) { XLog("ERROR: fence B event"); ok = false; }
                    else if (WaitForSingleObject(h->evB, 30000) != WAIT_OBJECT_0) { XLog("ERROR: timeout waiting GPU-B output readback"); ok = false; }

                    if (ok) {
                        // CPU hop into the next A-side upload ring slot
                        D3D12_RANGE r{0, (SIZE_T)need};
                        void* mRb = nullptr;
                        if (FAILED(h->rbB->Map(0, &r, &mRb))) { XLog("ERROR: rbB map failed"); ok = false; }
                        else {
                            int slotIdx = h->outNext % XGPU_OUT_SLOTS;
                            OutSlot* os = &h->outSlots[slotIdx];
                            if (!os->upA || os->size < need) {
                                if (os->upA) { os->upA->Release(); os->upA = nullptr; }
                                uint64_t sz = need < (1u << 20) ? (1u << 20) : need;
                                HRESULT hr2 = MakeBuffer(h->devA, D3D12_HEAP_TYPE_UPLOAD, sz,
                                                         D3D12_RESOURCE_STATE_GENERIC_READ, &os->upA);
                                if (FAILED(hr2)) { XLog("ERROR: out-slot upload create 0x%08X", (unsigned)hr2); ok = false; }
                                else os->size = sz;
                            }
                            if (ok) {
                                void* mUpA = nullptr;
                                if (FAILED(os->upA->Map(0, nullptr, &mUpA))) { XLog("ERROR: out-slot upload map failed"); ok = false; }
                                else {
                                    memcpy(mUpA, mRb, need);
                                    os->upA->Unmap(0, nullptr);
                                    upAUsed = os->upA;
                                    h->outNext++;
                                }
                            }
                            if (ok) h->rbB->Unmap(0, nullptr);
                        }
                    }
                }
            }
        }
    }

    // ---- record the copy-back into the GAME's command list (still open — mid-recording) ----
    if (upAUsed && origCmdList) {
        xgpuRecordOutputCopyBack(h, origCmdList, &origOut, upAUsed);
    } else if (outSwapped) {
        XLog("WARNING: output capture failed — game's output texture holds stale data this frame");
    }

    double t3 = NowMs();
    h->dispatches++;
    h->msInputs += t1 - t0;
    h->msFfxRecord += t2 - t1;
    h->msCapture += t3 - t2;
    if (h->dispatches % 60 == 1) {
        XLog("stats #%llu: inputs=%.2fms ffx_record=%.2fms capture+copyback=%.2fms total=%.2fms",
             (unsigned long long)h->dispatches, h->msInputs / h->dispatches,
             h->msFfxRecord / h->dispatches, h->msCapture / h->dispatches,
             (t3 - t0));
    }

restore:
    // restore the desc exactly as the game passed it (per field, only what we swapped)
    for (int i = 0; i < nIn; ++i) if (wasSwapped[i]) inputs[i]->resource = origByField[i].resource;
    *pCmdList = origCmdList;
    *outRes = origOut;

    return rc;
}

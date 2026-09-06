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
//   1. bounces all inputs A->B in one batched pass (task 6b): ONE GPU-A readback + single fence
//      wait, back-to-back CPU memcpys through RAM, ONE GPU-B upload + single fence wait —
//      2 fence round-trips total regardless of input count,
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
#include "../../third_party/FidelityFX-SDK/Kits/FidelityFX/framegeneration/include/ffx_framegeneration.h"

// ---------------------------------------------------------------------------
// Hub definition (opaque in the header)
// ---------------------------------------------------------------------------
#define XGPU_OUT_SLOTS 12  // output ring: up to 4 outputs/dispatch (FG) x 3 frames in flight

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
    double                msTotalMax;   // worst-case end-to-end ffxDispatch cost seen so far
    double                msTotalEma;   // steady-state estimate (EMA alpha=0.1) — the number that matters for latency
};

// ---------------------------------------------------------------------------
// Small helpers
// ---------------------------------------------------------------------------
// The proxy's parsed log level (set by LoadConfig in DllMain before any dispatch). XLog uses it
// so in-game logging works from the ini even when no UPPERSCALE_LOG env var is set.
extern int g_cfgLogLevel;

static void XLog(const char* fmt, ...) {
    // Log if either the proxy config (ini/env) or a direct env override says verbose.
    static int envOverride = -1;   // -1 undetermined
    if (envOverride == -1) {
        char b[8] = {};
        GetEnvironmentVariableA("UPPERSCALE_LOG", b, sizeof(b));
        envOverride = atoi(b);     // 0 if unset
    }
    int level = (g_cfgLogLevel > envOverride) ? g_cfgLogLevel : envOverride;
    if (level < 2) return;
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

// FFX resource state -> D3D12 tracked state (values from ffx_api_types.h enum).
// Used for the RESTORE barrier after a bounce — must put the game's resource back in the state it
// declared, so map every FFX state bit accurately. Unknown/0 falls back to COMMON (safe on this
// driver: transitions out of COMMON are always legal and accepted without validation).
static D3D12_RESOURCE_STATES FfxStateToD3d(uint32_t s) {
    switch (s) {
        case 0x1:   return D3D12_RESOURCE_STATE_COMMON;                    // COMMON
        case 0x2:   return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;          // UAV
        case 0x4:   return D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE; // COMPUTE_READ
        case 0x8:   return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;     // PIXEL_READ
        case 0xC:   return (D3D12_RESOURCE_STATES)(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE); // PIXEL_COMPUTE_READ
        case 0x10:  return D3D12_RESOURCE_STATE_COPY_SOURCE;               // COPY_SRC
        case 0x20:  return D3D12_RESOURCE_STATE_COPY_DEST;                 // COPY_DEST
        case 0x14:  return (D3D12_RESOURCE_STATES)(D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
                                                  D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE |
                                                  D3D12_RESOURCE_STATE_COPY_SOURCE);           // GENERIC_READ
        case 0x80:  return D3D12_RESOURCE_STATE_RENDER_TARGET;             // PRESENT (backbuffer pre-Present)
        case 0x100: return D3D12_RESOURCE_STATE_RENDER_TARGET;             // RENDER_TARGET
        case 0x200: return D3D12_RESOURCE_STATE_DEPTH_WRITE;               // DEPTH_ATTACHMENT
        default:    return D3D12_RESOURCE_STATE_COMMON;                    // safe for 0/unknown (incl. INDIRECT_ARGUMENT)
    }
}

// NOTE on the bounce read barrier: we transition the game resource from FfxStateToD3d(res->state) —
// i.e. the state the GAME declared in the FFX desc — to COPY_SOURCE, and back afterwards. We trust
// that declaration (FFX's contract requires it to be accurate) rather than forcing COMMON, because
// D3D12 uses your declared transitions to insert hardware barriers: claiming "from COMMON" for a
// resource actually in DEPTH_WRITE would skip the real fence and risk tearing on other drivers.

// Depth/stencil textures must be copied to buffers using their shader-readable view format.
// CRITICAL (task 7b, desc_probe3h): this AMD driver REJECTS CopyTextureRegion into a row-major
// buffer when the footprint format is a TYPELESS depth-family format (0x13/0x14/...) — Close()
// fails with E_INVALIDARG. The typed view (R32_FLOAT) works for every from-state. Cyberpunk's FG
// PREPARE passes its depth as R32G8X24_TYPELESS, so the pass-through below was the in-game bug.
static DXGI_FORMAT CopyFormatFor(DXGI_FORMAT f) {
    switch ((UINT)f) {
        case 0x13: // R32G8X24_TYPELESS
            return DXGI_FORMAT_R32_FLOAT;
        case 0x14: // D32_FLOAT_S8X24_UINT (stencil part is not readable via row-major copy anyway)
            return DXGI_FORMAT_R32_FLOAT;
        case 0x15: // R32_FLOAT_X8X24_TYPELESS
            return DXGI_FORMAT_R32_FLOAT;
        case 0x16: // X32_TYPELESS_G8X24_UINT (depth part)
            return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_D32_FLOAT:               return DXGI_FORMAT_R32_FLOAT;
        case DXGI_FORMAT_D16_UNORM:               return DXGI_FORMAT_R16_UNORM;
        default:                                  return f;
    }
}

// Depth-family formats (typeless or typed depth/stencil). On this AMD driver these can ONLY be
// created with ALLOW_DEPTH_STENCIL (or no flags) — ALLOW_UNORDERED_ACCESS and ALLOW_RENDER_TARGET
// are both rejected with E_INVALIDARG (verified in-game + desc_probe2, task 7b: Cyberpunk's FG
// PREPARE passes a 600x1248 R32G8X24_TYPELESS depth texture; UAV/RT mirrors failed, DS-only worked).
static bool IsDepthFamilyFormat(DXGI_FORMAT f) {
    switch ((UINT)f) {
        case 0x13: // R32G8X24_TYPELESS
        case 0x14: // D32_FLOAT_S8X24_UINT
        case 0x15: // R32_FLOAT_X8X24_TYPELESS
        case 0x16: // X32_TYPELESS_G8X24_UINT
        case 0x28: // D32_FLOAT (40)
        case 0x2C: // R24G8_TYPELESS (44)
        case 0x2D: // D24_UNORM_S8_UINT (45)
        case 0x2E: // R24_UNORM_X8_TYPELESS (46)
        case 0x2F: // X24_TYPELESS_G8_UINT (47)
        case 0x37: // D16_UNORM (55)
            return true;
        default:   return false;
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
    if (h->dispatches > 0)
        XLog("session summary: %llu dispatch(es), avg inputs=%.2fms ffx_record=%.2fms capture+copyback=%.2fms | frame_total ema=%.2fms max=%.2fms",
             (unsigned long long)h->dispatches, h->msInputs / h->dispatches,
             h->msFfxRecord / h->dispatches, h->msCapture / h->dispatches,
             h->msTotalEma, h->msTotalMax);
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

    // Full source desc dump (diagnostics, task 7b): every field that could make the mirror
    // creation fail on GPU B — Quality in particular is NOT validated by us and a non-zero
    // value with Count=1 is illegal.
    XLog("mirror attempt: %s dim=%u %ux%ux%u fmt=0x%x mips=%u count=%u quality=%u layout=%u align=%llu srcFlags=0x%X",
         isBuf ? "buf" : "tex", d.Dimension, (unsigned)d.Width, (unsigned)d.Height, d.DepthOrArraySize,
         (unsigned)d.Format, d.MipLevels, d.SampleDesc.Count, d.SampleDesc.Quality,
         (unsigned)d.Layout, (unsigned long long)d.Alignment, (unsigned)d.Flags);

    // FFX records its own state transitions into our command list and WILL use these mirrors as
    // UAVs (compute writes). Without ALLOW_UNORDERED_ACCESS the driver rejects any transition
    // into/out of UAV state at Close with E_INVALIDARG — so set it on every SINGLE-SAMPLE mirror.
    // Flag constraints we must respect when building the mirror's flag set:
    //   * MSAA (SampleDesc.Count > 1) CANNOT carry ALLOW_UNORDERED_ACCESS / ALLOW_RENDER_TARGET.
    //   * ALLOW_RENDER_TARGET and ALLOW_DEPTH_STENCIL are MUTUALLY EXCLUSIVE.
    //   * ALLOW_DEPTH_STENCIL is only legal on depth/stencil FORMATS. Cyberpunk's FG PREPARE passes a
    //     motion-vector texture (R16G16_FLOAT) whose desc carries ALLOW_DEPTH_STENCIL; inheriting that
    //     onto a non-depth float format is invalid and fails CreateCommittedResource with E_INVALIDARG
    //     on GPU B (seen in-game, task 7b). Our mirrors are compute targets for FFX — never real depth
    //     attachments (FFX reads depth via SRV/compute) — so drop any inherited ALLOW_DEPTH_STENCIL.
    //   * ALLOW_RENDER_TARGET is NOT legal on TEXTURE3D resources. Cyberpunk's MV texture is a
    //     depth-1 TEXTURE3D; adding RT to it failed with E_INVALIDARG (task 7b). Keep the dimension as-is
    //     (FFX creates Texture3D views over it, exactly as it does on the game's original resource) but
    //     skip the RT flag for 3D.
    bool msaa = (d.SampleDesc.Count > 1);
    bool is3D = (d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE3D);
    if (!msaa) d.SampleDesc.Quality = 0;   // Quality must be 0 when Count==1 — illegal otherwise
    d.Alignment = 0;   // committed mirrors can't carry the source's custom alignment (E_INVALIDARG); FFX doesn't need it
    D3D12_RESOURCE_FLAGS origFlags = d.Flags;

    // Build candidate flag sets to try, in order of preference. We don't always know which
    // combo the driver accepts for a given desc (format x dimension x flags interactions are
    // not fully documented), so we try them sequentially and log the winner.
    struct { D3D12_RESOURCE_FLAGS f; const char* label; } candidates[4];
    int nCand = 0;

    if (IsDepthFamilyFormat(d.Format)) {
        // Depth-family: this AMD driver ONLY accepts ALLOW_DEPTH_STENCIL (or none) — UAV and RT
        // are both E_INVALIDARG. The game's own resource carries DS, so preserve source flags
        // verbatim first, then fall back to no flags.
        candidates[nCand++] = { origFlags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL ?
                                D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL : (D3D12_RESOURCE_FLAGS)0, "srcDS" };
        if (!(origFlags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL))
            candidates[nCand++] = { D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL, "DS" };
    } else if (!msaa) {
        // Preferred: UAV + RT (most permissive for FFX which may use either view type)
        if (!is3D) {
            candidates[nCand++] = { D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, "UAV+RT" };
        }
        // UAV only (always legal on single-sample non-MSAA)
        candidates[nCand++] = { D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS, "UAV" };
    } else {
        // MSAA: no UAV/RT allowed — just inherit what's safe
        candidates[nCand++] = { (D3D12_RESOURCE_FLAGS)0, "none(MSAA)" };
    }
    // Fallback: source flags minus DS (in case the driver needs something specific we didn't add)
    D3D12_RESOURCE_FLAGS srcNoDS = d.Flags & ~D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    if (!msaa && !is3D) srcNoDS |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    candidates[nCand++] = { srcNoDS, "srcNoDS" };

    HRESULT hr = E_FAIL;
    int usedIdx = -1;
    for (int ci = 0; ci < nCand; ++ci) {
        d.Flags = candidates[ci].f;
        if (isBuf) {
            m->sizeBytes = d.Width;
            hr = h->devB->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m->resB));
        } else {
            m->width = (UINT)d.Width; m->height = (UINT)d.Height; m->format = d.Format;
            hr = h->devB->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &d,
                   D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&m->resB));
        }
        XLog("mirror combo '%s' flags=0x%X -> hr=0x%08X", candidates[ci].label, (unsigned)d.Flags, (unsigned)hr);
        if (SUCCEEDED(hr)) { usedIdx = ci; break; }
    }
    if (FAILED(hr)) {
        XLog("ERROR: mirror create failed all %d combos last hr=0x%08X (%s dim=%u %ux%ux%u fmt=0x%x mips=%u count=%u quality=%u layout=%u align=%llu srcFlags=0x%X msaa=%d is3D=%d)",
             nCand, (unsigned)hr, isBuf ? "buf" : "tex", d.Dimension, m->width, m->height, d.DepthOrArraySize,
             (unsigned)m->format, d.MipLevels, d.SampleDesc.Count, d.SampleDesc.Quality,
             (unsigned)d.Layout, (unsigned long long)d.Alignment, (unsigned)origFlags, msaa ? 1 : 0, is3D ? 1 : 0);
        return nullptr;
    }
    XLog("mirror OK via '%s' flags=0x%X", candidates[usedIdx].label, (unsigned)d.Flags);
    m->lastState = D3D12_RESOURCE_STATE_GENERIC_READ;   // creation state
    h->mirrorCount++;
    XLog("new mirror #%d: %s %ux%u fmt=0x%x size=%llu", h->mirrorCount - 1, isBuf ? "buf" : "tex",
         m->width, m->height, (unsigned)m->format, (unsigned long long)(isBuf ? m->sizeBytes : 0));
    return m;
}

// ---------------------------------------------------------------------------
// A -> B input bounce, BATCHED across all inputs of one dispatch.
//
// Task 6b: instead of serializing each input through its own pair of fence waits
// (A-readback wait + B-upload wait per input = 2N round-trips), we submit ALL
// inputs' A-side copies in ONE command list and wait ONCE, do all CPU memcpys
// back-to-back with a single Map/Unmap pair on each staging buffer, then submit
// ALL B-side uploads in ONE command list and wait ONCE. Total: 2 fence round-trips
// regardless of input count — the transfers are pipelined through shared GPU
// submissions rather than stalling the render thread between every resource.
// ---------------------------------------------------------------------------
bool xgpuBounceInputs(XGpuHub* h, FfxApiResource** inputs, int nIn) {
    XGpuMirror* mFor[8] = {}; uint64_t szFor[8] = {}, offA[8] = {}, offB[8] = {};
    bool isBufFor[8] = {}; UINT mipsFor[8] = {}; bool active[8] = {};
    uint64_t total = 0; int nAct = 0;

    // ---- Phase 0: resolve mirrors + compute per-input sizes/offsets (indexed by original i) ----
    for (int i = 0; i < nIn && i < 8; ++i) {
        if (!inputs[i]->resource) continue;
        ID3D12Resource* srcA = (ID3D12Resource*)inputs[i]->resource;
        XGpuMirror* m = GetOrCreateMirror(h, srcA);
        if (!m) return false;
        D3D12_RESOURCE_DESC d = srcA->GetDesc();
        bool isBuf = (d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER);
        uint64_t sz = 0; UINT mips = 1;
        if (isBuf) {
            sz = d.Width;
        } else {
            mips = d.MipLevels ? d.MipLevels : 1;
            if (mips > 16) { XLog("ERROR: too many mips %u", mips); return false; }
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[16]; UINT rows[16]; UINT64 pitch[16], slice[16];
            Footprints(h->devA, &d, mips, fp, rows, pitch, slice);
            for (UINT k = 0; k < mips; ++k) sz += slice[k];
        }
        mFor[i] = m; szFor[i] = sz; offA[i] = total; offB[i] = total;
        isBufFor[i] = isBuf; mipsFor[i] = mips; active[i] = true;
        total += sz; nAct++;
    }
    if (nAct == 0) return true;   // all inputs null — nothing to bounce

    // ---- Phase 1: ONE A-readback list for every input, single fence wait ----
    GrowBuffer(h, &h->rbA, &h->rbASize, h->devA, D3D12_HEAP_TYPE_READBACK,
               D3D12_RESOURCE_STATE_COPY_DEST, "A-readback", total);
    if (!h->rbA) return false;
    SafeReset(h->clA, h->allocA, &g_resetWarnedA);   // E_FAIL on fresh list is a known quirk — continue
    for (int i = 0; i < nIn && i < 8; ++i) {
        if (!active[i]) continue;
        ID3D12Resource* srcA = (ID3D12Resource*)inputs[i]->resource;
        D3D12_RESOURCE_DESC d = srcA->GetDesc();
        uint64_t base = offA[i];
        if (isBufFor[i]) {
            // Game resource: declared state == what the game left it in (we're mid-recording on its CL).
            Barrier(h->clA, srcA, FfxStateToD3d(inputs[i]->state), D3D12_RESOURCE_STATE_COPY_SOURCE);
            h->clA->CopyBufferRegion(h->rbA, base, srcA, 0, d.Width);
            Barrier(h->clA, srcA, D3D12_RESOURCE_STATE_COPY_SOURCE, FfxStateToD3d(inputs[i]->state));
        } else {
            UINT mips = mipsFor[i];
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[16]; UINT rows[16]; UINT64 pitch[16], slice[16];
            Footprints(h->devA, &d, mips, fp, rows, pitch, slice);
            Barrier(h->clA, srcA, FfxStateToD3d(inputs[i]->state), D3D12_RESOURCE_STATE_COPY_SOURCE);
            for (UINT k = 0; k < mips; ++k) {
                D3D12_TEXTURE_COPY_LOCATION dst{};
                dst.pResource = h->rbA; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                dst.PlacedFootprint.Offset = base + fp[k].Offset;
                dst.PlacedFootprint.Footprint.Format = CopyFormatFor(d.Format); // depth-view mapping
                dst.PlacedFootprint.Footprint.Width = fp[k].Footprint.Width;
                dst.PlacedFootprint.Footprint.Height = fp[k].Footprint.Height;
                dst.PlacedFootprint.Footprint.Depth = 1;
                dst.PlacedFootprint.Footprint.RowPitch = fp[k].Footprint.RowPitch;
                D3D12_TEXTURE_COPY_LOCATION src{};
                src.pResource = srcA; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                src.SubresourceIndex = k;
                h->clA->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            }
            Barrier(h->clA, srcA, D3D12_RESOURCE_STATE_COPY_SOURCE, FfxStateToD3d(inputs[i]->state));
        }
    }
    if (FAILED(h->clA->Close())) { XLog("ERROR: clA close (batch)"); return false; }
    ID3D12CommandList* lists[] = { h->clA };
    h->qA->ExecuteCommandLists(1, lists);
    h->fenceValA++;
    h->qA->Signal(h->fenceA, h->fenceValA);
    if (FAILED(h->fenceA->SetEventOnCompletion(h->fenceValA, h->evA))) { XLog("ERROR: fence A event"); return false; }
    if (WaitForSingleObject(h->evA, 30000) != WAIT_OBJECT_0) { XLog("ERROR: timeout waiting GPU-A batch readback"); return false; }

    // ---- Phase 2: CPU hop — all inputs rbA -> upB back-to-back (the RAM bounce), one Map/Unmap pair each ----
    GrowBuffer(h, &h->upB, &h->upBSize, h->devB, D3D12_HEAP_TYPE_UPLOAD,
               D3D12_RESOURCE_STATE_GENERIC_READ, "B-upload", total);
    if (!h->upB) return false;
    void* mUp = nullptr;
    if (FAILED(h->upB->Map(0, nullptr, &mUp))) { XLog("ERROR: upB map"); return false; }
    for (int i = 0; i < nIn && i < 8; ++i) {
        if (!active[i]) continue;
        D3D12_RANGE r{offA[i], offA[i] + (SIZE_T)szFor[i]};
        void* mRb = nullptr;
        if (FAILED(h->rbA->Map(0, &r, &mRb))) { XLog("ERROR: rbA map"); h->upB->Unmap(0, nullptr); return false; }
        memcpy((char*)mUp + offB[i], mRb, szFor[i]);
        h->rbA->Unmap(0, nullptr);
    }
    h->upB->Unmap(0, nullptr);

    // ---- Phase 3: ONE B-upload list for every input, single fence wait ----
    SafeReset(h->clB, h->allocB, &g_resetWarnedB);   // E_FAIL on fresh list is a known quirk — continue
    for (int i = 0; i < nIn && i < 8; ++i) {
        if (!active[i]) continue;
        XGpuMirror* m = mFor[i];
        D3D12_RESOURCE_DESC d = ((ID3D12Resource*)inputs[i]->resource)->GetDesc();
        uint64_t base = offB[i];
        if (isBufFor[i]) {
            Barrier(h->clB, h->upB, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_COPY_SOURCE);
            h->clB->CopyBufferRegion(m->resB, 0, h->upB, base, d.Width);
            Barrier(h->clB, h->upB, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_GENERIC_READ);
        } else {
            UINT mips = mipsFor[i];
            D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[16]; UINT rows[16]; UINT64 pitch[16], slice[16];
            Footprints(h->devB, &d, mips, fp, rows, pitch, slice);
            Barrier(h->clB, h->upB, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_COPY_SOURCE);
            // Our own mirror: use the tracked state (this driver rejects ALL_BARRIERS in transitions).
            Barrier(h->clB, m->resB, m->lastState, D3D12_RESOURCE_STATE_COPY_DEST);
            for (UINT k = 0; k < mips; ++k) {
                D3D12_TEXTURE_COPY_LOCATION src{};
                src.pResource = h->upB; src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                src.PlacedFootprint.Offset = base + fp[k].Offset;
                src.PlacedFootprint.Footprint.Format = CopyFormatFor(d.Format);
                src.PlacedFootprint.Footprint.Width = fp[k].Footprint.Width;
                src.PlacedFootprint.Footprint.Height = fp[k].Footprint.Height;
                src.PlacedFootprint.Footprint.Depth = 1;
                src.PlacedFootprint.Footprint.RowPitch = fp[k].Footprint.RowPitch;
                D3D12_TEXTURE_COPY_LOCATION dst{};
                dst.pResource = m->resB; dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                dst.SubresourceIndex = k;
                h->clB->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            }
            Barrier(h->clB, m->resB, D3D12_RESOURCE_STATE_COPY_DEST, FfxStateToD3d(inputs[i]->state));
            Barrier(h->clB, h->upB, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_GENERIC_READ);
        }
    }
    if (FAILED(h->clB->Close())) { XLog("ERROR: clB close (batch)"); return false; }
    ID3D12CommandList* listsB[] = { h->clB };
    h->qB->ExecuteCommandLists(1, listsB);
    h->fenceValB++;
    h->qB->Signal(h->fenceB, h->fenceValB);
    if (FAILED(h->fenceB->SetEventOnCompletion(h->fenceValB, h->evB))) { XLog("ERROR: fence B event"); return false; }
    if (WaitForSingleObject(h->evB, 30000) != WAIT_OBJECT_0) { XLog("ERROR: timeout waiting GPU-B batch upload"); return false; }

    // ---- swap every non-null input to its B-side mirror (state field stays as declared) ----
    for (int i = 0; i < nIn && i < 8; ++i) if (active[i]) inputs[i]->resource = mFor[i]->resB;
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

    // ---- locate the effect dispatch node in the chain (upscale OR frame generation) ----
    uint64_t nodeType = 0;
    ffxApiHeader* node = nullptr;
    for (const ffxApiHeader* n = desc; n; n = n->pNext) {
        if (n->type == FFX_API_DISPATCH_DESC_TYPE_UPSCALE ||
            n->type == FFX_API_DISPATCH_DESC_TYPE_UPSCALE_GENERATEREACTIVEMASK ||
            n->type == FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2 ||
            n->type == FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE ||   // deprecated v1, same field offsets
            n->type == FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION) {
            nodeType = n->type; node = (ffxApiHeader*)n; break;
        }
    }
    if (!node) { XLog("intercept: no upscale/FG node in chain (top=0x%llx)", (unsigned long long)desc->type); return FFX_API_RETURN_ERROR_PARAMETER; }

    // ---- collect input/output field pointers for this node type ----
    FfxApiResource* inputs[8] = {}; int nIn = 0;
    FfxApiResource* outputs[4] = {}; int nOut = 0;
    ID3D12GraphicsCommandList** pCmdList = nullptr;
    if (nodeType == FFX_API_DISPATCH_DESC_TYPE_UPSCALE) {
        ffxDispatchDescUpscale* u = (ffxDispatchDescUpscale*)node;
        inputs[nIn++] = &u->color;
        inputs[nIn++] = &u->depth;
        inputs[nIn++] = &u->motionVectors;
        inputs[nIn++] = &u->exposure;
        inputs[nIn++] = &u->reactive;
        inputs[nIn++] = &u->transparencyAndComposition;
        outputs[nOut++] = &u->output;
        pCmdList = (ID3D12GraphicsCommandList**)&u->commandList;
    } else if (nodeType == FFX_API_DISPATCH_DESC_TYPE_UPSCALE_GENERATEREACTIVEMASK) {
        ffxDispatchDescUpscaleGenerateReactiveMask* u = (ffxDispatchDescUpscaleGenerateReactiveMask*)node;
        inputs[nIn++] = &u->colorOpaqueOnly;
        inputs[nIn++] = &u->colorPreUpscale;
        outputs[nOut++] = &u->outReactive;
        pCmdList = (ID3D12GraphicsCommandList**)&u->commandList;
    } else if (nodeType == FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE_V2 ||
               nodeType == FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION_PREPARE) {
        // v1 and V2 share the same offsets for everything we touch (depth/MV/commandList).
        ffxDispatchDescFrameGenerationPrepareV2* p = (ffxDispatchDescFrameGenerationPrepareV2*)node;
        inputs[nIn++] = &p->depth;
        inputs[nIn++] = &p->motionVectors;
        // no outputs — dilation passes write into FFX-internal resources on B
        pCmdList = (ID3D12GraphicsCommandList**)&p->commandList;
    } else { // FFX_API_DISPATCH_DESC_TYPE_FRAMEGENERATION
        ffxDispatchDescFrameGeneration* g = (ffxDispatchDescFrameGeneration*)node;
        inputs[nIn++] = &g->presentColor;
        uint32_t nGen = g->numGeneratedFrames > 4 ? 4 : g->numGeneratedFrames;
        for (uint32_t i = 0; i < nGen; ++i) outputs[nOut++] = &g->outputs[i];
        pCmdList = (ID3D12GraphicsCommandList**)&g->commandList;
    }

    // ---- save originals for restore (per field index — robust to partial failure) ----
    FfxApiResource origByField[8]; bool wasSwapped[8] = {};
    ID3D12GraphicsCommandList* origCmdList = *pCmdList;
    FfxApiResource origOuts[4]; bool outSwapped[4] = {};
    for (int i = 0; i < nOut; ++i) origOuts[i] = *outputs[i];
    ffxReturnCode_t rc = FFX_API_RETURN_ERROR;

    double t0 = NowMs();

    // ---- bounce every non-null input A -> B (batched — task 6b) ----
    for (int i = 0; i < nIn; ++i) {
        if (!inputs[i]->resource) continue;
        origByField[i] = *inputs[i];
        wasSwapped[i] = true;   // restore is a no-op if the bounce fails before swapping anything
        XLog("bounce in[%d]: %s %ux%u fmt=0x%x state=0x%x", i,
             inputs[i]->description.type == FFX_API_RESOURCE_TYPE_BUFFER ? "buf" : "tex",
             inputs[i]->description.width, inputs[i]->description.height,
             (unsigned)inputs[i]->description.format, (unsigned)inputs[i]->state);
    }
    if (nIn > 8) { XLog("ERROR: too many inputs (%d), max 8 — aborting dispatch", nIn); goto restore; }
    if (!xgpuBounceInputs(h, inputs, nIn)) { XLog("ERROR: batched input bounce failed — aborting dispatch"); goto restore; }

    // ---- outputs: point FFX at B-side mirrors of the game's output textures (up to 4 for FG) ----
    for (int i = 0; i < nOut; ++i) {
        if (!outputs[i]->resource) continue;
        XGpuMirror* mOut = GetOrCreateMirror(h, (ID3D12Resource*)outputs[i]->resource);
        if (!mOut) goto restore;
        outputs[i]->resource = mOut->resB;
        outSwapped[i] = true;
    }

    // ---- hand FFX our GPU-B command list and record (E_FAIL on fresh list is a known quirk) ----
    HRESULT hr = SafeReset(h->clB, h->allocB, &g_resetWarnedB);
    *pCmdList = h->clB;

    double t1 = NowMs();
    rc = rd(&c, desc);   // SDK signature takes ffxContext* (pointer to handle)
    double t2 = NowMs();
    if (rc != FFX_API_RETURN_OK) { XLog("real ffxDispatch failed rc=%u", rc); goto restore; }

    // ---- append output readbacks to the SAME open list FFX recorded into, execute on B ----
    ID3D12Resource* upAUsed[4] = {};   // A-side upload slot per swapped output (nullptr if not captured)
    {
        uint64_t totalNeed = 0;
        for (int i = 0; i < nOut; ++i)
            if (outSwapped[i]) {
                D3D12_RESOURCE_DESC d = ((ID3D12Resource*)outputs[i]->resource)->GetDesc();
                if (d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) totalNeed += d.Width;
                else {
                    UINT mips = d.MipLevels ? d.MipLevels : 1;
                    if (mips <= 16) {
                        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[16]; UINT rows[16]; UINT64 pitch[16], slice[16];
                        Footprints(h->devB, &d, mips, fp, rows, pitch, slice);
                        for (UINT k = 0; k < mips; ++k) totalNeed += slice[k];
                    }
                }
            }

        if (totalNeed > 0) {
            GrowBuffer(h, &h->rbB, &h->rbBSize, h->devB, D3D12_HEAP_TYPE_READBACK,
                       D3D12_RESOURCE_STATE_COPY_DEST, "B-readback", totalNeed);
            if (h->rbB) {
                uint64_t off = 0;
                for (int i = 0; i < nOut && h->rbB; ++i) {
                    if (!outSwapped[i]) continue;
                    ID3D12Resource* srcB = (ID3D12Resource*)outputs[i]->resource;   // our B-side mirror
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
                            for (UINT k = 0; k < mips; ++k) need += slice[k];
                        } else continue;   // too many mips — skip this output
                    }

                    if (isBuf) {
                        // FFX may have left the mirror in any state after its passes — transition from COMMON(0).
                        Barrier(h->clB, srcB, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
                        h->clB->CopyBufferRegion(h->rbB, off, srcB, 0, d.Width);
                        Barrier(h->clB, srcB, D3D12_RESOURCE_STATE_COPY_SOURCE, FfxStateToD3d(origOuts[i].state));
                    } else {
                        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[16]; UINT rows[16]; UINT64 pitch[16], slice[16];
                        Footprints(h->devB, &d, mips, fp, rows, pitch, slice);
                        // FFX may have left the mirror in any state after its passes — transition from COMMON(0)
                        // (this driver rejects ALL_BARRIERS and UAV barriers; verified xgpu_probe18 case g).
                        Barrier(h->clB, srcB, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
                        for (UINT k = 0; k < mips; ++k) {
                            D3D12_TEXTURE_COPY_LOCATION dst{};
                            dst.pResource = h->rbB; dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                            dst.PlacedFootprint.Offset = off + fp[k].Offset;
                            dst.PlacedFootprint.Footprint.Format = CopyFormatFor(d.Format);
                            dst.PlacedFootprint.Footprint.Width = fp[k].Footprint.Width;
                            dst.PlacedFootprint.Footprint.Height = fp[k].Footprint.Height;
                            dst.PlacedFootprint.Footprint.Depth = 1;
                            dst.PlacedFootprint.Footprint.RowPitch = fp[k].Footprint.RowPitch;
                            D3D12_TEXTURE_COPY_LOCATION src{};
                            src.pResource = srcB; src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                            src.SubresourceIndex = k;
                            h->clB->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                        }
                        Barrier(h->clB, srcB, D3D12_RESOURCE_STATE_COPY_SOURCE, FfxStateToD3d(origOuts[i].state));
                    }
                    off += need;
                }

                if (FAILED(h->clB->Close())) { XLog("ERROR: clB close (output) failed"); }
                else {
                    ID3D12CommandList* lists[] = { h->clB };
                    h->qB->ExecuteCommandLists(1, lists);
                    h->fenceValB++;
                    h->qB->Signal(h->fenceB, h->fenceValB);
                    if (FAILED(h->fenceB->SetEventOnCompletion(h->fenceValB, h->evB))) XLog("ERROR: fence B event");
                    else if (WaitForSingleObject(h->evB, 30000) != WAIT_OBJECT_0) XLog("ERROR: timeout waiting GPU-B output readback");

                    // CPU hop: each captured output -> its own A-side upload ring slot
                    uint64_t off2 = 0;
                    for (int i = 0; i < nOut && h->rbB; ++i) {
                        if (!outSwapped[i]) continue;
                        ID3D12Resource* srcB = (ID3D12Resource*)outputs[i]->resource;
                        D3D12_RESOURCE_DESC d = srcB->GetDesc();
                        bool isBuf = (d.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER);
                        uint64_t need = 0;
                        UINT mips = 1;
                        if (isBuf) need = d.Width;
                        else {
                            mips = d.MipLevels ? d.MipLevels : 1;
                            D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp[16]; UINT rows[16]; UINT64 pitch[16], slice[16];
                            if (mips <= 16) {
                                Footprints(h->devB, &d, mips, fp, rows, pitch, slice);
                                for (UINT k = 0; k < mips; ++k) need += slice[k];
                            } else continue;
                        }

                        D3D12_RANGE r{off2, off2 + (SIZE_T)need};
                        void* mRb = nullptr;
                        if (FAILED(h->rbB->Map(0, &r, &mRb))) { XLog("ERROR: rbB map failed"); continue; }
                        int slotIdx = h->outNext % XGPU_OUT_SLOTS;
                        OutSlot* os = &h->outSlots[slotIdx];
                        if (!os->upA || os->size < need) {
                            if (os->upA) { os->upA->Release(); os->upA = nullptr; }
                            uint64_t sz = need < (1u << 20) ? (1u << 20) : need;
                            HRESULT hr2 = MakeBuffer(h->devA, D3D12_HEAP_TYPE_UPLOAD, sz,
                                                     D3D12_RESOURCE_STATE_GENERIC_READ, &os->upA);
                            if (FAILED(hr2)) { XLog("ERROR: out-slot upload create 0x%08X", (unsigned)hr2); h->rbB->Unmap(0, nullptr); continue; }
                            os->size = sz;
                        }
                        void* mUpA = nullptr;
                        if (FAILED(os->upA->Map(0, nullptr, &mUpA))) { XLog("ERROR: out-slot upload map failed"); h->rbB->Unmap(0, nullptr); continue; }
                        memcpy(mUpA, mRb, need);
                        os->upA->Unmap(0, nullptr);
                        upAUsed[i] = os->upA;
                        h->outNext++;
                        h->rbB->Unmap(0, nullptr);
                        off2 += need;
                    }
                }
            }
        }
    }

    // ---- record the copy-backs into the GAME's command list (still open — mid-recording) ----
    int anyCopied = 0, anySwapped = 0;
    for (int i = 0; i < nOut; ++i) {
        if (!outSwapped[i]) continue;
        anySwapped++;
        if (upAUsed[i] && origCmdList) { xgpuRecordOutputCopyBack(h, origCmdList, &origOuts[i], upAUsed[i]); anyCopied++; }
    }
    if (anySwapped > anyCopied)
        XLog("WARNING: %d/%d output captures failed — game's output textures hold stale data this frame",
             anySwapped - anyCopied, anySwapped);

    double t3 = NowMs();
    h->dispatches++;
    h->msInputs += t1 - t0;
    h->msFfxRecord += t2 - t1;
    h->msCapture += t3 - t2;
    // Per-frame latency: total is the end-to-end cost added to this frame's ffxDispatch call
    // (v1 is fully synchronous, so it lands entirely on the game thread). EMA = steady-state
    // estimate; max = worst case (first frames include shader compile + buffer growth).
    double totalMs = t3 - t0;
    if (totalMs > h->msTotalMax) h->msTotalMax = totalMs;
    h->msTotalEma = (h->dispatches == 1) ? totalMs : h->msTotalEma * 0.9 + totalMs * 0.1;
    if (h->dispatches <= 3 || h->dispatches % 60 == 1) {
        XLog("stats #%llu: inputs=%.2f ffx_record=%.2f capture+copyback=%.2f | frame_total=%.2fms ema=%.2fms max=%.2fms",
             (unsigned long long)h->dispatches, h->msInputs / h->dispatches,
             h->msFfxRecord / h->dispatches, h->msCapture / h->dispatches,
             totalMs, h->msTotalEma, h->msTotalMax);
    }

restore:
    // restore the desc exactly as the game passed it (per field, only what we swapped)
    for (int i = 0; i < nIn; ++i) if (wasSwapped[i]) inputs[i]->resource = origByField[i].resource;
    *pCmdList = origCmdList;
    for (int i = 0; i < nOut; ++i) if (outSwapped[i]) outputs[i]->resource = origOuts[i].resource;

    return rc;
}

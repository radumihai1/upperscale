// upperscale_xgpu.h — cross-GPU (A->B / B->A) texture transfer hub for the proxy DLL.
//
// Hardware constraint: CrossNodeSharingTier == 0 on both GPUs, so no cross-adapter shared
// heaps and no VRAM-to-VRAM P2P. The only validated path is a CPU RAM bounce:
//   A: tex -> readback buffer (COPY) -> Map/memcpy
//   B: upload buffer (Map'd) -> tex (COPY, PLACED_FOOTPRINT)
// Validated in tools/xgpu_probe4.cpp (~1.3 ms one-way at 512x288 RGBA8).
//
// FFX dispatch semantics: ffxDispatch RECORDS compute into the provided command list; it does
// not execute. The proxy therefore hands FFX its own GPU-B command list, executes it right
// after, appends an output readback to the same list (keeps ordering on B), and records the
// output copy-back into the GAME's original command list with symmetric barriers.
//
// v1 is fully synchronous: every bounce completes before ffxDispatch returns. Consequence in
// single-submit engines: input textures are read before this frame's render passes have
// executed, so FFX sees last-executed-frame inputs (a mutually consistent set -> coherent
// temporal history, +1 frame latency). Task 6 replaces this with an async pipeline.
//
// NOTE: this header deliberately does NOT include the FFX SDK headers — ffx_api.h declares its
// entry points with __declspec(dllexport), which would collide with our own definitions in the
// proxy DLL (C2733). The .cpp includes what it needs; ABI compatibility is by layout, not by
// shared declarations.

#pragma once
#include <d3d12.h>
#include <dxgi1_6.h>
#include <windows.h>
#include <stdint.h>
#include <stdbool.h>

struct FfxApiResource;   // from ffx_api_types.h — forward decl keeps this header light

// Opaque hub: one per (gameDevice, gpuBDevice) pair. Full definition in upperscale_xgpu.cpp.
typedef struct XGpuHub XGpuHub;

// Create/return the hub for this (gameDevice, gpuBDevice) pair. AddRefs gameDevice.
XGpuHub* xgpuGetOrCreateHub(ID3D12Device* devA, ID3D12Device* devB);

// Synchronously bounce one input resource from A to B: readback on our qA (barriers around the
// declared state), CPU memcpy into the B upload buffer, upload into a cached B-side mirror.
// On success sets res->resource to the B-side mirror and returns true. The caller must restore
// the original pointer after ffxDispatch returns (FFX only records — it does not consume).
bool xgpuBounceInput(XGpuHub* hub, FfxApiResource* res);

// Record the output copy-back into the game's command list:
//   barrier(uploadA GR->CS), barrier(gameTex <declared>->COPY_DEST),
//   CopyTextureRegion(PLACED_FOOTPRINT uploadA -> gameTex subresource 0..mips-1),
//   restore both. Symmetric with the state the game declared in resOut->state.
void xgpuRecordOutputCopyBack(XGpuHub* hub, ID3D12GraphicsCommandList* clGame,
                              FfxApiResource* resOut, ID3D12Resource* uploadA);

// One-shot cross-GPU dispatch: bounces all inputs A->B, swaps the command list + output to B-side
// mirrors, calls realDispatch (records into our GPU-B CL), executes it on GPU B, captures the
// output back through RAM into a rotating A-side UPLOAD slot, records the copy-back into the
// game's original command list, and restores every desc field. Returns FFX's return code.
// ctx is the ffxContext handle (void*); realDispatch has AMD's PfnFfxDispatch signature.
uint32_t xgpuInterceptDispatch(XGpuHub* hub, void* ctx, const void* desc, void* realDispatch);

// Release a hub (called on context destroy / process detach).
void xgpuReleaseHub(XGpuHub* hub);

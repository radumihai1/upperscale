# UPPERSCALE — HANDOFF / NEXT STEPS (read this first when resuming)

Single source of truth for picking up work in a fresh session. Keep it updated as you go.

## FINAL GOAL
A Windows tool ("upperscale") that lets **GPU A** do the heavy raster rendering of a game while
**GPU B** does FSR/FSR4 upscaling + frame generation, by intercepting AMD's FidelityFX DLL calls
and rerouting all FFX compute to GPU B. The game must have FSR enabled (that is what produces the
motion vectors / depth / jitter we need); we do NOT disable it — we swap which device runs the math.

Deliverable: a drop-in replacement `amd_fidelityfx_dx12.dll` + a small config/CLI tool, open source,
MIT/Apache-licensed, vendoring AMD's official FidelityFX SDK (permissive) rather than OptiScaler GPL code.

## HARDWARE (this box — Windows 11 build 26200)
- GPU A = RX 7900 XTX (RDNA3, 24GB), LUID {0x0000, 0x2A089} — renders the game (user pins via Windows GPU Preference).
- GPU B = RX 9060 XT (RDNA4, 16GB), LUID {0x0000, 0x27214} — owns both displays; will run upscaling/FG.
- Both discrete AMD cards on the same PCIe root complex. No NVLink-class P2P → cross-GPU transfer is a
  pinned-system-RAM bounce over PCIe (fast enough: ~11 MB/frame @720p, pipelined).

## ARCHITECTURE (decided)
DLL replacement (OptiScaler-proven pattern):
1. Drop our `amd_fidelityfx_dx12.dll` into the game folder. Game loads it thinking it's AMD's.
2. Intercept `ffxCreateContext(device, ...)`: create OUR OWN D3D12 device on GPU B by LUID (bypasses
   Windows GPU preference), pass THAT to the real FFX context. Keep a reference to the game's original
   device (GPU A) for reading its textures.
3. On each `ffxDispatch(...)`: game passes its input textures (color/depth/MV, on GPU A). We copy them
   A→B via cross-adapter shared heap + async CopyResource + fences, call the real FSR4 upscaler / FG DLL
   with GPU-B-resident resources, and return the output texture.
4. Double/triple buffer so GPU B upscales frame N while GPU A renders/ships frame N+1 (hide copy latency).

FFX API flow (from vendored SDK headers): game → `ffxCreateContext` / `ffxDispatch`; resources passed via
backend callbacks; DX12 backend in `api_ffx_api_dx12.h`. AMD loader DLL is a ~26KB stub that loads the real
upscaler/FG DLLs — our proxy must match its export table (use tools/dump_exports.cpp to enumerate it).

## REPO LAYOUT
- third_party/FidelityFX-SDK/   — AMD official SDK clone. Signed FSR4 DLLs in Kits/FidelityFX/signedbin/:
  amd_fidelityfx_loader_dx12.dll, amd_fidelityfx_upscaler_dx12.dll, amd_fidelityfx_framegeneration_dx12.dll.
  Headers in Kits/FidelityFX/api/include/. HLSL FG source (optical flow / interp / inpaint) also present.
- third_party/dx-samples/       — Microsoft DirectX-Graphics-Samples clone (D3D12LinkedGpus = reference).
- tools/device_probe.cpp        — enumerate DXGI adapters, print LUIDs, create D3D12 device by LUID. WORKS.
- tools/xgpu_test.cpp           — cross-adapter transfer round-trip test (A→B pixel check). BLOCKED (see below).
- tools/xgpu_probe2.cpp         — isolates which cross-adapter heap configs work. WORKS, output in build/probe2_out.txt.
- tools/dump_exports.cpp        — list a DLL's exported functions via GetProcAddress. (build when needed)
- src/proxy/                    — the actual proxy DLL goes here (NOT STARTED).
- docs/, tests/, build/         — docs / test harness / build artifacts + msvc_env.sh.

## BUILD COMMAND THAT WORKS (git-bash, MSVC 14.51 BuildTools)
```
cd C:/Users/mrmih/Playground/AI/upperscale
source build/msvc_env.sh
export MSYS_NO_PATHCONV=1
cl.exe /nologo /EHsc /O2 /TP tools/xgpu_test.cpp /Fe:build/xgpu_test.exe \
  "/link" "/LIBPATH:C:\Program Files (x86)\Windows Kits\10\Lib\10.0.26100.0\um\x64" d3d12.lib dxgi.lib user32.lib
./build/xgpu_test.exe
```
Build gotchas (all hit and solved):
- Use `/TP` to force C++ — MSYS mangles .cpp extension detection, cl treats it as C otherwise.
- `MSYS_NO_PATHCONV=1` is REQUIRED for `/LIBPATH:C:\...` (backslash Windows path) or link mangles it into a .obj name.
- In build/msvc_env.sh: PATH entries MUST be forward-slash MSYS form (`/c/Program Files ...`) so bash can traverse;
  INCLUDE/LIB are native `C:\...` form for cl/link. (A backslash PATH entry silently breaks `which cl.exe`.)
- Compiler: C:\Program Files (x86)\Microsoft Visual Studio\18\BuildTools, toolset 14.51.36231.
- Windows SDK headers/libs at ...\Windows Kits\10\Include|Lib\10.0.26100.0 (also 10.0.22621.0 present).

## THIS MACHINE'S d3d12.h IS OLD (um/, Windows-10-era) — API DIFFERENCES
The installed header is NOT the modern shared/ one. Signatures that differ from what you'd expect:
- `CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE, ...)` takes an ENUM directly (no D3D12_COMMAND_ALLOCATION_DESC struct).
- No `OpenSharedResource` — it's **`OpenSharedHandle(HANDLE, REFIID, void**)`** on ID3D12Device.
- `CreateSharedHandle(resource, securityAttrs, access, name, HANDLE*)` — 5 args (has a Name param).
- `D3D12_TEXTURE_COPY_LOCATION{resource, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX, {subres}}` needs explicit Type + SubresourceIndex.
- `CreateCommittedResource(props, heapFlags, desc, initialState, deniedDescs, iid)` — 6 args (no resource-flags slot).
- `CopyTextureRegion(&dstLoc, x,y,z, &srcLoc, box*)`.
If you keep hitting E_INVALIDARG on a call that "should" work, suspect an old-header signature mismatch and grep the local um/d3d12.h for the exact declaration.

## CURRENT BLOCKER (the thing to solve next)
Cross-adapter shared heap **creation** SUCCEEDS but resource **placement** fails:
  - `CreateHeap(CUSTOM L0 + D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER)` → OK
  - `CreatePlacedResource` / `CreateCommittedResource` on it (buffer AND texture, with ALLOW_CROSS_ADAPTER) → E_INVALIDARG (0x80070057)
  - EVEN the control case (plain same-adapter shared heap) fails placement identically.
Interpretation: uniform failure across all cases ⇒ likely a header-signature mismatch or test bug, NOT necessarily a hard driver limit. Investigate in this order:
  1. Grep local um/d3d12.h for the exact `CreatePlacedResource` / `CreateCommittedResource` declarations; match arg count/types precisely (old-header trap).
  2. Try creating resources with initial state D3D12_RESOURCE_STATE_COMMON instead of COPY_DEST.
  3. If still failing, consider the ALTERNATIVE mechanism below.

## KEY DISCOVERY: Microsoft's multi-GPU sample uses the AFFINITY LAYER, not cross-adapter heaps
third_party/dx-samples/Samples/Desktop/D3D12LinkedGpus/ does NOT use D3D12_HEAP_FLAG_SHARED_CROSS_ADAPTER.
It uses `IDXGIAffinityFactory` / an affinity proxy device + **NodeMask** on queues/PSOs/root-signatures, and
an UPLOAD heap with `VisibleNodeMask` for CPU-visible data shared across nodes (see src/LinkedGpus/CrossNodeResources.cpp).
Caveat: the affinity layer requires both GPUs on one root complex AND driver support, and it models ONE device
spanning nodes — which does NOT directly fit our "the game owns its own GPU-A device" DLL-proxy model. So cross-adapter
shared heaps remain the primary plan; affinity is a fallback/learning reference only.

## NEXT STEPS (in order)
1. [ ] Solve the cross-adapter placement E_INVALIDARG (steps above). Get tools/xgpu_test.cpp to print "PASS: pixel round-trip A->B". This de-risks the whole project — if we can't move a texture GPU A→GPU B, nothing else works.
2. [ ] If cross-adapter truly blocked on this driver: prototype the pinned-RAM fallback (game-side Map/ReadBack to CPU → write to GPU-B upload heap) and measure per-frame latency; decide if acceptable.
3. [ ] Build src/proxy/: DLL exporting the FFX loader symbols (match amd_fidelityfx_loader_dx12.dll export table via tools/dump_exports.cpp). Intercept ffxCreateContext → create GPU-B device by LUID from config, pass to real context.
4. [ ] Wire cross-GPU input transfer into ffxDispatch: copy color/depth/MV A→B (shared heap or RAM fallback), call real FSR4 upscaler DLL with B-resident resources, return output. Validate vs single-GPU run for parity.
5. [ ] Add frame-generation path on GPU B (call amd_fidelityfx_framegeneration_dx12.dll) + double/triple buffering + end-to-end latency measurement.
6. [ ] Config tool: pick GPU B by LUID (list adapters like device_probe), CLI args, write a small config the proxy reads at load.
7. [ ] End-to-end test in a real FSR-enabled game; capture before/after frames; document added latency.
8. [ ] Docs (README build/run/troubleshoot), tests, push to https://github.com/radumihai1/upperscale.git

## STATUS SNAPSHOT (as of last update)
- Repo set up at C:\Users\mrmih\Playground\AI\upperscale, git branch main. Remote origin = github radumihai1/upperscale (authed via credential manager; remote already had LICENSE + README).
- FidelityFX SDK vendored (signed FSR4 DLLs present). dx-samples cloned.
- device_probe WORKS (enumerates both GPUs by LUID, creates devices). xgpu_probe2 WORKS (output captured).
- xgpu_test BLOCKED on cross-adapter placement E_INVALIDARG — this is the active task.
- Proxy DLL NOT started. No commits pushed yet beyond remote's initial LICENSE/README.

## DO / DON'T
- DO kill any background test processes after verifying (user is sensitive to leftover servers).
- DO keep this file current; it is the resume point.
- DON'T vendor OptiScaler GPL code into our MIT/Apache project — use AMD's permissive FidelityFX SDK as reference + dependency only.
- DON'T leave GUI/test apps running on the user's desktop after verification.

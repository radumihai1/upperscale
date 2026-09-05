# UPPERSCALE — HANDOFF / NEXT STEPS (read this first when resuming)

Single source of truth for picking up work in a fresh session. Keep it updated as you go, then commit+push.
Last updated after the cross-GPU transfer path was VALIDATED working (xgpu_probe4 PASS).

## FINAL GOAL
A Windows tool ("upperscale") that lets **GPU A** do the heavy raster rendering of a game while
**GPU B** does FSR/FSR4 upscaling + frame generation, by intercepting AMD's FidelityFX DLL calls
and rerouting all FFX compute to GPU B. The game must have FSR enabled (that is what produces the
motion vectors / depth / jitter we need); we do NOT disable it — we swap which device runs the math.

Deliverable: a drop-in replacement `amd_fidelityfx_dx12.dll` + a small config/CLI tool, open source,
MIT/Apache-licensed, vendoring AMD's official FidelityFX SDK (permissive) rather than OptiScaler GPL code.

## HARDWARE (this box — Windows 11 build 26200)
- GPU A = RX 7900 XTX (RDNA3, 24GB), LUID {0x0000, 0x2A089} — renders the game. NO physical display attached.
- GPU B = RX 9060 XT (RDNA4, 16GB), LUID {0x0000, 0x27214} — owns BOTH displays; will run upscaling/FG.
- Both discrete AMD cards on the same PCIe root complex. No NVLink-class P2P.

## ✅ TRANSFER PATH VALIDATED (the core feasibility question is ANSWERED)
tools/xgpu_probe4.cpp prints **PASS: all 589824 bytes identical GPU A -> CPU RAM -> GPU B** at 512x288 RGBA8.
Measured one-way per frame (single-pass, no pipelining yet):
  - GPU A readback path : ~0.68 ms   (texture->readback buffer + fence)
  - CPU memcpy hop      : ~0.03 ms   (~22 GB/s effective — this is the actual cross-GPU transfer)
  - GPU B upload path   : ~0.58 ms   (upload buffer->texture + verify copy)
  - TOTAL one-way       : ~1.29 ms @ 512x288; scales linearly -> ~2-3 ms @720p, ~4-6 ms @1080p.
This is acceptable and pipelinable (overlap readback of frame N with render/upload of N+1). The design works.

## ⚠️ CRITICAL FINDING: cross-adapter shared heaps are NOT supported here
tools/xgpu_probe3.cpp queries `D3D12_FEATURE_DATA_D3D12_OPTIONS` on BOTH GPUs:
  - `CrossNodeSharingTier = 0` (NOT_SUPPORTED)   and   `CrossAdapterRowMajorTextureSupported = 0`
Consequence: any resource flagged `D3D12_RESOURCE_FLAG_ALLOW_CROSS_ADAPTER` is rejected E_INVALIDARG.
So the ONLY way to move a texture GPU A -> GPU B on this hardware is the **CPU RAM bounce** above
(readback buffer on A -> Map/memcpy -> upload buffer on B). Do NOT re-attempt cross-adapter shared heaps.

## 🔧 AMD DRIVER QUIRKS ON THIS BOX (cost hours; do not rediscover)
These are real driver behaviors, NOT header bugs or test mistakes. All confirmed by isolated probes:
1. **`CopyTextureRegion` with a BUFFER as source/dest via `SUBRESOURCE_INDEX` -> INVALID_CALL**
   (0x887A0001), which then puts the device in removed state (subsequent calls return 0x887A0005).
   FIX: for any buffer<->texture copy, use **`D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT`** with a filled-in
   `PlacedFootprint` (Offset=0, Format=R8G8B8A8_UNORM, Width/Height, Depth=1, RowPitch=W*4). See BufLoc() in probe4.
   Texture<->texture copies via SUBRESOURCE_INDEX work fine; buffer<->buffer CopyBufferRegion works fine.
2. **`Map()` with range `{0, SIZE_MAX}` is rejected** (E_INVALIDARG 0x80070057). Pass an explicit byte count
   (`D3D12_RANGE{0, BYTES}`) or `nullptr`. Confirmed in probe8: same buffer, {0,-1} fails, {0,BYTES} succeeds.
3. **READBACK buffers can be created in COPY_DEST state and still Map fine** — no barrier to GENERIC_READ needed
   for the CPU readback hop (probe7/8). UPLOAD buffers must be GENERIC_READ to Map; a copy SOURCE buffer must be
   transitioned to COPY_SOURCE before CopyTextureRegion/CopyBufferRegion.
4. **`D3D12_RESOURCE_DESC.SampleDesc.Count` MUST be set to 1** for buffers too — a zero-initialized desc has
   Count=0 and CreateCommittedResource returns E_INVALIDARG (this silently broke every buffer in early probes).

## ✅ CORE ASSUMPTION PROVEN (ffx_bindtest PASS on BOTH GPUs)
tools/ffx_bindtest.cpp loads the REAL signed `amd_fidelityfx_upscaler_dx12.dll` (FSR4 v4.1.1), creates a
D3D12 device BY LUID, and calls its `ffxCreateContext`. Result: **PASS on both GPU A (7900 XTX) and
GPU B (9060 XT)** — context created rc=0, provider version query returns "4.1.1", clean destroy.
This proves the whole architecture at the API level: we CAN bind FFX to a LUID-selected device.

Key facts learned building it (do not rediscover):
- The signed upscaler imports `amdxc64.dll` + `dxgi.dll` (AMD driver runtime) — present in System32, loads fine.
- `ffxQuery(GET_VERSIONS)` is TWO-PHASE: call 1 with only `outputCount` set -> returns count; call 2 with
  `versionIds`+`versionNames` arrays filled to get the ids/names. If you forget `outputCount`, count reads 0.
- Available upscaler versions on this box: **4.1.1 (FSR4)**, 3.1.5, 2.3.4. Use id from version[0] in an
  `ffxOverrideVersion` node chained into the create-desc for FSR4.
- Create-desc chain a game builds: `ffxCreateContextDescUpscale -> [ffxOverrideVersion] -> ffxCreateBackendDX12Desc{device} -> (optional allocCallbacks)`. The `.device` in the backend node is THE POINTER WE SWAP to GPU B.
- If you install custom resource-allocator callbacks, you MUST honor `pHeapProps->Type` (FFX requests UPLOAD for mappable buffers); forcing DEFAULT makes FFX crash inside create. Simplest: don't install alloc callbacks at all — let FFX use its own default allocator on the backend device (what real games do).

## ✅ PROXY DLL SKELETON BUILT + VERIFIED (task 4b DONE)
src/proxy/upperscale_proxy.cpp builds `build/amd_fidelityfx_dx12.dll` (drop-in for AMD's loader).
Exports EXACTLY the 5 FFX symbols (+ internal upperscaleGetState helper). Two modes, both SMOKE-PASS:

- **PASSTHROUGH** (default): forwards all 5 calls untouched to AMD's real loader (bundled as
  `upperscale_real_loader.dll` next to us, or UPPERSCALE_REAL_LOADER path). Verified: FSR4 context
  created on GPU A, provider "4.1.1", clean destroy — behaviorally identical to AMD's DLL. This is the
  SAFE first in-game test (proves drop-in works without changing where FFX runs).
- **ACTIVE** (UPPERSCALE_ENABLE=1 + UPPERSCALE_GPU_LUID=<hex>): walks the pNext desc chain on create/query,
  finds the backend-DX12 node, SWAPS its ID3D12Device* for a device we created BY LUID on GPU B. Verified:
  "created GPU-B device by LUID {0,27214}: AMD Radeon RX 9060 XT" + "SWAP: backend device A -> B", FSR4
  context bound to GPU B rc=0, provider "4.1.1".

tools/ffx_smoke.cpp is the harness (simulates a game on GPU A, loads our DLL, checks upperscaleGetState).
Run it from a dir containing: amd_fidelityfx_dx12.dll (ours) + upperscale_real_loader.dll (AMD's loader) +
the effect DLLs (upscaler/framegeneration) — see build/smoke/ for the working layout.

Config (env or upperscale.ini [proxy] in CWD): UPPERSCALE_ENABLE, UPPERSCALE_GPU_LUID(_HI),
UPPERSCALE_REAL_LOADER, UPPERSCALE_LOG (0 off / 1 create+destroy / 2 verbose). Log -> <CWD>\upperscale.log.

⚠️ KNOWN LIMITATION (task 5 pending): in ACTIVE mode ffxDispatch is NOT yet wired for cross-GPU transfer —
the context is bound to GPU B but the game's command list + input textures live on GPU A, so forwarding would
be undefined behavior. The proxy therefore returns FFX_API_RETURN_ERROR from dispatch in active mode (logged).
Use PASSTHROUGH for real-game testing until task 5 lands.

⚠️ Windows DLL search order gotcha (cost a debug cycle): LoadLibraryA("name.dll") searches the EXE's own dir
before CWD. When running ffx_smoke.exe, put it in the SAME dir as our proxy + real loader, or pass an explicit
UPPERSCALE_REAL_LOADER path. The proxy itself resolves its bundled real-loader relative to ITS OWN module dir
(GetModuleFileNameA(g_hInst)), which is correct for the drop-in case (proxy sits next to the game's DLLs).

## ✅ EXPORT TABLE (all 3 effect DLLs identical)
tools/dump_exports.cpp (rewritten to read PE from disk — LoadLibraryExA is unreliable here) shows each of
loader/upscaler/framegeneration exports EXACTLY 5 symbols: `ffxConfigure, ffxCreateContext, ffxDestroyContext, ffxDispatch, ffxQuery`. Our proxy must export these same 5. The loader `LoadLibraryA`s effect DLLs by name (upscaler/FG/denoiser/radiancecache) — our proxy replicates that routing + adds the device swap.

## ARCHITECTURE (decided, transfer path now proven)
DLL replacement (OptiScaler-proven pattern):
1. Drop our `amd_fidelityfx_dx12.dll` into the game folder. Game loads it thinking it's AMD's.
2. Intercept `ffxCreateContext(device, ...)`: create OUR OWN D3D12 device on GPU B by LUID (bypasses Windows GPU
   preference), pass THAT to the real FFX context. Keep a reference to the game's original device (GPU A) for reading its textures.
3. On each `ffxDispatch(...)`: game passes input textures (color/depth/MV, resident on GPU A). Move them via the
   validated CPU RAM bounce: CopyTextureRegion(texA -> readback buffer A, PLACED_FOOTPRINT) + fence; Map/memcpy to a
   pinned/upload buffer on B; CopyTextureRegion(upload buffer B -> texB, PLACED_FOOTPRINT); run real FSR4 on GPU B.
   Return the output texture (resident on GPU B — see OPEN QUESTION about present path).
4. Double/triple buffer so GPU B upscales frame N while GPU A renders/ships frame N+1 (hide copy latency).

FFX API flow (from vendored SDK headers): game -> `ffxCreateContext` / `ffxDispatch`; resources passed via backend
callbacks; DX12 backend in `api_ffx_api_dx12.h`. AMD loader DLL is a ~26KB stub that loads the real upscaler/FG DLLs —
our proxy must match its export table (use tools/dump_exports.cpp to enumerate it).

## OPEN QUESTION (resolve early, affects design)
**Where does the upscaled OUTPUT get presented?** GPU B owns the displays but has no physical monitor in the usual
sense; GPU A renders. If the game presents via a swapchain on its own (GPU-A) device, our GPU-B output would need to
cross back B->A (a second RAM bounce). If it can present from GPU B directly (GPU B owns displays), the output stays
put — only inputs cross A->B. Investigate how the target game's swapchain/present is bound before finalizing transfer
design. This determines 1 vs 2 bounces per frame.

## REPO LAYOUT
- third_party/FidelityFX-SDK/   — AMD official SDK clone (GITIGNORED — not in git; it's a full repo itself).
  If missing, re-clone: `git clone --depth 1 https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK.git third_party/FidelityFX-SDK`.
  Signed FSR4 DLLs in Kits/FidelityFX/signedbin/: amd_fidelityfx_loader_dx12.dll, amd_fidelityfx_upscaler_dx12.dll,
  amd_fidelityfx_framegeneration_dx12.dll. Headers in Kits/FidelityFX/api/include/. HLSL FG source also present.
- third_party/dx-samples/       — Microsoft DirectX-Graphics-Samples clone (D3D12LinkedGpus = reference; uses affinity layer).
- tools/device_probe.cpp        — enumerate DXGI adapters, print LUIDs, create D3D12 device by LUID. WORKS.
- tools/xgpu_test.cpp           — original cross-adapter attempt. OBSOLETE (cross-adapter dead); keep for history.
- tools/xgpu_probe2.cpp         — isolates which cross-adapter heap configs work. Output build/probe2_out.txt.
- tools/xgpu_probe3.cpp         — **capability probe.** CrossNodeSharingTier=0 on both GPUs -> cross-adapter dead. WORKS.
- tools/xgpu_probe4.cpp         — **THE VALIDATED TRANSFER TEST.** RAM-bounce A->B round-trip, PASS + timing. WORKS.
- tools/xgpu_probe5..14.cpp     — isolation probes that found the driver quirks (buffer state matrix, Map range,
  copy-type isolation, PLACED_FOOTPRINT fix). Keep as reference for the quirks section above.
- tools/dump_exports.cpp        — list a DLL's exported functions by reading its PE from disk (LoadLibraryExA is
  unreliable here). WORKS. Output build/*_exports.txt.
- tools/ffx_bindtest.cpp        — **CORE DE-RISK.** Loads the REAL signed upscaler DLL, creates device by LUID, calls
  ffxCreateContext. PASS on both GPUs (build/ffx_bindtest.exe [A|B]). WORKS.
- tools/ffx_smoke.cpp           — **PROXY SMOKE TEST.** Simulates a game on GPU A, loads our proxy DLL, verifies the
  device swap (ACTIVE) / passthrough. PASS in both modes. Run from build/smoke/ layout. WORKS.
- src/proxy/upperscale_proxy.cpp + .def — **THE PROXY DLL** -> build/amd_fidelityfx_dx12.dll. 5 FFX exports, config
  (env/ini), logging, passthrough + active device-swap modes. BUILT + SMOKE-PASS. (.def kept for reference; link here
  rejects .def files with LNK1107 — exports come from __declspec(dllexport) instead.)
- docs/, tests/, build/         — docs / test harness / build artifacts + msvc_env.sh. build/smoke/ = working proxy test dir.

## BUILD COMMAND THAT WORKS (git-bash, MSVC 14.51 BuildTools)
```
cd C:/Users/mrmih/Playground/AI/upperscale
source build/msvc_env.sh
export MSYS_NO_PATHCONV=1
cl.exe /nologo /EHsc /O2 /TP tools/xgpu_probe4.cpp /Fe:build/xgpu_probe4.exe \
  "/link" "/LIBPATH:C:\Program Files (x86)\Windows Kits\10\Lib\10.0.26100.0\um\x64" d3d12.lib dxgi.lib user32.lib
./build/xgpu_probe4.exe > build/probe4_out.txt 2>&1; cat build/probe4_out.txt
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
- `D3D12CreateDevice(IUnknown* pAdapter, D3D_FEATURE_LEVEL, REFIID riid, void** ppDevice)` — 4 args; pass
  `__uuidof(ID3D12Device)` (NOT nullptr) as the IID. No `D3D12_CREATE_DEVICE_DESC` in this header.
- `IDXGIAffinityFactory` is NOT present in these headers (would need Agility SDK / newer Windows SDK).
If you keep hitting E_INVALIDARG on a call that "should" work, suspect an old-header signature mismatch and grep the local um/d3d12.h for the exact declaration.

## NEXT STEPS (in order)
1. [x] Validate cross-GPU transfer path — DONE (probe4 PASS, ~1.3ms one-way @512x288).
2. [x] De-risk: real signed FSR4 upscaler binds to a LUID device — DONE (ffx_bindtest PASS both GPUs).
3. [x] Build src/proxy/ DLL skeleton + export table + passthrough/active modes — DONE (smoke PASS both modes).
4. [ ] **WIRE ffxDispatch cross-GPU transfer (THE NEXT BUILD).** In ACTIVE mode, on each dispatch: read the game's
   input textures (color/depth/MV, resident on GPU A) via the validated RAM bounce (PLACED_FOOTPRINT copy -> readback
   buffer -> Map/memcpy -> upload texture on B), build a B-resident ffxDispatchDescUpscale with our own command list +
   fence on GPU B, call the real upscaler's dispatch, then return the output. The game's `output` resource is on GPU A —
   we must write FSR4's result back to it (second bounce B->A) OR present from B (see OPEN QUESTION). Validate parity vs a
   single-GPU run. This unblocks ACTIVE mode end-to-end.
5. [ ] **Resolve OPEN QUESTION** (output present path): does the game present from GPU A or B? Determines 1 vs 2 bounces/frame.
6. [ ] Add frame-generation path on GPU B + double/triple buffering to hide copy latency + end-to-end latency measurement.
7. [ ] Config tool: pick GPU B by LUID (list adapters like device_probe), CLI args, write the ini the proxy reads at load.
8. [ ] End-to-end test in a real FSR-enabled game; capture before/after frames; document added latency.
9. [ ] Docs (README build/run/troubleshoot), tests, push to https://github.com/radumihai1/upperscale.git

## STATUS SNAPSHOT (as of last update)
- Repo at C:\Users\mrmih\Playground\AI\upperscale, git branch main. Remote origin = github radumihai1/upperscale
  (authed via credential manager). Pushed to origin/main; verify with `git ls-remote origin main` vs local HEAD.
- FidelityFX SDK vendored (signed FSR4 DLLs present). dx-samples cloned.
- device_probe WORKS. xgpu_probe3 WORKS (cross-adapter = NOT_SUPPORTED, tier 0). **xgpu_probe4 PASS** (transfer validated).
- Cross-GPU transfer is a CPU RAM bounce (readback->memcpy->upload), ~1.3ms one-way @512x288. Driver quirks documented above.
- **ffx_bindtest PASS both GPUs** — real signed FSR4 v4.1.1 binds to LUID-selected device. Core architecture proven.
- **Proxy DLL BUILT + SMOKE-PASS (both PASSTHROUGH and ACTIVE modes).** build/amd_fidelityfx_dx12.dll exports the 5 FFX
  symbols; active mode swaps backend device to GPU B by LUID. ffxDispatch in active mode returns ERROR until task 4 lands.
- Active next task = **wire ffxDispatch cross-GPU transfer** (task 4 above). Everything before it is done and verified.

## DO / DON'T
- DO kill any background test processes after verifying (user is sensitive to leftover servers).
- DO keep this file current; it is the resume point. Update STATUS SNAPSHOT + NEXT STEPS as you go, then commit+push.
- DON'T vendor OptiScaler GPL code into our MIT/Apache project — use AMD's permissive FidelityFX SDK as reference + dependency only.
- DON'T leave GUI/test apps running on the user's desktop after verification.
- DON'T re-attempt cross-adapter shared heap placement — proven unsupported (CrossNodeSharingTier=0). Use the RAM bounce.

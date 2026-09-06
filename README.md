# upperscale

Run AMD FSR / FSR4 **upscaling and frame generation on a second GPU** while your game renders on the first — by replacing `amd_fidelityfx_dx12.dll` with a drop-in proxy that reroutes all FFX compute to a user-selected GPU.

The game keeps its normal FSR pipeline (that's what produces the depth + motion vectors). We intercept the FFX dispatch calls, bounce the real inputs across GPUs through system RAM, run the **real signed AMD FFX** on the target GPU, and copy the result back into the game's original output texture. No shader mods, no injection, no frame-level post-processing — the actual FSR4/FG math runs natively on your second card with correct motion data.

## Why this (vs the alternatives)

| Tool | What it does | Limitation |
|---|---|---|
| **OptiScaler** | Reroutes FFX to another GPU | Cannot transfer motion vectors cross-GPU → no real frame generation |
| **Loosless Scaling** | Applies filters over frames (frame-level post) | Not a real FFX reroute; no MV-based interpolation |
| **upperscale** | Intercepts the actual FFX dispatch calls and moves color/depth/MV | — both upscaling AND frame generation run natively on GPU B with correct motion data |

## How it works

```
game (GPU A, e.g. 7900 XTX)                 upperscale proxy                target GPU B (e.g. 9060 XT)
─────────────────────────────               ─────────────────               ───────────────────────────
ffxCreateContext(device=A) ───────────────► swap device A → B by LUID ────► FFX context bound to B
ffxDispatch(upscale/FG, inputs on A) ─────► 1. bounce each input A→B via RAM (readback→memcpy→upload)
                                              2. hand FFX our GPU-B command list; real FFX records compute
                                              3. execute on B's queue, wait fence
                                              4. read output back B→A into an upload ring slot
                                              5. record copy-back into the GAME's open command list
                                              6. restore the desc exactly as passed
game executes its CL ─────────────────────► (runs our recorded copy-back) ► game's output texture filled
```

- **Transfer path**: CPU RAM bounce only. Cross-adapter shared heaps are unsupported on this hardware class (`CrossNodeSharingTier=0`), so readback buffer → `Map`/`memcpy` → upload buffer is the way. Measured ~1.3 ms one-way @512×288, scaling to a few ms at 720p–1080p.
- **Async**: v1 is synchronous per dispatch (correct, ~4–13 ms/frame depending on effect + resolution). Pipelining to overlap the CPU hop with GPU work is tracked as task 6b.

## Requirements

- Windows 10/11, a game that uses `amd_fidelityfx_dx12.dll` with FSR enabled (Cyberpunk 2077 confirmed; most modern AMD-FSR titles).
- Two D3D12 GPUs. The target GPU is selected by LUID — it does **not** need to own the display, and Windows per-app GPU preference is bypassed entirely for the FFX work.
- MSVC BuildTools + Windows SDK (to build); nothing special at runtime beyond the game's existing AMD driver stack.

## Building

```bat
:: git-bash or cmd with VS dev environment; from repo root:
source build/msvc_env.sh          :: git-bash (sets cl/link/SDK paths)
export MSYS_NO_PATHCONV=1
cl /nologo /EHsc /O2 /TP src/proxy/upperscale_proxy.cpp src/proxy/upperscale_xgpu.cpp ^
   /Fe:build/amd_fidelityfx_dx12.dll "/link" /DLL d3d12.lib dxgi.lib user32.lib
cl /nologo /EHsc /O2 /TP tools/upperscale_config.cpp /Fe:build/upperscale_config.exe "/link" d3d12.lib dxgi.lib user32.lib
```

The AMD FidelityFX SDK is vendored under `third_party/FidelityFX-SDK/` (gitignored — re-clone if missing, see `docs/HANDOFF.md`). The signed effect DLLs (`amd_fidelityfx_upscaler_dx12.dll`, `amd_fidelityfx_framegeneration_dx12.dll`) ship in the SDK's `Kits/FidelityFX/signedbin/`.

## Using it (3 steps)

1. **Pick your FFX GPU** with the config tool:
   ```bat
   upperscale_config.exe list                 :: see adapters + LUIDs
   upperscale_config.exe set 0 --dir "C:\path\to\game"   :: write upperscale.ini there
   ```
2. **Drop the files** into the game folder (next to where `amd_fidelityfx_dx12.dll` normally lives):
   - `build/amd_fidelityfx_dx12.dll`  ← our proxy (replaces AMD's loader)
   - `upperscale_real_loader.dll`     ← a copy of AMD's real loader (`signedbin/amd_fidelityfx_loader_dx12.dll`)
   - the effect DLLs if the game doesn't ship them: `amd_fidelityfx_upscaler_dx12.dll`, `amd_fidelityfx_framegeneration_dx12.dll`
   - `upperscale.ini` (from step 1)
3. **Launch the game with FSR enabled.** Check `<game-dir>\upperscale.log`: you want `mode=ACTIVE luid={0,<your LUID>}` and, per frame, `ffxDispatch: intercepted type=... rc=0`.

### Config (ini or env vars)

`upperscale.ini` in the game's working directory (`[proxy]` section):
```ini
[proxy]
enable=1
gpu_luid_low=0x27214      ; low 32 bits of the target GPU LUID
gpu_luid_high=0x0         ; high 32 bits (usually 0)
log=2                     ; 0 off, 1 create/destroy, 2 verbose, 3 per-dispatch
```
Env vars are read first and then **overridden by the ini if present**: `UPPERSCALE_ENABLE`, `UPPERSCALE_GPU_LUID` (+`_HI`), `UPPERSCALE_REAL_LOADER`, `UPPERSCALE_LOG`.

## Testing (no game required)

From `build/smoke/` (contains proxy + real loader + effect DLLs):
```bat
:: upscale path — FSR4 on GPU B, output parity vs passive control
set UPPERSCALE_ENABLE=1& set UPPERSCALE_GPU_LUID=0x27214& set UPPERSCALE_LOG=3
ffx_dispatchtest.exe A out_active.bin        :: PASS = valid upscaled image

:: frame generation path — PREPARE_V2 + FRAMEGENERATION on GPU B
ffx_fgtest.exe out_fg.bin 5                  :: PASS = non-zero generated frame
```
`docs/HANDOFF.md` has the full test matrix, driver quirks hit on this hardware, and build gotchas.

## Troubleshooting

- **Log says `mode=PASSTHROUGH`** — ini not found in CWD or LUID wrong; run `upperscale_config.exe list`.
- **`ffxDispatch: intercepted ... rc=1`** — see the `[xgpu] ERROR:` lines above it in the log (input bounce / mirror creation failures are logged with details).
- **Game crashes on FFX dispatch** — make sure the game's FFX textures have `ALLOW_UNORDERED_ACCESS` (all real games do; if you're testing a custom harness, set it — see HANDOFF quirk #7).
- **First frame is slow (~1 s)** — expected: FFX compiles its shaders on first dispatch.

## Status

- ✅ Cross-GPU upscale (FSR4) — verified end-to-end with output parity vs single-GPU control
- ✅ Cross-GPU frame generation (PREPARE_V2 + FRAMEGENERATION, up to 4 outputs) — verified end-to-end
- ✅ Config tool (`upperscale_config.exe`) + ini/env config
- ⏳ Async pipelining of the RAM bounce (task 6b), per-frame latency stats (6c)
- ⏳ Real-game validation pass (Cyberpunk 2077)

## License

MIT/Apache dual. Vendors AMD's FidelityFX SDK (permissive license) as a dependency; does not include OptiScaler code.

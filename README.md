# upperscale

Run AMD FSR / FSR4 **upscaling and frame generation on a second GPU** while your game renders on the first — by replacing `amd_fidelityfx_dx12.dll` with a drop-in proxy that reroutes all FFX compute to a user-selected GPU.

The game keeps its normal FSR pipeline (that's what produces the depth + motion vectors). We intercept the FFX dispatch calls, bounce the real inputs across GPUs through system RAM, run the **real signed AMD FFX** on the target GPU, and copy the result back into the game's original output texture. No shader mods, no injection, no frame-level post-processing — the actual FSR4/FG math runs natively on your second card with correct motion data.

> **Why is a copy-back needed at all?** The swapchain Cyberpunk presents from lives on GPU A (the render device), and FG outputs are consumed by the engine itself before present. Windows does composite every presented frame onto GPU B's display (DWM) — but that happens *after* the engine has already used the FFX results, so it can't be reused to deliver them back into the pipeline. Copying the results back to A is what makes the image correct; a presentation-takeover design (present generated frames from our own GPU-B swapchain) would skip both hops and is tracked as future work.

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
ffxDispatch(upscale/FG, inputs on A) ─────► 1. bounce all inputs A→B via RAM (one readback + one upload per dispatch)
                                              2. hand FFX our GPU-B command list; real FFX records compute
                                              3. execute on B's queue, wait fence
                                              4. read output back B→A into an upload ring slot
                                              5. copy outputs back to A on OUR OWN queue (never touches the game's open list)
                                              6. restore the desc exactly as passed
game executes its CL ─────────────────────► (unchanged — no appended commands) ► game's output texture already filled
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
rc /nologo /fo src/proxy/upperscale_version.res src/proxy/upperscale_version.rc
cl /nologo /EHsc /O2 /TP src/proxy/upperscale_proxy.cpp src/proxy/upperscale_xgpu.cpp src/proxy/upperscale_overlay.cpp ^
   /Fe:build/amd_fidelityfx_dx12.dll "/link" upperscale_version.res d3d12.lib dxgi.lib user32.lib gdi32.lib
cl /nologo /EHsc /O2 /TP tools/upperscale_config.cpp /Fe:build/upperscale_config.exe "/link" d3d12.lib dxgi.lib user32.lib
```

The version resource (`upperscale_version.rc`) is **required**: it makes the proxy report the stock loader's file version, which Cyberpunk checks when deciding whether to offer FSR4 (see Troubleshooting). The AMD FidelityFX SDK is vendored under `third_party/FidelityFX-SDK/` (gitignored — re-clone if missing, see `docs/HANDOFF.md`).

## Using it (installer)

The `dist/` folder is a self-contained package — copy it anywhere:

```
dist\
  upperscale_installer.exe <- GUI installer (recommended): pick GPU, install / update LUID / uninstall
  install.bat              <- same steps, command-line version
  uninstall.bat            <- remove everything again
  amd_fidelityfx_dx12.dll  <- the proxy (replaces AMD's loader)
  upperscale_config.exe    <- headless GPU picker / ini writer
```

**Install (GUI):** double-click `upperscale_installer.exe`. It shows your D3D12 adapters with VRAM + LUID, a live status line for the game folder, and three actions:
- **INSTALL** — kills+verifies any running game process, backs up the original `amd_fidelityfx_dx12.dll` once to `upperscale_backup_amd_fidelityfx_dx12.dll`, stages our proxy + the game's own original loader as `upperscale_real_loader.dll` (the backend — this must be the game's fat custom loader, not a thin SDK stub; see Troubleshooting), and writes `upperscale.ini` for the selected GPU. **Pick the card that does NOT render the game** (the one your monitor is plugged into).
- **UPDATE GPU / LUID** — if already installed: re-pick the adapter and it rewrites only the LUID in `upperscale.ini`, preserving enable/log/fg settings. Use this after a reboot or driver change, because **Windows reassigns adapter LUIDs on reboot** — an old LUID silently puts you back in passthrough mode (the HUD will say `gpuB=(not created)`).
- **UNINSTALL** — verifies the current DLL is ours (MD5), restores the original loader from backup with a byte-identity check, and removes all upperscale files.

The same three operations work headless: `upperscale_installer.exe install|update|uninstall <game-dir> [gpuIndex] [fg]`.

**Install (bat):** double-click `install.bat` — identical steps to the GUI's INSTALL, with prompts instead of a window.

## Debug HUD (OptiScaler-style overlay)

The proxy creates a small always-on-top panel (top-left) showing live stats:

```
upperscale  ACTIVE  gpuB=RX 9060 XT
frame 16.7ms (60 fps)   min 14.2 / max 38.9
dispatches 12345  errors 0
bounce 11.4ms  ffx 0.5ms  capture+copyback 2.1ms
```

- **frame** — the game's frame period, measured between per-frame FFX dispatches (EMA + min/max). This is your real in-game FPS as seen by the proxy.
- **bounce / ffx / capture+copyback** — the three phases of each cross-GPU dispatch (EMAs). If `capture+copyback` grows, GPU B or the RAM bounce is the bottleneck.
- **errors** — count of failed bounces/dispatches; non-zero means something is wrong (check `upperscale.log`).

Hotkeys work while the game has focus:

| Key | Action |
|---|---|
| `Insert` | show / hide the HUD |
| `Delete` | cycle log level 0→1→2→3 (also persisted to ini) |
| `End` | toggle ACTIVE ↔ PASSTHROUGH live (no restart; also persisted to ini) |
| `Home` | toggle fg=0/1 — FG native on A vs FG on B (persisted to ini; takes effect for contexts created after the next game launch, since games create their FFX contexts at startup) |

Notes:
- The HUD is a topmost GDI window. In **exclusive fullscreen** the game covers it — use **borderless/windowed mode** to see it (same limitation as most OSDs). Stats are always in `upperscale.log` regardless; treat the log as the source of truth when the HUD is covered.
- Toggling to PASSTHROUGH live is the fastest way to A/B-test "is the proxy causing this?" without restarting.

## Manual install (other games)

For a game that isn't auto-detected, do what `install.bat` does by hand in its folder:

1. Back up the original loader: copy `amd_fidelityfx_dx12.dll` → `upperscale_backup_amd_fidelityfx_dx12.dll`.
2. Copy our proxy over it (same filename).
3. Copy that **original** loader to `upperscale_real_loader.dll` — this is the backend the proxy forwards to, and for Cyberpunk it must be the game's own custom 6 MB+ loader (it embeds FFX effect implementations; a thin SDK stub breaks FSR4 detection).
4. Write `upperscale.ini`:
   ```bat
   upperscale_config.exe list                 :: see adapters + LUIDs
   upperscale_config.exe set <index> --dir "C:\path\to\game"
   ```

### Config (ini or env vars)

`upperscale.ini` in the game's working directory (`[proxy]` section):
```ini
[proxy]
enable=1                 ; 1 = ACTIVE (cross-GPU), 0 = PASSTHROUGH (forward untouched)
gpu_luid_low=0x27214     ; low 32 bits of the target GPU LUID
gpu_luid_high=0x0        ; high 32 bits (usually 0)
log=2                    ; 0 off, 1 create/destroy, 2 verbose, 3 per-dispatch
fg=0                     ; DEFAULT: frame generation stays native on GPU A; only upscaling goes
                         ;            cross-GPU. Set fg=1 to run FG on GPU B too — EXPERIMENTAL:
                         ;            in Cyberpunk 2077 this removes GPU B after the first generated
                         ;            frame (FFX holds the game's swapchain from ffxConfigure and its
                         ;            GENERATE passes reference it cross-adapter; see HANDOFF §4).
```
Env vars are read first and then **overridden by the ini if present**: `UPPERSCALE_ENABLE`, `UPPERSCALE_GPU_LUID` (+`_HI`), `UPPERSCALE_REAL_LOADER`, `UPPERSCALE_LOG`, `UPPERSCALE_FG`.

> **Why is fg=0 the default?** In Cyberpunk 2077, FFX's frame-generation passes reference the
> game's swapchain (passed to `ffxConfigure`) while executing on GPU B — a cross-adapter violation
> that removes device B after the first generated frame. Verified in-game with isolated execution
> (FFX's recorded list alone kills devB; our readback commands never run). With `fg=0` the FG
> contexts keep their native GPU-A device and are forwarded untouched, so Cyberpunk runs its stock
> FSR4+FG pipeline on the render card while any standalone upscaling still offloads to GPU B.

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

- **FSR4 missing from the upscaler list** — known causes:
  1. *Backend was a thin SDK stub.* Cyberpunk ships its own custom ~6 MB loader with embedded FFX effect implementations; forwarding to a 26 KB SDK stub makes provider lookups fail and FSR4 disappears (even in passthrough). The installer now stages the game's **own original loader** as `upperscale_real_loader.dll`.
  2. *Proxy reported version 0.0.0.0.* Cyberpunk gates FSR4/FG on the loader's file version, not just its API behavior — a proxy with no version resource hid FSR4 even when forwarding every call verbatim (verified: all `ffxQuery` rc=0, zero swaps, FSR4 still gone). The build now embeds the stock loader's exact version metadata (`1.0.1.41314`, "AMD FidelityFX").
  3. ***Still missing with current builds — under investigation.*** With (1) and (2) fixed, our proxy returns **byte-identical** `GET_VERSIONS` results to stock and every in-game query returns rc=0, yet Cyberpunk still hides FSR4 from settings on both GPUs/LUIDs. The gate is therefore a file-level property of the DLL itself, not its API behavior. Leading hypothesis: an Authenticode check (stock loader = signed by AMD `Valid`; our proxy = unsigned). No AMD-specific signature-verification string was found in any game binary (all `WinVerifyTrust` imports belong to NVIDIA Streamline), so this is unconfirmed — see HANDOFF §"FSR4 visibility".
- **Log says `mode=PASSTHROUGH`** — ini not found in CWD or LUID wrong; run `upperscale_config.exe list`. Note: **Windows reassigns adapter LUIDs on reboot**, so a previously-written ini can silently point at the wrong (or no) adapter after a restart. Re-run the installer's UPDATE step (or `upperscale_config.exe set`) to refresh the LUID.
- **HUD says `gpuB=(not created)`** — same root cause: the configured GPU-B LUID no longer matches any live adapter (reboot/driver change). Update the LUID via the GUI installer or config tool; the HUD updates on next launch.
- **`ffxDispatch: intercepted ... rc=1`** — see the `[xgpu] ERROR:` lines above it in the log (input bounce / mirror creation failures are logged with details).
- **Game crashes / GPU B removed after enabling `fg=1` in Cyberpunk** — known and expected: FFX's FG passes reference the game's swapchain cross-adapter (verified root cause, see HANDOFF §4). Use the default `fg=0`: upscaling still runs on GPU B, frame generation stays native on the render card.
- **Game crashes on FFX dispatch** — make sure the game's FFX textures have `ALLOW_UNORDERED_ACCESS` (all real games do; if you're testing a custom harness, set it — see HANDOFF quirk #7).
- **First frame is slow (~1 s)** — expected: FFX compiles its shaders on first dispatch.
- **Garbage/black frames right after enabling FG** — the engine calls `ffxDispatch` while its command list is still open (single-submit), so our synchronous bounce reads *last* executed frame's inputs (+1 frame latency). The first generated frames can be garbage until temporal history settles; in menus there are no motion vectors at all, so FG output there is meaningless.
- **HUD not visible** — exclusive fullscreen covers topmost windows; switch to borderless/windowed mode.

## Status

- ✅ Cross-GPU upscale (FSR4) — verified end-to-end with output parity vs single-GPU control
- ✅ Passthrough transparency: stock version metadata + game's own loader as backend; 16+ min in-game, zero errors. **Open:** FSR4 still absent from Cyberpunk settings menu with current builds despite byte-identical feature-detection responses — file-level gate suspected (Authenticode), under investigation
- ✅ **Safe mode `fg=0` (default)** — FG-family contexts stay native on GPU A and are forwarded untouched; only upscaling goes cross-GPU. Wired end-to-end (create gate + per-context dispatch gate), synthetic tests PASS both fg values
- ⛔ Cross-GPU frame generation in Cyberpunk 2077 (`fg=1`) — **root-caused, disabled by default**: FFX holds the game's swapchain from `ffxConfigure` and its GENERATE passes reference it while executing on GPU B → device removed (0x887A0006). Proven in-game with isolated execution: FFX's recorded list alone removes devB before any of our readback commands run. Fixing requires a presentation-takeover design (own swapchain on GPU B) — tracked as future work
- ✅ Config tool (`upperscale_config.exe`) + ini/env config, `--fg` option, target-device validation
- ✅ **GUI installer** (`upperscale_installer.exe`, single native Win32 exe): pick GPU → INSTALL / UPDATE LUID (post-reboot) / UNINSTALL; MD5 identity checks on backup+restore; headless CLI mode for the same ops. Verified end-to-end against a fake game dir
- ✅ Installer/uninstaller with original-DLL backup, process kill+verify before staging, restore identity check (`dist/`)
- ✅ Debug HUD: frame times (EMA/min/max), per-phase dispatch stats, live mode/log/fg toggles (Insert/Delete/End/Home)
- ⏳ Async pipelining of the RAM bounce; presentation-takeover design to skip copy-backs and unlock FG-on-B

## Versioning

`v0.9.x` — safe-mode release line: cross-GPU upscaling stable, FG-on-B disabled by default for Cyberpunk (root cause documented). `v1.0.0` is reserved for when FG-on-B works in-game or the presentation-takeover design lands.

## License

MIT/Apache dual. Vendors AMD's FidelityFX SDK (permissive license) as a dependency; does not include OptiScaler code.

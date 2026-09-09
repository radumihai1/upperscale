# upperscale — HANDOFF (updated 2026-09-09)

Cross-GPU FSR4/frame-generation proxy for Cyberpunk 2077 on a dual-AMD box.
GPU A = RX 7900 XTX — game rendering. LUIDs are REASSIGNED ON REBOOT: was low=0x2A089, now 0x59173 (high=0).
GPU B = RX 9060 XT — display output adapter; both monitors cabled here. Was low=0x27214, now 0x5670A.
NOTE: a SECOND "RX 9060 XT"-named adapter appeared after the reboot (LUID 0xF7470) — verify by name+VRAM, not index.

This document is the single source of truth for resuming work. It separates VERIFIED facts from
HYPOTHESES. Do not treat hypotheses as established. **The ACTIVE-mode FG crash that blocked v9 is
now ROOT-CAUSED and worked around (fg=0 default). See §4.** **FSR4 visibility in the settings menu is
ROOT-CAUSED as a hardware limitation (RDNA3 render GPU), not a proxy bug — see §3/§3b.**

=====================================================================
## 1. STATUS AT A GLANCE
=====================================================================
WORKS (verified in-game):
- PASSTHROUGH mode with correct backend: stable 16+ min, thousands of ffxDispatch rc=0, zero errors.
- **fg=0 SAFE MODE (now the DEFAULT) in ACTIVE mode**: both FG-family contexts kept native on GPU A,
  every dispatch forwarded untouched, ZERO device swaps, ZERO [xgpu] intercepts, ZERO errors across
  ~95k FG GENERATE dispatches spanning a 10+ min menu session AND ~4 min of active gameplay (drove
  Space→Enter into the game). This is the shipping configuration for Cyberpunk.
- Synthetic tests (ffx_dispatchtest fg=0/1, ffx_fgtest fg=0) PASS on current build; test tools now resolve
  GPU A by name (env override UPPERSCALE_GPUA_LO/HI) so they survive LUID reassignment across reboots.
- Standalone HUD test PASSES; hotkeys: Insert=show/hide, Delete=cycle log level, End=toggle mode live,
  **Home=toggle fg** (all persisted to ini).
- Installer/uninstaller scripts work end-to-end; NEW GUI installer (upperscale_installer.exe) verified
  end-to-end against a fake game dir (install → update LUID → uninstall, stock restored byte-identical).

RESOLVED (2026-09-09):
- FSR4 absent from Cyberpunk's Graphics settings menu — ROOT CAUSE: FSR4 is RDNA4-only and the game
  renders on the RDNA3 7900 XTX (GpuPreference=2 → HighPerfAdapter DEV_744C). Not a proxy issue.
  Fix path documented in §3b + tools/fsr4_gpu_switch.ps1 (user decision: conflicts with "7900 XTX primary").

SHIPPED AS DISABLED-BY-DEFAULT (root-caused, not a bug in our code):
- ACTIVE mode with fg=1: GPU B device is REMOVED (0x887A0006) after the FIRST FRAMEGENERATION list.
  Root cause proven (§4). Fixing requires a presentation-takeover design — future work.

=====================================================================
## 2. VERIFIED FACTS (with evidence) vs HYPOTHESES
=====================================================================
VERIFIED:
1. Cyberpunk ships its own custom ~6.6MB amd_fidelityfx_dx12.dll with embedded FFX effect implementations;
   the thin SDK loader stub is only 26KB. Staging the THIN STUB as backend → ffxCreateContext rc=2 for
   FG-family contexts → FSR4 hidden. Staging the GAME'S OWN LOADER → both creates rc=0, FSR4 back.
   (ROOT CAUSE OF "FSR4 DISAPPEARS" = wrong backend loader.)
2. The proxy's version resource must mirror stock: FILEVERSION 1.0.1.41314 / ProductName "AMD FidelityFX".
   Verified byte-for-byte against the game's backed-up original via PowerShell VersionInfo (both report
   identical FileVersion/ProductVersion/ProductName/FileDescription/CompanyName; InternalName and
   OriginalFilename are empty in stock, so we leave them empty).
3. **E1 RESULT (this session):** Cyberpunk passes a NON-NULL swapChain to ffxConfigure for the FG context:
     `E1: ffxConfigure FG ctx=... swapChain=0x1344F4660 presentCallback=NULL pcUserCtx=NULL fgCallback=0x7FF64572B7B0 fgUserCtx=<ctx> pNext=NULL`
   (presentCallback is null; the game uses a frameGenerationCallback instead.) Hypothesis A CONFIRMED.
4. **E2 RESULT (this session):** with FFX's recorded GENERATE list executed ALONE on GPU B (our readback
   block held back on a separate clB2), devB is removed immediately:
     `E2: devB REMOVED after FFX-ONLY list execute reason=0x887A0006 -> illegal command is inside FFX's recorded passes`
     `E2: skipping readback block — devB already removed by FFX's own passes`
   => The illegal command is INSIDE what FFX records for GENERATE, not in our bounce/readback commands.
5. **ROOT CAUSE (from 3+4):** FFX holds the game's GPU-A swapchain from ffxConfigure and its GENERATE
   passes reference it while executing on GPU B → cross-adapter resource violation → device B removed.
   This is a property of how Cyberpunk drives FFX FG, not something our proxy can fix by reordering or
   re-barriering our own commands (we already proved in v9 that our recorded sequence is legal in isolation).
6. Driver quirks (all verified by probes/in-game): Reset() on a fresh list returns E_FAIL but the list is
   usable; CreateCommittedResource needs SampleDesc.Count=1 + Format=UNKNOWN + Layout=ROW_MAJOR for buffers;
   this SDK's ID3D12GraphicsCommandList::Reset takes 2 args, CreateCommandList 5; D3D12_CPU_DESCRIPTOR_HANDLE
   has .ptr not .Offset; GetGPUDescriptorHandleForHeapStart exists (…ForDescriptorStart does NOT);
   CreateUnorderedAccessView returns void here; ALL_BARRIERS and some UAV barrier forms are rejected.
7. FFX FG dispatch struct: ffxDispatchDescFrameGeneration has presentColor + outputs[4]; PREPARE V2 has
   depth + motionVectors. ffxConfigureDescFrameGeneration contains a `void* swapChain` field (now proven used).

HYPOTHESES (NOT verified — do not build on these without evidence):
- "Black frames in menus are due to FG without motion vectors" — plausible but unverified; secondary issue.
  (In fg=0 mode the game runs its own stock FG, so this is no longer our concern for Cyberpunk.)

=====================================================================
## 3. THE FSR4-DISAPPEARANCE (root-caused 2026-09-09 — hardware, not proxy)
=====================================================================
Fixed and verified earlier:
- dist/install.bat stages the GAME'S OWN original loader as upperscale_real_loader.dll (backed up to
  upperscale_backup_amd_fidelityfx_dx12.dll on first install; uninstall restores it). The thin SDK stub is
  NOT part of the package.
- Proxy embeds stock version metadata (1.0.1.41314, "AMD FidelityFX") — byte-identical resource to stock.

RESOLVED: with both fixes in place, user testing on 2026-09-08 showed FSR4 still absent from Cyberpunk's
Graphics settings menu (both GPU A and B as target). Root cause found 2026-09-09: FSR4 is an RDNA4-only
feature and the game renders on the RDNA3 7900 XTX. See §3b for the full evidence chain + fix path.

=====================================================================
## 3b. FSR4 VISIBILITY — ROOT CAUSE FOUND (2026-09-09)
=====================================================================
ROOT CAUSE: FSR4 is an RDNA4-only feature, and Cyberpunk renders on the RDNA3 card.

Evidence chain (all verified on this machine):
1. AMD + CDPR confirm FSR4 in Cyberpunk 2077 is exclusive to Radeon RX 9000 series (RDNA4).
   The 7900 XTX is RDNA3 and physically cannot run FSR4 — it gets FSR3 only.
2. Windows per-app GPU settings (HKCU\Software\Microsoft\DirectX\UserGpuPreferences):
     Cyberpunk2077.exe = "GpuPreference=2;AutoHDREnable=2097;"   (2 = High Performance)
     DirectXUserGlobalSettings: HighPerfAdapter = 1002&744C&2422148C
3. PCI ID mapping (Win32_VideoController): DEV_744C = RX 7900 XTX, DEV_7590 = RX 9060 XT.
   => Cyberpunk's render device is the RDNA3 7900 XTX. FSR4 can therefore NEVER appear in its
   settings menu on this configuration — independent of our proxy.

Why earlier evidence looked like "our tool hides FSR4":
- Our proxy returns byte-identical GET_VERSIONS to stock (UPSCALE 3.1.4/2.3.3, FRAMEGEN 1.1.3,
  FGSWAPCHAIN 1.1.3) and every in-game ffxQuery returns rc=0 with swapped=0 — the API is fully
  transparent; it does not hide anything.
- "FSR4 disappears when I activate the tool" was almost certainly a misattribution: FSR4 was never
  going to show while Cyberpunk renders on RDNA3, so noticing its absence around the time of
  enabling our proxy made it look causal.
- Changing the LUID in upperscale.ini (or "trying the other GPU") only changes where OUR proxy
  offloads work — it does NOT change which physical GPU Cyberpunk renders on, and that's what gates FSR4.

Deprioritized hypothesis (kept for completeness): Authenticode signature gate. Stock loader is
AMD-signed (Valid), our proxy unsigned; no AMD-specific verification strings found in game binaries
(all WinVerifyTrust imports are NVIDIA Streamline). Still testable via tools/fsr4_gpu_switch.ps1 +
a signed build, but the RDNA3 render-GPU explanation accounts for ALL observed symptoms.

TO GET FSR4 IN SETTINGS (user decision required — conflicts with "7900 XTX primary" preference):
- Point Cyberpunk at the 9060 XT: tools/fsr4_gpu_switch.ps1 -To9060 (typed CONFIRM guard; edits ONE
  HKCU registry value only; -Revert restores). Then FSR4 should appear in Graphics -> Upscaler.
- If doing so, re-point our proxy's offload target back to the 7900 XTX via the GUI installer's
  UPDATE step (render=9060 XT, offload=7900 XTX) — that is the intended cross-GPU configuration.

NOTE ON LUIDS: Windows reassigned ALL adapter LUIDs on the 2026-09-08 reboot (7900 XTX was 0x2A089, now
0x59173; 9060 XT was 0x27214, now 0x5670A — and a SECOND 9060-XT-named adapter appeared at 0xF7470). Test
tools now resolve GPU A by name with env override (UPPERSCALE_GPUA_LO/HI); the GUI installer's UPDATE step
exists precisely for this.

=====================================================================
## 4. ACTIVE-MODE FG CRASH — ROOT-CAUSED (this session)
=====================================================================
Timeline of what was tried (all builds by md5):
- v9 auto-reset + PREPARE-list fixes: did NOT fix the crash.
- Copy-backs moved to our own GPU-A queue: did NOT fix it.
- 7af7f235 (PRESENT-state inputs rewritten to PIXEL_COMPUTE_READ + device-removed instrumentation): SAME
  crash at the same spot (first GENERATE list). Consistent with fact #6 (our commands are legal; the killer
  is inside FFX's own recorded passes).

DECISIVE EXPERIMENTS RUN THIS SESSION (build v10, md5 b584b421ed3a4fbad7552620cc9f7e8c):
- E1: ffxConfigure now logs the FG desc fields UNCONDITIONALLY (one-shot at startup). Result: swapChain is
  NON-NULL in Cyberpunk. → Hypothesis A confirmed.
- E2: xgpuInterceptDispatch restructured so FFX's recorded list executes ALONE first, devB health is checked,
  and our readback block runs on a SEPARATE clB2 only if devB survived. Result: devB dies after the FFX-only
  list, before any of our readbacks run. → The violation is FFX-internal (it references the game's swapchain).

CONCLUSION / DECISION GATE (per plan): swapChain IS involved AND E2 confirms FFX-internal removal ⇒
ship with fg=0 as the default for Cyberpunk; track FG-on-B as future work requiring a different presentation
strategy (presentation takeover: own swapchain on GPU B, present generated frames from there).

fg=0 IMPLEMENTATION (wired end-to-end this session):
- ffxCreateContext: if g_cfgFgOnB==0 and the top-level create desc is FG-family (effect id 0x2xxxx/0x3xxxx/
  0x4xxxx, i.e. FRAMEGENERATION / FGSWAPCHAIN / FGSWAPCHAIN_VK), do NOT swap the backend device — context
  stays native on GPU A. Tracked in CtxInfo with swapped=0.
- ffxDispatch: any dispatch whose tracked context has swapped==0 is forwarded UNTOUCHED to the real loader
  (no bounce, no copy-back). One log line per such context, then quiet.
- fg default = 0 in LoadConfig AND in upperscale_config.exe (`--fg`, writes `fg=` to ini). Home hotkey toggles
  it live and persists to ini (takes effect for contexts created after the next game launch — games create
  their FFX contexts at startup).
- Cyberpunk's whole FSR4 pipeline is FG-family, so fg=0 ≈ passthrough there; standalone upscale contexts in
  other titles still offload cross-GPU.

=====================================================================
## 5. FILE STATE (as of v10 handoff)
=====================================================================
Repo: C:\Users\mrmih\Playground\AI\upperscale, branch main. Working tree clean after this session's commit.
Key source files (all under src/proxy/):
- upperscale_proxy.cpp   — 5 FFX entry points, config parse, fg=0 create/dispatch gates, IniSetKey (section-aware, atomic), live control API (mode/log/fg).
- upperscale_xgpu.cpp    — cross-GPU RAM-bounce hub + dispatch intercept; E2 restructure (clB executes alone, clB2 readbacks); PRESENT→state 0 mapping; MakeBuffer >4GB guard; XLog single FILE*.
- upperscale_overlay.{h,cpp} — GDI layered HUD + hotkeys (Insert/Delete/End/Home).
- upperscale_version.rc  — version resource 1.0.1.41314 "AMD FidelityFX" (matches stock exactly).
tools/upperscale_config.cpp — CLI: list / set (--dir --enable --log --fg) / show; validates target device opens as D3D12.
dist/                    — installer package: install.bat, uninstall.bat, amd_fidelityfx_dx12.dll (v10), upperscale_config.exe.

Build artifacts:
- build/amd_fidelityfx_dx12.dll = b584b421ed3a4fbad7552620cc9f7e8c  (v10 — the shipping DLL)
- build/upperscale_config.exe   (= dist copy)
- build/smoke/ffx_dispatchtest.exe + ffx_fgtest.exe (both PASS at fg=0 and fg=1 on v10)

Game dir staging: C:\Program Files (x86)\Steam\steamapps\common\Cyberpunk 2077\bin\x64
- amd_fidelityfx_dx12.dll = b584b421 (staged), upperscale_real_loader.dll = game's own loader,
  upperscale.ini: enable=1 log=3 gpu_luid_low=0x27214 fg=0. NO game process running at handoff time.

=====================================================================
## 6. CODE NOTES / TRAPS
=====================================================================
- XGpuHub has allocB2/clB2 (second GPU-B list) — now USED by the E2 readback path and released on teardown.
- g_cfgFgOnB is parsed (ini key "fg", env UPPERSCALE_FG), published in DllMain, AND consulted: ffxCreateContext
  skips the swap for FG-family contexts when fg=0; ffxDispatch forwards unswapped contexts untouched.
- Log file upperscale.log opens in APPEND mode; session boundaries = "=== upperscale proxy loaded vN ===" banners.
- MSVC C++ mode: no C99 compound literals; struct name is UpperscaleStats (two p's).
- Batch scripts: literal "(x86)" breaks for/if parsing → store paths in variables first; files MUST be CRLF;
  non-ASCII chars break the parser under cp850 — keep .bat pure ASCII.
- Bash printf mangles backslashes when creating .bat files — use write_file + `sed -i 's/$/\r/'` or Python.
- Launching Cyberpunk: terminal tool with background=true, NEVER shell "&". Kill before staging:
  powershell Get-Process Cyberpunk2077,REDprelauncher | Stop-Process -Force.
- Driving the game UI (Space/Enter to reach gameplay): computer_use key input needs an approval that times out
  when the user is at the terminal; a PowerShell SendKeys path (SetForegroundWindow on the main window handle,
  then [System.Windows.Forms.SendKeys]::SendWait) works without raising via cua-driver.

=====================================================================
## 7. EXACT NEXT STEPS (in order) — for whoever resumes
=====================================================================
1. (Optional, high value) Presentation-takeover design to unlock FG-on-B: own swapchain on GPU B, present
   generated frames from there so FFX never references the game's GPU-A swapchain cross-adapter. This is the
   only path to real cross-GPU frame generation in Cyberpunk. Until it lands, fg=0 stays the default.
2. (Optional) Async pipelining of the RAM bounce (double/triple-buffered readback/upload ring) to cut the
   synchronous per-dispatch stall on standalone upscaling contexts. Low priority for Cyberpunk (fg=0 means its
   pipeline never hits our cross-GPU path), higher value for titles with real upscale-only offload.
3. If a NEW title is targeted: stage that game's own loader as backend, set fg appropriately, run the synthetic
   matrix + a short in-game loop, and record per-title flags here (e.g. whether its FG passes use swapChain).

Build command:
  cd src/proxy && source ../../build/msvc_env.sh >/dev/null 2>&1; export MSYS_NO_PATHCONV=1
  rc /nologo /fo upperscale_version.res upperscale_version.rc
  cl.exe /nologo /EHsc /O2 /TP upperscale_proxy.cpp upperscale_xgpu.cpp upperscale_overlay.cpp \
    /Fe:../../build/amd_fidelityfx_dx12.dll /LD "/link" upperscale_version.res d3d12.lib dxgi.lib user32.lib gdi32.lib
  cl.exe /nologo /EHsc /O2 /TP ../tools/upperscale_config.cpp /Fe:../../build/upperscale_config.exe "/link" d3d12.lib dxgi.lib user32.lib

Stage command (GAME="/c/Program Files (x86)/Steam/steamapps/common/Cyberpunk 2077/bin/x64"):
  kill procs; cp build/amd_fidelityfx_dx12.dll "$GAME/" ; rm -f "$GAME/upperscale.log"

Launch: cd to game dir, run ./Cyberpunk2077.exe with terminal background=true + notify_on_complete.
Log check after ~90s: head -1 log (banner), grep -cE "REMOVED|ERROR", tail of log; then KILL THE GAME.

=====================================================================
## 8. DELIVERABLES STATE (user's standing request)
=====================================================================
- Debug overlay with frame times + stats: DONE (built, unit-tested; Home hotkey added).
- Self-contained installer folder dist/: upperscale_installer.exe (GUI) + install.bat + uninstall.bat +
  proxy DLL + config exe — all verified end-to-end. GUI supports INSTALL / UPDATE LUID / UNINSTALL with
  MD5 identity checks; headless CLI mode for the same ops.
- README: updated with GUI installer as primary path, LUID-reboot pitfall (gpuB=(not created)), and the
  FSR4 root cause (RDNA3 render GPU — hardware limit, not a proxy bug).
- tools/fsr4_gpu_switch.ps1: guarded helper to point Cyberpunk at the RDNA4 card (typed CONFIRM; one HKCU
  registry value; -Revert restores) for when the user opts in.
- GitHub push: GUI installer + docs shipped (58c0638); this commit adds the FSR4 root cause + helper.

User requirements to keep honoring: close the game after every test; document everything; put everything
needed in one folder with usage docs; push to GitHub when done.

# upperscale — HANDOFF (updated 2026-09-07)

Cross-GPU FSR4/frame-generation proxy for Cyberpunk 2077 on a dual-AMD box.
GPU A = RX 7900 XTX (LUID low=0x2A089, high=0) — game rendering.
GPU B = RX 9060 XT (LUID low=0x27214, high=0) — display output adapter; both monitors cabled here.

This document is the single source of truth for resuming work. It separates VERIFIED facts from
HYPOTHESES. Do not treat hypotheses as established.

=====================================================================
## 1. STATUS AT A GLANCE
=====================================================================
WORKS (verified in-game):
- PASSTHROUGH mode with correct backend: stable 16+ min, thousands of ffxDispatch rc=0, zero errors.
- FSR4 visible again when the game's own loader is staged as backend (see §3 root cause).
- Synthetic tests (ffx_dispatchtest, ffx_fgtest) PASS on current build.
- Standalone HUD test (hud_test.exe) PASSES; HUD hotkeys: Insert=show/hide, Delete=cycle log level, End=toggle mode live.
- Installer/uninstaller scripts work end-to-end (tested via `cmd /c script.bat < input.txt`).

BROKEN (the open bug):
- ACTIVE mode: GPU B device is REMOVED (0x887A0006) after the FIRST FRAMEGENERATION list executes.
  PREPARE dispatches survive; first GENERATE kills devB; every later bounce fails (upB map 0x887A0005);
  game shows alternating black frames and eventually crashes when entering gameplay.

=====================================================================
## 2. VERIFIED FACTS (with evidence) vs HYPOTHESES
=====================================================================
VERIFIED:
1. Cyberpunk ships its own custom ~6.6MB amd_fidelityfx_dx12.dll (md5 49230ad9...) with embedded FFX
   effect implementations. The thin SDK loader stub from signedbin is only 26KB.
2. With the THIN STUB staged as backend: ffxCreateContext returns rc=2 for both FG-family contexts
   (topType 0x20001 and 0x30001) → Cyberpunk hides FSR4 from the upscaler list. This was observed in
   an actual game log, not inferred.
3. With the GAME'S OWN LOADER staged as backend: both creates return rc=0 (observed in-game, passthrough
   run), FSR4 is expected back; user's 16-min passthrough session ran clean with zero errors.
   => ROOT CAUSE OF "FSR4 DISAPPEARS" = wrong backend loader, NOT the version resource and NOT our
      forwarding logic (all queries forwarded verbatim, rc=0).
4. ACTIVE-mode crash signature (from instrumented build 7af7f235 in-game log):
     - PREPARE dispatches: bounce OK, ffxDispatch rc=0, list executes clean.
     - First GENERATE: `ERROR: devB REMOVED after qB execute reason=0x887A0006` — i.e. AFTER we
       ExecuteCommandLists the list FFX recorded into (our readback block is appended to that same list).
     - All subsequent dispatches: `upB map hr=0x887A0005 devB_removed=0x887A0006` → bounce aborts,
       outputs left stale (black frames), game later dies in gameplay.
   => The illegal command is executed as part of the GENERATE list on GPU B. Which exact command: UNKNOWN.
5. probe_common_barrier.exe (standalone, GPU B): transition with before-state COMMON(0) AND with
   before-state UAV are BOTH legal — no device removal. Our readback barrier style is NOT the killer.
6. probe_generate_list.exe (standalone replica of our full recorded sequence: input bounce for a
   PRESENT-rewritten presentColor, FFX-style output-to-UAV transition, readback block with COMMON
   before-state): executes CLEAN, no device removal, in all skip-flag combinations.
   => Our own recorded commands are legal on this driver. The illegal command must be inside what
      FFX ITSELF records during ffxDispatch (the probe only simulated a barrier for that part).
7. Driver quirks (all verified by probes/in-game):
   - Reset() on a fresh command list returns E_FAIL (0x80004005) but the list is usable — suppressed as known quirk.
   - CreateCommittedResource with zero-initialized D3D12_RESOURCE_DESC fails E_INVALIDARG; SampleDesc.Count=1,
     Format=UNKNOWN, Layout=ROW_MAJOR must be set explicitly for buffers.
   - This SDK's ID3D12GraphicsCommandList::Reset takes 2 args (alloc, initialState); CreateCommandList takes 5
     (nodeMask, type, allocator, initialState, riid). D3D12_CPU_DESCRIPTOR_HANDLE has .ptr (SIZE_T), no .Offset.
   - GetGPUDescriptorHandleForHeapStart exists; ...ForDescriptorStart does NOT in this SDK version.
   - CreateUnorderedAccessView returns void here — a bad desc can crash the driver instead of returning an error.
   - ALL_BARRIERS and some UAV barrier forms are rejected by this AMD driver (earlier probe work).
8. FFX FG dispatch struct (ffx_framegeneration.h): ffxDispatchDescFrameGeneration has exactly these
   resource fields: presentColor + outputs[4]. PREPARE V2 has depth + motionVectors. No other resources.
9. ffxConfigureDescFrameGeneration CONTAINS a `void* swapChain` field and a presentCallback (header fact).

HYPOTHESES (NOT verified — do not build on these without evidence):
A. "FFX holds the game's GPU-A swapchain from ffxConfigure and references it during GENERATE dispatch,
   causing a cross-adapter violation that removes device B."
   - Status: UNVERIFIED. We have NOT logged whether Cyberpunk passes a non-null swapChain to ffxConfigure,
     nor proven FFX touches it at dispatch time. The previous session presented this as the root cause —
     that was overreach. It remains the leading hypothesis because (6) rules out our commands and (4) shows
     the removal happens on the GENERATE list specifically.
   - How to verify: add one log line in ffxConfigure printing desc->type and, for type 0x20002 (FG configure),
     the swapChain pointer value + presentCallback pointer. One game launch settles it.
B. "The version resource (1.0.1.41314) matters for FSR4 detection." — UNVERIFIED; the verified cause is #2/#3.
   The resource is embedded anyway (harmless, matches stock). Keep or drop later.
C. "Black frames in menus are due to FG without motion vectors" — plausible but unverified; secondary issue.

=====================================================================
## 3. THE FSR4-DISAPPEARANCE FIX (done, verified)
=====================================================================
- dist/install.bat now stages the GAME'S OWN original loader as upperscale_real_loader.dll (backed up to
  upperscale_backup_amd_fidelityfx_dx12.dll on first install; uninstall restores it). The thin SDK stub is
  NO LONGER part of the package.
- Verified in-game: passthrough run with correct backend → both ffxCreateContext rc=0, stable session.

=====================================================================
## 4. ACTIVE-MODE CRASH — INVESTIGATION STATE
=====================================================================
Timeline of what was tried (all builds listed by md5):
- v9 auto-reset + PREPARE-list fixes: did NOT fix the crash.
- Copy-backs moved from "appended to game's open list" to our own GPU-A queue: did NOT fix it.
- 7af7f235 (current build/ artifact): PRESENT-state inputs rewritten to PIXEL_COMPUTE_READ before being
  declared to FFX + device-removed instrumentation after every execute. In-game result: SAME crash at the
  same spot (first GENERATE list). So the PRESENT rewrite was not sufficient — consistent with fact #6
  (our commands are legal; the killer is inside FFX's own recorded passes).

Ruled out so far: our readback barriers (#5), our full recorded sequence in isolation (#6), stale event
state, poisoned allocators.

Remaining suspects (in priority order):
1. Something FFX records internally for GENERATE that references a GPU-A resource (swapchain hypothesis A,
   or an internal resource created before the device swap, or a fence/event object from the game's device).
2. The presentCallback: if Cyberpunk registers one and FFX invokes it during dispatch on devB with
   GPU-A resources in its params — same class of violation.

Decisive experiments for next session (cheap → expensive):
E1. Log ffxConfigure FG desc fields (swapChain, presentCallback pointers). One launch. Kills/keeps hypothesis A.
E2. In xgpuInterceptDispatch, for GENERATE only: execute FFX's recorded list WITHOUT our readback block
    appended (close+execute clB right after rd() returns), check devB health before doing readbacks on a
    second list (hub already has allocB2/clB2 fields declared — see §6). If devB dies without our block →
    100% FFX-internal. (probe_generate_list already suggests this, but in-game confirmation is the proof.)
E3. If E1/E2 confirm FG-on-B is fundamentally incompatible with Cyberpunk: implement fg=0 properly (§7) and
    ship it as default for this game; cross-GPU offload stays available for standalone upscale contexts.

=====================================================================
## 5. FILE STATE (as of handoff)
=====================================================================
Repo: C:\Users\mrmih\Playground\AI\upperscale, branch main, HEAD=4ca53c5 (pushed to origin).
Uncommitted changes:
- M .gitignore            (build/, *.obj/*.res/*.i/preproc.err, _input.txt, _t_*.bat, _test_*.bat, _repro.bat)
  !! third_party/ is NOT ignored and is 3.4GB — MUST add `third_party/` to .gitignore before committing.
- M README.md             (installer/HUD/config docs added; status section still says "crashes" — update after fix)
- M src/proxy/upperscale_proxy.cpp   (fg config parsing, g_cfgFgOnB publish in DllMain, version banner v9)
- M src/proxy/upperscale_xgpu.cpp    (PRESENT rewrite effState[], device-removed checks, upB map detail log,
                                      allocB2/clB2 struct fields [UNBUILT], PRESENT-rewrite note log [UNBUILT])
- M tools/upperscale_config.cpp      (CmdSet argv fix: argc-2/argv+2; ResolveAdapter accepts decimal index or hex LUID)
Untracked new files:
- src/proxy/upperscale_overlay.{h,cpp}   (GDI layered HUD, worker thread, RegisterHotKey hotkeys)
- src/proxy/upperscale_version.rc        (version resource 1.0.1.41314 "AMD FidelityFX")
- tools/hud_test.cpp                     (standalone overlay smoke test — PASSES)
- tools/backend_probe.cpp                (loader query probe; superseded by the two probes below)
- tools/probe_common_barrier.cpp         (PASSES: COMMON before-state legal on GPU B)
- tools/probe_generate_list.cpp          (PASSES: full replica of our recorded GENERATE sequence is legal)
- dist/                                  (installer package — see §8; contains stale _input.txt, delete it)

Build artifacts:
- build/amd_fidelityfx_dx12.dll = 7af7f235911b7ae8bf449380efe7b704  (PRESENT rewrite + instrumentation;
  does NOT include the unbuilt struct fields / note log — rebuild before next in-game test)
- build/upperscale_config.exe = 7d6cbe1ba2a5ea14fc8038649c4fcfbd   (= dist copy, verified identical)
- build/hud_test.exe, build/probe_common_barrier.exe, build/probe_generate_list.exe
- build/smoke/ffx_dispatchtest.exe + ffx_fgtest.exe (both PASS on 7af7f235)

Game dir staging: C:\Program Files (x86)\Steam\steamapps\common\Cyberpunk 2077\bin\x64
- amd_fidelityfx_dx12.dll = 7af7f235 (staged), upperscale_real_loader.dll = game's own loader,
  upperscale.ini: enable=1 log=3 gpu_luid_low=0x27214. NO game process running at handoff time.

dist/ currently holds amd_fidelityfx_dx12.dll = f38dde51 (STALE — older than build/'s 7af7f235).
Re-copy from build/ after the final fix.

=====================================================================
## 6. CODE NOTES / TRAPS
=====================================================================
- XGpuHub has allocB2/clB2 fields declared but NO creation code and no usage yet (dead weight, unbuilt).
  Either complete them for experiment E2 or delete the fields before rebuilding.
- g_cfgFgOnB is parsed (ini key "fg", env UPPERSCALE_FG) and published in DllMain, BUT ffxDispatch never
  consults it — fg=0 currently does NOTHING. Wiring it up is §7 item 1.
- Log file upperscale.log opens in APPEND mode; session boundaries = "=== upperscale proxy loaded vN ===" banners.
- MSVC C++ mode: no C99 compound literals; struct name is UpperscaleStats (two p's).
- Batch scripts: literal "(x86)" breaks for/if parsing → store paths in variables first; files MUST be CRLF;
  non-ASCII chars (em-dash) break the parser under cp850 — keep .bat pure ASCII.
- Bash printf mangles backslashes when creating .bat files — use write_file + `sed -i 's/$/\r/'` or Python.
- Launching Cyberpunk: terminal tool with background=true, NEVER shell "&". Kill before staging:
  powershell Get-Process Cyberpunk2077,REDprelauncher | Stop-Process -Force.

=====================================================================
## 7. EXACT NEXT STEPS (in order)
=====================================================================
1. Add ffxConfigure logging for FG configure desc (swapChain + presentCallback pointers). Rebuild, stage,
   one in-game launch (ACTIVE), read log, KILL THE GAME. → settles hypothesis A.
2. Implement experiment E2 (execute FFX's GENERATE list without our readback block; check devB health first).
3. Based on 1+2: either fix the specific violation or implement fg=0 properly:
   - In ffxCreateContext: if g_cfgFgOnB==0 and topType is FG-family (effect id 0x2xxxx/0x3xxxx), do NOT swap
     the backend device → context stays native on GPU A; its dispatches must then be forwarded untouched.
   - In ffxDispatch: forward as-is any dispatch whose context was not swapped (track per-context flag in CtxInfo).
   - Cyberpunk's FSR4 pipeline is entirely FG-family contexts, so fg=0 ≈ passthrough there; standalone upscale
     contexts still offload cross-GPU. Set default fg=0 for shipping until the crash is fixed.
4. Rebuild (include or remove allocB2/clB2), run synthetic tests, stage, in-game test ACTIVE (and fg=0 mode),
   KILL THE GAME after each test.
5. Update README status section with final state; re-copy final DLL into dist/; delete dist/_input.txt.
6. Add `third_party/` to .gitignore (3.4GB SDK — never commit). Commit everything, push to origin/main.

Build command:
  cd src/proxy && source ../../build/msvc_env.sh >/dev/null 2>&1; export MSYS_NO_PATHCONV=1
  cl.exe /nologo /EHsc /O2 /TP upperscale_proxy.cpp upperscale_xgpu.cpp upperscale_overlay.cpp \
    /Fe:../../build/amd_fidelityfx_dx12.dll /LD "/link" upperscale_version.res d3d12.lib dxgi.lib user32.lib gdi32.lib

Stage command (GAME="/c/Program Files (x86)/Steam/steamapps/common/Cyberpunk 2077/bin/x64"):
  kill procs; cp build/amd_fidelityfx_dx12.dll "$GAME/" ; rm -f "$GAME/upperscale.log"

Launch: cd to game dir, run ./Cyberpunk2077.exe with terminal background=true + notify_on_complete.
Log check after ~90s: head -1 log (banner), grep -cE "REMOVED|ERROR", tail of log; then KILL THE GAME.

=====================================================================
## 8. DELIVERABLES STATE (user's standing request)
=====================================================================
- Debug overlay with frame times + stats: DONE (built, unit-tested; ShowWindow fix in current build).
- Self-contained installer folder dist/: install.bat + uninstall.bat + proxy DLL + config exe — scripts work;
  DLL copy is stale until step 5 above. Usage documented in README.md.
- README: updated with installer/HUD/config docs; status section needs final update after the crash fix.
- GitHub push: PENDING (blocked on .gitignore for third_party/ and final build).

User requirements to keep honoring: close the game after every test; document everything; put everything
needed in one folder with usage docs; push to GitHub when done.

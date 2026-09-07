// upperscale_overlay.h — OptiScaler-style debug HUD + live control API.
//
// The proxy DLL creates a small always-on-top text panel (GDI, no D3D) showing FPS/frame-time
// and the cross-GPU transfer stats in real time. Global hotkeys let you toggle everything
// without leaving the game:
//   Insert  — show/hide the HUD
//   Delete  — cycle log level 0 -> 1 -> 2 -> 3 (persisted to upperscale.ini)
//   End     — toggle ACTIVE <-> PASSTHROUGH live (persisted to upperscale.ini)
//
// All stats are plain volatile fields written by the render thread and read by the HUD timer.
// Torn reads of a double are harmless for a debug display; counters use Interlocked ops.

#pragma once
#include <windows.h>

struct UpperscaleStats {
    // --- mode / config (written at load + on live toggle) ---
    volatile LONG   mode;          // 0 = passthrough, 1 = active
    volatile LONG   logLevel;      // current effective log level
    ULONG           luidLo, luidHi;// target GPU LUID from config
    char            gpuBName[64];  // resolved adapter name ("" until the device is created)

    // --- dispatch counters (render thread -> Interlocked) ---
    volatile LONG   dispatches;    // total intercepted ffxDispatch calls (active mode only)
    volatile LONG   errors;        // failed intercepts / non-OK return codes
    volatile LONG   lastRc;        // return code of the most recent intercepted dispatch

    // --- frame timing: interval between "frame anchor" dispatches (upscale / FG prepare) ---
    volatile double frameMsEma;    // steady-state per-frame period seen by FFX (ms)
    volatile double frameMsMax;
    volatile double frameMsMin;

    // --- per-phase cost of one intercepted dispatch (EMA, ms) ---
    volatile double msInputsEma;   // A->B input bounce (readback + memcpy + upload)
    volatile double msFfxRecordEma;// real FFX record time on GPU B
    volatile double msCaptureEma;  // output capture back to RAM + copy-back recording
    volatile double msTotalEma;    // end-to-end ffxDispatch cost (what the game thread pays)

    // --- hub state ---
    volatile LONG   mirrorCount;   // live B-side mirrors in the cache
};

// Defined in upperscale_proxy.cpp. Zero-initialized; safe to read at any time, from any thread.
extern UpperscaleStats g_upperscaleStats;

// Start the overlay worker thread (window + hotkeys). No-op if already started or disabled.
// Called once from DllMain after config load; the thread defers UI creation ~1.5 s so no Win32
// UI work happens while the loader lock is held.
void upperscaleOverlayStart(int enabled);

// Live control API (hotkeys + external tools). Each call updates in-memory state, mirrors it to
// g_upperscaleStats, and persists the change to upperscale.ini so it survives a restart.
int upperscaleSetMode(int enable);      // 1 = active, 0 = passthrough; returns new mode
int upperscaleSetLogLevel(int level);   // 0..3; returns new level

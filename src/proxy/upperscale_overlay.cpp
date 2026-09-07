// upperscale_overlay.cpp — OptiScaler-style debug HUD (layered GDI window) + global hotkeys.
//
// Design notes:
//   * The worker thread defers all Win32 UI work ~1.5 s after DllMain so nothing runs while the
//     loader lock is held (UI calls from a DLL attach are a classic deadlock source).
//   * Layered window + UpdateLayeredWindow gives real per-pixel alpha (the OptiScaler look) with
//     plain GDI text — no D3D, no swapchain, nothing that could interfere with the game.
//   * Hotkeys are global (RegisterHotKey) so they work while the game is fullscreen exclusive-ish;
//     if a game grabs the keyboard exclusively the keys simply won't arrive until alt-tab.

#include "upperscale_overlay.h"
#include <stdio.h>
#include <math.h>

static const wchar_t* kHudClass = L"upperscale_hud_v1";
static HWND  g_hud = nullptr;
static HFONT g_font = nullptr, g_fontBold = nullptr;
static bool  g_visible = true;

// ---- text panel rendering (offscreen DIB -> UpdateLayeredWindow) -----------------------------
static void RenderPanel(HWND hwnd, int w, int h) {
    HDC screen = GetDC(nullptr);
    HDC dcMem = CreateCompatibleDC(screen);
    BITMAPINFO dibInfo{};
    dibInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    dibInfo.bmiHeader.biWidth = w; dibInfo.bmiHeader.biHeight = h;
    dibInfo.bmiHeader.biPlanes = 1; dibInfo.bmiHeader.biBitCount = 32; dibInfo.bmiHeader.biCompression = BI_RGB;
    HBITMAP dib = CreateDIBSection(nullptr, &dibInfo, DIB_RGB_COLORS, nullptr, nullptr, 0);
    HGDIOBJ old = SelectObject(dcMem, dib);

    // Panel background: dark blue-grey at ~85% alpha (fill the DIB directly).
    void* bits = nullptr;
    if (GetDIBits(screen, dib, 0, h, &bits, &dibInfo, DIB_RGB_COLORS) == 0 || !bits) bits = nullptr;
    if (bits) {
        for (int y = 0; y < h; ++y) {
            DWORD* row = (DWORD*)((BYTE*)bits + (size_t)y * w * sizeof(DWORD));
            for (int x = 0; x < w; ++x) row[x] = 0xE8101018;   // ABGR: a=0xE8 b=0x18 g=0x10 r=0x10
        }
    }

    SetBkMode(dcMem, TRANSPARENT);
    SelectObject(dcMem, g_fontBold);
    SetTextColor(dcMem, RGB(235, 240, 255));
    TextOutA(dcMem, 10, 8, "upperscale", 10);

    UpperscaleStats* s = &g_upperscaleStats;
    int x = 118;
    if (s->mode == 1) { SetTextColor(dcMem, RGB(90, 235, 140)); TextOutA(dcMem, x, 8, "[ACTIVE]", 8); }
    else              { SetTextColor(dcMem, RGB(170, 175, 190)); TextOutA(dcMem, x, 8, "[PASSTHROUGH]", 13); }

    char line[256];
    int y = 30;
    SelectObject(dcMem, g_font);
    SetTextColor(dcMem, RGB(200, 205, 220));

    snprintf(line, sizeof(line), "GPU B: %s", s->gpuBName[0] ? s->gpuBName : "(not created yet)");
    TextOutA(dcMem, 10, y, line, (int)strlen(line)); y += 17;

    if (s->frameMsEma > 0.05) {
        snprintf(line, sizeof(line), "frame %.1f fps | %.2f ms (min %.2f / max %.2f)",
                 1000.0 / s->frameMsEma, s->frameMsEma, s->frameMsMin, s->frameMsMax);
    } else {
        snprintf(line, sizeof(line), "frame -- fps | waiting for FFX dispatches...");
    }
    TextOutA(dcMem, 10, y, line, (int)strlen(line)); y += 17;

    if (s->dispatches > 0) {
        snprintf(line, sizeof(line), "dispatches %ld | errors %ld | last rc=%d",
                 s->dispatches, s->errors, (int)s->lastRc);
        TextOutA(dcMem, 10, y, line, (int)strlen(line)); y += 17;

        snprintf(line, sizeof(line), "bounce A->B %.2f ms | ffx rec %.2f ms | cap B->A %.2f ms | total %.2f ms",
                 s->msInputsEma, s->msFfxRecordEma, s->msCaptureEma, s->msTotalEma);
        TextOutA(dcMem, 10, y, line, (int)strlen(line)); y += 17;

        snprintf(line, sizeof(line), "mirrors %ld | log level %d | fg=%s",
                 s->mirrorCount, (int)s->logLevel, s->fgOnB ? "1 (FG on B)" : "0 (FG native)");
        TextOutA(dcMem, 10, y, line, (int)strlen(line)); y += 19;
    } else {
        SetTextColor(dcMem, RGB(150, 155, 170));
        snprintf(line, sizeof(line), "no FFX dispatches yet — enable FSR in the game");
        TextOutA(dcMem, 10, y, line, (int)strlen(line)); y += 19;
    }

    SetTextColor(dcMem, RGB(120, 125, 145));
    const char* help = "[Ins] hide   [Del] log   [End] mode   [Home] fg";
    TextOutA(dcMem, 10, y, help, (int)strlen(help));

    POINT ptZero{0, 0};
    SIZE sz{(LONG)w, (LONG)h};
    BLENDFUNCTION bf{}; bf.BlendOp = AC_SRC_OVER; bf.SourceConstantAlpha = 255; bf.AlphaFormat = AC_SRC_ALPHA;
    UpdateLayeredWindow(hwnd, screen, &ptZero, &sz, dcMem, &ptZero, 0, &bf, ULW_ALPHA);

    SelectObject(dcMem, old);
    DeleteObject(dib);
    DeleteDC(dcMem);
    ReleaseDC(nullptr, screen);
}

static LRESULT CALLBACK HudProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
        case WM_TIMER:
            if (wp == 1 && g_visible) InvalidateRect(hwnd, nullptr, FALSE);
            return 0;
        case WM_PAINT: {
            PAINTSTRUCT ps; BeginPaint(hwnd, &ps); EndPaint(hwnd, &ps);   // layered: paint is a no-op
            RECT r; GetClientRect(hwnd, &r);
            RenderPanel(hwnd, r.right - r.left, r.bottom - r.top);
            return 0;
        }
        case WM_HOTKEY:
            if (wp == 1) {   // Insert — show/hide
                g_visible = !g_visible;
                ShowWindow(hwnd, g_visible ? SW_SHOWNOACTIVATE : SW_HIDE);
            } else if (wp == 2) {   // Delete — cycle log level
                upperscaleSetLogLevel(((int)g_upperscaleStats.logLevel + 1) % 4);
                InvalidateRect(hwnd, nullptr, FALSE);
            } else if (wp == 3) {   // End — toggle mode live
                upperscaleSetMode(g_upperscaleStats.mode ? 0 : 1);
                InvalidateRect(hwnd, nullptr, FALSE);
            } else if (wp == 4) {   // Home — toggle fg=0/1 (persisted; effective next launch)
                upperscaleSetFgOnB(g_upperscaleStats.fgOnB ? 0 : 1);
                InvalidateRect(hwnd, nullptr, FALSE);
            }
            return 0;
        case WM_DESTROY:
            UnregisterHotKey(hwnd, 1);
            UnregisterHotKey(hwnd, 2);
            UnregisterHotKey(hwnd, 3);
            UnregisterHotKey(hwnd, 4);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static DWORD WINAPI HudThread(LPVOID) {
    Sleep(1500);   // get out from under the loader lock before any UI work

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = HudProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = kHudClass;
    RegisterClassExW(&wc);

    g_font = CreateFontA(-12, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         FIXED_PITCH | FF_MODERN, "Consolas");
    g_fontBold = CreateFontA(-12, 0, 0, 0, FW_BOLD, FALSE, FALSE, FALSE, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                             FIXED_PITCH | FF_MODERN, "Consolas");

    const int W = 470, H = 142;
    g_hud = CreateWindowExW(WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_LAYERED, kHudClass, L"upperscale",
                            WS_POPUP, 24, 64, W, H, nullptr, nullptr, wc.hInstance, nullptr);
    if (!g_hud) return 0;

    ShowWindow(g_hud, g_visible ? SW_SHOWNOACTIVATE : SW_HIDE);   // WS_POPUP starts hidden — show it now

    RegisterHotKey(g_hud, 1, MOD_NOREPEAT, VK_INSERT);
    RegisterHotKey(g_hud, 2, MOD_NOREPEAT, VK_DELETE);
    RegisterHotKey(g_hud, 3, MOD_NOREPEAT, VK_END);
    RegisterHotKey(g_hud, 4, MOD_NOREPEAT, VK_HOME);
    SetTimer(g_hud, 1, 250, nullptr);

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0) > 0) { TranslateMessage(&m); DispatchMessageW(&m); }
    if (g_font) DeleteObject(g_font);
    if (g_fontBold) DeleteObject(g_fontBold);
    g_hud = nullptr;
    return 0;
}

void upperscaleOverlayStart(int enabled) {
    if (!enabled) return;
    static HANDLE hThread = nullptr;
    if (hThread) return;   // already started
    hThread = CreateThread(nullptr, 0, HudThread, nullptr, 0, nullptr);
    if (hThread) CloseHandle(hThread);
}

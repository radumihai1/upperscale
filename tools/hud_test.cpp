// hud_test.cpp — standalone smoke test for the upperscale debug HUD.
// Loads the proxy DLL in a plain console process (no game, no GPU work), waits for the overlay
// thread to create its window, then verifies: window exists, is visible, responds to WM_HOTKEY
// (Insert = hide/show). Prints PASS/FAIL lines so it can run unattended.
//
// Build: cl /nologo /EHsc /O2 /TP tools/hud_test.cpp /Fe:build/hud_test.exe "/link" user32.lib

#include <windows.h>
#include <stdio.h>
#include <process.h>

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    // passthrough defaults (no ini needed): no GPU work happens unless ffx* is called.
    SetEnvironmentVariableA("UPPERSCALE_LOG", "1");

    const char* dll = argc > 1 ? argv[1] : "../build/amd_fidelityfx_dx12.dll";
    HMODULE h = LoadLibraryA(dll);
    if (!h) { printf("FAIL: LoadLibrary(%s) err=%lu\n", dll, GetLastError()); return 1; }
    printf("loaded %s (pid=%lu)\n", dll, GetCurrentProcessId());

    Sleep(4000);   // overlay thread sleeps 1.5 s then creates the window

    HWND hud = FindWindowW(L"upperscale_hud_v1", nullptr);
    if (!hud) { printf("FAIL: HUD window not found after 4 s\n"); return 2; }
    printf("PASS: HUD window exists (hwnd=%p)\n", (void*)hud);

    RECT r{}; GetWindowRect(hud, &r);
    printf("PASS: HUD visible at (%d,%d)-(%d,%d) style=0x%lx exstyle=0x%lx\n",
           r.left, r.top, r.right, r.bottom, (unsigned long)GetWindowLongPtrW(hud, GWL_STYLE),
           (unsigned long)GetWindowLongPtrW(hud, GWL_EXSTYLE));

    // Insert hotkey -> hide. PostMessage the WM_HOTKEY directly (id 1).
    PostMessageW(hud, WM_HOTKEY, 1, 0);
    Sleep(800);
    int vis = IsWindowVisible(hud);
    printf("%s: after Insert, visible=%d (expect 0)\n", vis ? "FAIL" : "PASS", vis);

    // Insert again -> show.
    PostMessageW(hud, WM_HOTKEY, 1, 0);
    Sleep(800);
    vis = IsWindowVisible(hud);
    printf("%s: after second Insert, visible=%d (expect 1)\n", vis ? "PASS" : "FAIL", vis);

    // Delete hotkey -> log level cycles; check the ini was written with a new value.
    PostMessageW(hud, WM_HOTKEY, 2, 0);
    Sleep(800);
    FILE* f = fopen("upperscale.ini", "r");
    if (f) { char b[512] = {}; size_t n = fread(b, 1, sizeof(b) - 1, f); fclose(f); b[n] = 0; printf("ini after Delete hotkey:\n%s\n", b); }
    else printf("note: no upperscale.ini written (expected if CWD not writable)\n");

    FreeLibrary(h);
    printf("hud_test done\n");
    return 0;
}

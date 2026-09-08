// upperscale_installer.cpp — GUI installer for the upperscale cross-GPU FFX proxy.
//
// Single native Win32 exe (no dependencies). Mirrors install.bat / uninstall.bat exactly:
//   Install  : kill game procs -> back up original amd_fidelityfx_dx12.dll ONCE
//              (upperscale_backup_amd_fidelityfx_dx12.dll) -> copy the backup to
//              upperscale_real_loader.dll (the game's OWN fat loader is the backend;
//              a thin SDK stub breaks FSR4) -> replace original with our proxy ->
//              write upperscale.ini for the selected GPU.
//   Update   : already installed? pick a new GPU, rewrite the ini LUID only
//              (preserves enable/log/fg). Takes effect next launch.
//   Uninstall: verify current DLL is ours (MD5) -> restore original from backup with
//              byte-identity check -> remove upperscale.ini / .log / real-loader copy.
//
// Hidden CLI mode (used for headless testing):
//   upperscale_installer.exe install  <gamedir> <gpuIndex> [fg]
//   upperscale_installer.exe update   <gamedir> <gpuIndex>
//   upperscale_installer.exe uninstall <gamedir>
//
// Build: cl /nologo /EHsc /O2 /TP tools/upperscale_installer.cpp /Fe:build/upperscale_installer.exe \
//        "/link" d3d12.lib dxgi.lib user32.lib gdi32.lib comctl32.lib shell32.lib crypt32.lib

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>
#include <bcrypt.h>
#include <tlhelp32.h>
#include <strsafe.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <vector>
#include <string>

// ---------------------------------------------------------------- constants --
static const wchar_t* kProxyName   = L"amd_fidelityfx_dx12.dll";
static const wchar_t* kBackupName  = L"upperscale_backup_amd_fidelityfx_dx12.dll";
static const wchar_t* kRealLoader  = L"upperscale_real_loader.dll";
static const wchar_t* kIniName     = L"upperscale.ini";
static const wchar_t* kLogName     = L"upperscale.log";

// control ids
enum { IDC_GAMEEDIT=100, IDC_BROWSE, IDC_STATUS, IDC_GPU, IDC_REFRESH, IDC_FG,
       IDC_INSTALL, IDC_UPDATE, IDC_UNINSTALL, IDC_OPENDIR, IDC_LOG };

static const UINT WM_APP_LOG  = WM_APP + 1;   // wParam = log line count (see g_log)
static const UINT WM_APP_DONE = WM_APP + 2;   // wParam = 0 ok / 1 fail, lParam = op kind

// ------------------------------------------------------------------- globals --
struct AdapterInfo {
    ULONG luidHi, luidLo;
    wchar_t name[512];
    double vramMB;
    bool   d3d12ok;
};

static HINSTANCE g_hInst = nullptr;
static HWND      g_hwnd  = nullptr;
static char      g_selfDir[MAX_PATH]{};     // dir containing this exe (dist/)
static wchar_t   g_gameDir[MAX_PATH * 2]{}; // current game folder choice

// thread-safe log buffer shared with the worker thread
static CRITICAL_SECTION g_logCs;
static std::vector<std::string> g_log;      // needs <vector>/<string> — see below
static int g_logCount = 0;
static volatile LONG g_busy = 0;            // 1 while a worker op runs

// ------------------------------------------------------------------- helpers --
static void LogLine(const char* fmt, ...) {
    char buf[512];
    va_list ap; va_start(ap, fmt);
    _vsnprintf_s(buf, sizeof(buf), _TRUNCATE, fmt, ap);
    va_end(ap);
    if (!g_hwnd) printf("%s\n", buf);   // CLI mode: no window to post to — print directly
    EnterCriticalSection(&g_logCs);
    g_log.push_back(buf);
    int n = (int)g_log.size();
    LeaveCriticalSection(&g_logCs);
    if (g_hwnd && IsWindow(g_hwnd)) PostMessageW(g_hwnd, WM_APP_LOG, (WPARAM)n, 0);
}

// MD5 via CNG (BCrypt). The legacy CryptoAPI (CryptAcquireContext etc.) was REMOVED from
// crypt32.dll on this Windows 11 build, and the SDK's um/x64/crypt32.lib is an empty stub —
// so we use bcrypt.dll, which always exists and exports the full CNG hash API.
typedef LONG (WINAPI *pfnOpenAlg)(BCRYPT_ALG_HANDLE*, LPCWSTR, LPCWSTR, DWORD);
typedef LONG (WINAPI *pfnCreateHash)(BCRYPT_ALG_HANDLE, BCRYPT_HASH_HANDLE*, PUCHAR, ULONG, PUCHAR, ULONG, DWORD);
typedef LONG (WINAPI *pfnHashData)(BCRYPT_HASH_HANDLE, PUCHAR, ULONG, DWORD);
typedef LONG (WINAPI *pfnFinishHash)(BCRYPT_HASH_HANDLE, PUCHAR, ULONG, DWORD);
typedef LONG (WINAPI *pfnDestroyHash)(BCRYPT_HASH_HANDLE);
typedef LONG (WINAPI *pfnCloseAlg)(BCRYPT_ALG_HANDLE, DWORD);

static pfnOpenAlg    g_pBcOpen = nullptr;
static pfnCreateHash g_pBcCreate = nullptr;
static pfnHashData   g_pBcHash = nullptr;
static pfnFinishHash g_pBcFinish = nullptr;
static pfnDestroyHash g_pBcDestroy = nullptr;
static pfnCloseAlg   g_pBcClose = nullptr;

static bool InitCrypto() {
    if (g_pBcOpen) return true;
    HMODULE m = GetModuleHandleW(L"bcrypt.dll");   // always loaded on modern Windows
    if (!m) m = LoadLibraryW(L"bcrypt.dll");
    if (!m) return false;
    g_pBcOpen     = (pfnOpenAlg)(void*)GetProcAddress(m, "BCryptOpenAlgorithmProvider");
    g_pBcCreate   = (pfnCreateHash)(void*)GetProcAddress(m, "BCryptCreateHash");
    g_pBcHash     = (pfnHashData)(void*)GetProcAddress(m, "BCryptHashData");
    g_pBcFinish   = (pfnFinishHash)(void*)GetProcAddress(m, "BCryptFinishHash");
    g_pBcDestroy  = (pfnDestroyHash)(void*)GetProcAddress(m, "BCryptDestroyHash");
    g_pBcClose    = (pfnCloseAlg)(void*)GetProcAddress(m, "BCryptCloseAlgorithmProvider");
    return g_pBcOpen && g_pBcCreate && g_pBcHash && g_pBcFinish && g_pBcDestroy && g_pBcClose;
}

static bool FileMD5(const wchar_t* path, BYTE out[16]) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool ok = InitCrypto();
    BCRYPT_ALG_HANDLE alg = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (ok && g_pBcOpen(&alg, L"MD5", nullptr, 0) == 0) {
        // NULL object buffer = provider-allocated hash object. Passing our own buffer makes
        // BCryptCreateHash return STATUS_INVALID_PARAMETER on this build.
        if (g_pBcCreate(alg, &hash, nullptr, 0, nullptr, 0, 0) == 0) {
            ok = true;
            BYTE buf[65536]; DWORD rd;
            while (ok && ReadFile(h, buf, sizeof(buf), &rd, nullptr) && rd > 0)
                ok = g_pBcHash(hash, buf, rd, 0) == 0;
            if (ok) ok = g_pBcFinish(hash, out, 16, 0) == 0;
            g_pBcDestroy(hash);
        }
        g_pBcClose(alg, 0);
    }
    CloseHandle(h);
    return ok;
}

static void ToWide(const char* a, wchar_t* w, size_t nW) {
    int need = MultiByteToWideChar(CP_UTF8, 0, a, -1, nullptr, 0);
    if (need > (int)nW) need = (int)nW;
    MultiByteToWideChar(CP_UTF8, 0, a, -1, w, need);
}

static std::string ToNarrow(const wchar_t* w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return "";
    std::string s((size_t)n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

static bool MD5Equal(const wchar_t* a, const wchar_t* b) {
    BYTE ha[16], hb[16];
    if (!FileMD5(a, ha) || !FileMD5(b, hb)) return false;
    return memcmp(ha, hb, 16) == 0;
}

static void PathJoin(char* out, size_t n, const char* dir, const char* file) {
    snprintf(out, n, "%s\\%s", dir, file);
}
static void WPathJoin(wchar_t* out, size_t nW, const wchar_t* dir, const wchar_t* file) {
    StringCchCopyW(out, nW, dir);
    StringCchCatW(out, nW, L"\\");
    StringCchCatW(out, nW, file);
}

static int EnumerateAdapters(AdapterInfo* out, int maxOut) {
    IDXGIFactory1* factory = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return -1;
    int n = 0;
    for (UINT i = 0; ; ++i) {
        IDXGIAdapter1* adapter = nullptr;
        HRESULT hr = factory->EnumAdapters1(i, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND || FAILED(hr)) break;
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);
        ID3D12Device* dev = nullptr;
        bool ok = SUCCEEDED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev)));
        if (n < maxOut) {
            out[n].luidHi = desc.AdapterLuid.HighPart;
            out[n].luidLo = desc.AdapterLuid.LowPart;
            out[n].vramMB = (double)desc.DedicatedVideoMemory / 1048576.0;
            out[n].d3d12ok = ok;
            StringCchCopyW(out[n].name, 512, desc.Description);
        }
        if (dev) dev->Release();
        adapter->Release();
        n++;
    }
    factory->Release();
    return n;
}

// ------------------------------------------------------- game process control --
static bool GameProcessRunning() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"Cyberpunk2077.exe") == 0 ||
                _wcsicmp(pe.szExeFile, L"REDprelauncher.exe") == 0) found = true;
        } while (!found && Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

static void KillGameProcessesOnce() {
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"Cyberpunk2077.exe") == 0 ||
                _wcsicmp(pe.szExeFile, L"REDprelauncher.exe") == 0) {
                HANDLE p = OpenProcess(PROCESS_TERMINATE, FALSE, pe.th32ProcessID);
                if (p) { TerminateProcess(p, 1); CloseHandle(p); }
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
}

// Kill + verify they are gone (open handles would tear the DLL copy). Mirrors bat.
static bool StopGameAndWait() {
    if (!GameProcessRunning()) return true;
    LogLine("Stopping running Cyberpunk processes...");
    for (int i = 0; i < 5; ++i) {
        KillGameProcessesOnce();
        Sleep(2000);
        if (!GameProcessRunning()) return true;
    }
    return false;   // still running after 5 attempts
}

// ------------------------------------------------------------------- ini I/O --
struct IniVals { int enable = 1, logLevel = 2, fgOnB = 0; bool present = false; };

static IniVals ReadIni(const wchar_t* path) {
    IniVals v;
    FILE* f = _wfopen(path, L"r");
    if (!f) return v;
    char line[256];
    while (fgets(line, sizeof(line), f)) {
        // strip trailing newline/CR
        size_t len = strlen(line);
        while (len && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = 0;
        char key[64], val[192];
        if (sscanf(line, "%63[^=]=%191s", key, val) == 2) {
            while (*val && (val[strlen(val)-1]==' ' || val[strlen(val)-1]=='\t')) val[strlen(val)-1] = 0;
            if (!stricmp(key, "enable")) v.enable = atoi(val);
            else if (!stricmp(key, "log")) v.logLevel = atoi(val);
            else if (!stricmp(key, "fg")) v.fgOnB = atoi(val) != 0;
        }
    }
    fclose(f);
    v.present = true;
    return v;
}

static bool WriteIni(const wchar_t* path, const AdapterInfo& a, int enable, int logLevel, int fgOnB) {
    FILE* f = _wfopen(path, L"w");
    if (!f) return false;
    fprintf(f, "; upperscale proxy config - generated by upperscale_installer.exe\n");
    fprintf(f, "; The drop-in amd_fidelityfx_dx12.dll reads this from its working directory at load.\n");
    fprintf(f, "[proxy]\n");
    fprintf(f, "enable=%d\n", enable);
    fprintf(f, "gpu_luid_low=0x%lx\n", a.luidLo);
    fprintf(f, "gpu_luid_high=0x%lx\n", a.luidHi);
    fprintf(f, "log=%d\n", logLevel);
    fprintf(f, "fg=%d\n", fgOnB);   // 1 = FG also on GPU B (experimental), 0 = FG native on A (default)
    fclose(f);
    return true;
}

// ------------------------------------------------------------- install state --
enum InstallState { ST_NO_GAME=0, ST_STOCK, ST_INSTALLED, ST_PROXY_NO_BACKUP, ST_MODIFIED };

static InstallState DetectState(const wchar_t* gameDir, const char* selfProxy) {
    wchar_t cur[MAX_PATH * 2], bak[MAX_PATH * 2], oursW[MAX_PATH];
    WPathJoin(cur, MAX_PATH * 2, gameDir, kProxyName);
    WPathJoin(bak, MAX_PATH * 2, gameDir, kBackupName);
    ToWide(selfProxy, oursW, MAX_PATH);
    if (GetFileAttributesW(cur) == INVALID_FILE_ATTRIBUTES) return ST_NO_GAME;
    bool haveBak = GetFileAttributesW(bak) != INVALID_FILE_ATTRIBUTES;
    bool curIsOurs = MD5Equal(cur, oursW);
    if (haveBak && curIsOurs) return ST_INSTALLED;
    if (!haveBak && curIsOurs) return ST_PROXY_NO_BACKUP;
    if (haveBak && !curIsOurs) {
        // stock or something else — if it matches the backup, effectively stock+backup
        if (MD5Equal(cur, bak)) return ST_STOCK;   // backup present but original in place
        return ST_MODIFIED;
    }
    return ST_STOCK;
}

// ------------------------------------------------------------- core operations --
// Each returns true on success. `fg` only used by install. All file paths derived
// from gameDir (wide) + g_selfDir (narrow, for our proxy).
static bool OpInstall(const wchar_t* gameDir, const AdapterInfo& gpu, int fgOnB) {
    char selfProxy[MAX_PATH]; PathJoin(selfProxy, sizeof(selfProxy), g_selfDir, "amd_fidelityfx_dx12.dll");
    if (GetFileAttributesA(selfProxy) == INVALID_FILE_ATTRIBUTES) {
        LogLine("ERROR: %s not found next to this installer.", selfProxy);
        return false;
    }
    wchar_t oursW[MAX_PATH]; ToWide(selfProxy, oursW, MAX_PATH);
    wchar_t cur[MAX_PATH * 2], bak[MAX_PATH * 2];
    WPathJoin(cur, MAX_PATH * 2, gameDir, kProxyName);
    WPathJoin(bak, MAX_PATH * 2, gameDir, kBackupName);
    if (GetFileAttributesW(cur) == INVALID_FILE_ATTRIBUTES) {
        LogLine("ERROR: %s does not contain amd_fidelityfx_dx12.dll", gameDir);
        return false;
    }

    if (!StopGameAndWait()) {
        LogLine("ERROR: a Cyberpunk process is still running after 5 stop attempts — close it manually and retry.");
        return false;
    }

    // 1) back up the original loader exactly once (only if current is NOT ours)
    bool haveBak = GetFileAttributesW(bak) != INVALID_FILE_ATTRIBUTES;
    if (!haveBak) {
        if (MD5Equal(cur, oursW)) {
            LogLine("Current DLL is already the upperscale proxy — nothing to back up.");
        } else {
            if (!CopyFileW(cur, bak, FALSE)) {
                LogLine("ERROR: could not back up the original loader (err=%lu).", GetLastError());
                return false;
            }
            if (!MD5Equal(bak, cur)) {   // verify backup integrity before trusting it
                LogLine("ERROR: backup verification failed — aborting.");
                DeleteFileW(bak);
                return false;
            }
            LogLine("Backed up original loader -> %s", ToNarrow(bak).c_str());
        }
    } else {
        LogLine("Backup present: %s", ToNarrow(bak).c_str());
    }

    // 2) stage the game's OWN original loader as the FFX backend (critical for FSR4)
    if (GetFileAttributesW(bak) == INVALID_FILE_ATTRIBUTES) {
        LogLine("ERROR: no backup found and the original was already replaced. Run Steam 'verify integrity', then retry.");
        return false;
    }
    wchar_t real[MAX_PATH * 2]; WPathJoin(real, MAX_PATH * 2, gameDir, kRealLoader);
    if (!CopyFileW(bak, real, FALSE)) {   // bFailIfExists=FALSE => overwrite (copy /y)
        LogLine("ERROR: could not stage upperscale_real_loader.dll (err=%lu).", GetLastError());
        return false;
    }

    // 3) replace the original with our proxy + verify
    if (!CopyFileW(oursW, cur, FALSE)) {   // overwrite in place
        LogLine("ERROR: could not copy the proxy into the game folder (err=%lu — read-only?).", GetLastError());
        return false;
    }
    if (!MD5Equal(cur, oursW)) {
        LogLine("ERROR: staged proxy does not match our build — aborting.");
        return false;
    }
    LogLine("Staged proxy + game's own loader as upperscale_real_loader.dll into %s", ToNarrow(gameDir).c_str());

    // 4) write the ini for the selected GPU (preserve existing enable/log/fg if present)
    wchar_t ini[MAX_PATH * 2]; WPathJoin(ini, MAX_PATH * 2, gameDir, kIniName);
    IniVals v = ReadIni(ini);
    int fg = v.present ? v.fgOnB : fgOnB;   // checkbox only applies to a fresh install
    if (!WriteIni(ini, gpu, v.enable, v.logLevel, fg)) {
        LogLine("ERROR: could not write %s", ToNarrow(ini).c_str());
        return false;
    }
    LogLine("Wrote %s (GPU B LUID low=0x%lx high=0x%lx, fg=%d)", ToNarrow(ini).c_str(), gpu.luidLo, gpu.luidHi, fg);
    LogLine("Done. Launch the game with FSR enabled — HUD: [Ins] show/hide, [End] ACTIVE/PASSTHROUGH, [Home] toggle fg.");
    return true;
}

static bool OpUpdate(const wchar_t* gameDir, const AdapterInfo& gpu) {
    wchar_t cur[MAX_PATH * 2], bak[MAX_PATH * 2], ini[MAX_PATH * 2];
    WPathJoin(cur, MAX_PATH * 2, gameDir, kProxyName);
    WPathJoin(bak, MAX_PATH * 2, gameDir, kBackupName);
    WPathJoin(ini, MAX_PATH * 2, gameDir, kIniName);
    char selfProxy[MAX_PATH]; PathJoin(selfProxy, sizeof(selfProxy), g_selfDir, "amd_fidelityfx_dx12.dll");
    wchar_t oursW[MAX_PATH]; ToWide(selfProxy, oursW, MAX_PATH);

    bool installed = (GetFileAttributesW(bak) != INVALID_FILE_ATTRIBUTES) ||
                     (MD5Equal(cur, oursW));
    if (!installed) {
        LogLine("ERROR: not installed in this folder — run Install first.");
        return false;
    }
    IniVals v = ReadIni(ini);   // preserve enable/log/fg; only the LUID changes
    if (!WriteIni(ini, gpu, v.enable, v.logLevel, v.fgOnB)) {
        LogLine("ERROR: could not write %s", ToNarrow(ini).c_str());
        return false;
    }
    LogLine("Updated %s (GPU B LUID low=0x%lx high=0x%lx). Takes effect on next launch.", ToNarrow(ini).c_str(), gpu.luidLo, gpu.luidHi);
    return true;
}

static bool OpUninstall(const wchar_t* gameDir) {
    char selfProxy[MAX_PATH]; PathJoin(selfProxy, sizeof(selfProxy), g_selfDir, "amd_fidelityfx_dx12.dll");
    wchar_t oursW[MAX_PATH]; ToWide(selfProxy, oursW, MAX_PATH);
    wchar_t cur[MAX_PATH * 2], bak[MAX_PATH * 2];
    WPathJoin(cur, MAX_PATH * 2, gameDir, kProxyName);
    WPathJoin(bak, MAX_PATH * 2, gameDir, kBackupName);

    if (GetFileAttributesW(bak) == INVALID_FILE_ATTRIBUTES) {
        LogLine("ERROR: no %s in the game dir. Use Steam 'verify integrity of game files', or restore AMD's loader manually.", ToNarrow(kBackupName).c_str());
        return false;
    }
    if (!StopGameAndWait()) {
        LogLine("ERROR: a Cyberpunk process is still running after 5 stop attempts — close it manually and retry.");
        return false;
    }

    BYTE hb[16];
    if (!FileMD5(bak, hb)) { LogLine("ERROR: could not hash the backup file."); return false; }
    bool curIsOurs = MD5Equal(cur, oursW);
    bool curIsBak  = MD5Equal(cur, bak);

    if (curIsOurs) {
        if (!CopyFileW(bak, cur, FALSE)) { LogLine("ERROR: could not restore the original loader from backup."); return false; }
        BYTE hn[16];
        if (FileMD5(cur, hn) && memcmp(hn, hb, 16) == 0)
            LogLine("Restored original amd_fidelityfx_dx12.dll from backup — verified identical.");
        else
            LogLine("WARNING: restored file does NOT match the backup hash — run Steam verify integrity to be sure.");
    } else if (curIsBak) {
        LogLine("Current loader already matches the backup — nothing to restore.");
    } else {
        LogLine("WARNING: current amd_fidelityfx_dx12.dll is neither our proxy nor the backed-up original. Restoring backup anyway.");
        if (!CopyFileW(bak, cur, FALSE)) { LogLine("ERROR: could not restore the original loader from backup."); return false; }
    }

    wchar_t p[MAX_PATH * 2];
    WPathJoin(p, MAX_PATH * 2, gameDir, kIniName);       if (DeleteFileW(p)) LogLine("Removed upperscale.ini");
    WPathJoin(p, MAX_PATH * 2, gameDir, kLogName);       if (DeleteFileW(p)) LogLine("Removed upperscale.log");
    WPathJoin(p, MAX_PATH * 2, gameDir, kRealLoader);    if (DeleteFileW(p)) LogLine("Removed upperscale_real_loader.dll");

    LogLine("Done — game folder is back to stock.");
    return true;
}

// ------------------------------------------------------------- worker thread --
struct Op { int kind; /* 0 install, 1 update, 2 uninstall */ AdapterInfo gpu; int fg; };

static DWORD WINAPI WorkerThread(LPVOID pv) {
    Op* op = (Op*)pv;
    bool ok = false;
    switch (op->kind) {
        case 0: ok = OpInstall(g_gameDir, op->gpu, op->fg); break;
        case 1: ok = OpUpdate(g_gameDir, op->gpu);          break;
        case 2: ok = OpUninstall(g_gameDir);                break;
    }
    if (g_hwnd && IsWindow(g_hwnd)) PostMessageW(g_hwnd, WM_APP_DONE, ok ? 0 : 1, op->kind);
    delete op;
    return 0;
}

static bool StartOp(int kind, const AdapterInfo& gpu, int fg) {
    if (InterlockedCompareExchange(&g_busy, 1, 0) != 0) return false;
    Op* op = new Op();
    op->kind = kind; op->gpu = gpu; op->fg = fg;
    HANDLE h = CreateThread(nullptr, 0, WorkerThread, op, 0, nullptr);
    if (!h) { InterlockedExchange(&g_busy, 0); delete op; return false; }
    CloseHandle(h);
    EnableWindow(GetDlgItem(g_hwnd, IDC_INSTALL), FALSE);
    EnableWindow(GetDlgItem(g_hwnd, IDC_UPDATE), FALSE);
    EnableWindow(GetDlgItem(g_hwnd, IDC_UNINSTALL), FALSE);
    EnableWindow(GetDlgItem(g_hwnd, IDC_BROWSE), FALSE);
    return true;
}

// ------------------------------------------------------------------- UI bits --
static void AppendLogLine(const char* line) {
    HWND log = GetDlgItem(g_hwnd, IDC_LOG);
    SendMessageW(log, EM_SETSEL, (WPARAM)-1, 0);
    int n = MultiByteToWideChar(CP_UTF8, 0, line, -1, nullptr, 0);
    std::wstring w((size_t)n > 0 ? (size_t)n - 1 : 0, L'\0');
    if (!w.empty()) MultiByteToWideChar(CP_UTF8, 0, line, -1, &w[0], n);
    SendMessageW(log, EM_REPLACESEL, FALSE, (LPARAM)w.c_str());
    SendMessageW(log, EM_SCROLLCARET, 0, 0);
}

static void RefreshStatus() {
    char selfProxy[MAX_PATH]; PathJoin(selfProxy, sizeof(selfProxy), g_selfDir, "amd_fidelityfx_dx12.dll");
    InstallState st = DetectState(g_gameDir, selfProxy);
    const wchar_t* msg;
    switch (st) {
        case ST_NO_GAME:         msg = L"Game folder not found — pick one with Browse."; break;
        case ST_STOCK:           msg = L"Not installed (stock loader in place)."; break;
        case ST_INSTALLED:       msg = L"INSTALLED — proxy staged, original backed up."; break;
        case ST_PROXY_NO_BACKUP: msg = L"Proxy present but NO backup — uninstall needs Steam verify integrity first!"; break;
        case ST_MODIFIED:        msg = L"WARNING: current DLL is neither our proxy nor the backup."; break;
    }
    SetWindowTextW(GetDlgItem(g_hwnd, IDC_STATUS), msg);
    bool haveGame = (st != ST_NO_GAME);
    EnableWindow(GetDlgItem(g_hwnd, IDC_INSTALL),   haveGame && !g_busy);
    EnableWindow(GetDlgItem(g_hwnd, IDC_UPDATE),    (st == ST_INSTALLED || st == ST_PROXY_NO_BACKUP) && !g_busy);
    EnableWindow(GetDlgItem(g_hwnd, IDC_UNINSTALL), (st == ST_INSTALLED || st == ST_MODIFIED) && !g_busy);
}

static void RefreshGpuCombo() {
    HWND cb = GetDlgItem(g_hwnd, IDC_GPU);
    SendMessageW(cb, CB_RESETCONTENT, 0, 0);
    AdapterInfo a[32];
    int n = EnumerateAdapters(a, 32);
    if (n < 0) { SetWindowTextW(GetDlgItem(g_hwnd, IDC_STATUS), L"ERROR: could not enumerate GPU adapters."); return; }
    for (int i = 0; i < n; ++i) {
        wchar_t item[640];
        StringCchPrintfW(item, 640, L"[%d] %s — %.0f GB VRAM — LUID 0x%08X%s",
                         i, a[i].name, a[i].vramMB / 1024.0, (unsigned)a[i].luidLo,
                         a[i].d3d12ok ? L"" : L"  [NO D3D12]");
        SendMessageW(cb, CB_ADDSTRING, 0, (LPARAM)item);
    }
    // default selection: prefer the adapter that is NOT attached to any monitor? We can't
    // know which renders; keep index 0 unless a previous ini says otherwise.
    int sel = 0;
    wchar_t ini[MAX_PATH * 2]; WPathJoin(ini, MAX_PATH * 2, g_gameDir, kIniName);
    FILE* f = _wfopen(ini, L"r");
    if (f) {
        char line[256];
        while (fgets(line, sizeof(line), f)) {
            char key[64], val[192];
            if (sscanf(line, "%63[^=]=%191s", key, val) == 2 && !stricmp(key, "gpu_luid_low")) {
                unsigned long lo = strtoul(val, nullptr, 16);
                for (int i = 0; i < n; ++i) if (a[i].luidLo == lo) { sel = i; break; }
            }
        }
        fclose(f);
    }
    SendMessageW(cb, CB_SETCURSEL, sel, 0);
}

static void FindDefaultGameDir() {
    const wchar_t* cands[] = {
        L"C:\\Program Files (x86)\\Steam\\steamapps\\common\\Cyberpunk 2077\\bin\\x64",
        L"D:\\Steam\\steamapps\\common\\Cyberpunk 2077\\bin\\x64",
    };
    for (auto c : cands) {
        wchar_t probe[MAX_PATH * 2]; WPathJoin(probe, MAX_PATH * 2, c, kProxyName);
        if (GetFileAttributesW(probe) != INVALID_FILE_ATTRIBUTES) {
            StringCchCopyW(g_gameDir, MAX_PATH * 2, c);
            return;
        }
    }
}

static void BrowseForFolder() {
    BROWSEINFOW bi{};
    bi.hwndOwner = g_hwnd;
    bi.lpszTitle = L"Select the game folder containing amd_fidelityfx_dx12.dll (usually bin\\x64)";
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE;
    LPITEMIDLIST pidl = SHBrowseForFolderW(&bi);
    if (!pidl) return;
    wchar_t path[MAX_PATH * 2]{};
    if (SHGetPathFromIDListW(pidl, path)) {
        // auto-append bin\x64 if the user picked the game root
        wchar_t probe[MAX_PATH * 3]; WPathJoin(probe, MAX_PATH * 3, path, L"bin\\x64");
        wchar_t probeDll[MAX_PATH * 4]; WPathJoin(probeDll, MAX_PATH * 4, probe, kProxyName);
        if (GetFileAttributesW(probeDll) != INVALID_FILE_ATTRIBUTES) StringCchCopyW(g_gameDir, MAX_PATH * 2, probe);
        else StringCchCopyW(g_gameDir, MAX_PATH * 2, path);
        SetWindowTextW(GetDlgItem(g_hwnd, IDC_GAMEEDIT), g_gameDir);
    }
    CoTaskMemFree(pidl);
    RefreshStatus();
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        NONCLIENTMETRICSW ncm{}; ncm.cbSize = sizeof(ncm);
        SystemParametersInfoW(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0);
        HFONT font = CreateFontIndirectW(&ncm.lfMessageFont);

        auto mk = [&](const wchar_t* cls, const wchar_t* txt, DWORD style, int x, int y, int w, int h, int id) -> HWND {
            HWND c = CreateWindowExW(0, cls, txt, WS_CHILD | WS_VISIBLE | style, x, y, w, h, hwnd, (HMENU)(intptr_t)id, g_hInst, nullptr);
            if (font) SendMessageW(c, WM_SETFONT, (WPARAM)font, TRUE);
            return c;
        };

        mk(L"STATIC", L"Game folder:", 0, 12, 16, 84, 20, 0);
        mk(L"EDIT", g_gameDir, WS_BORDER | ES_AUTOHSCROLL, 100, 12, 470, 24, IDC_GAMEEDIT);
        mk(L"BUTTON", L"Browse...", BS_PUSHBUTTON, 580, 11, 90, 26, IDC_BROWSE);

        mk(L"STATIC", L"", SS_LEFTNOWORDWRAP, 12, 44, 658, 20, IDC_STATUS);
        SetWindowTextW(GetDlgItem(hwnd, IDC_STATUS), L"Detecting...");

        mk(L"STATIC", L"GPU that runs FFX (the one NOT rendering the game):", 0, 12, 74, 460, 20, 0);
        mk(L"COMBOBOX", L"", CBS_DROPDOWNLIST | WS_VSCROLL | WS_TABSTOP, 100, 70, 470, 300, IDC_GPU);
        mk(L"BUTTON", L"Refresh", BS_PUSHBUTTON, 580, 69, 90, 26, IDC_REFRESH);

        mk(L"BUTTON", L"Frame generation on GPU B (experimental — may crash some games)", BS_AUTOCHECKBOX, 100, 104, 470, 22, IDC_FG);
        SendMessageW(GetDlgItem(hwnd, IDC_FG), BM_SETCHECK, BST_UNCHECKED, 0);

        mk(L"BUTTON", L"Install", BS_PUSHBUTTON, 100, 138, 110, 32, IDC_INSTALL);
        mk(L"BUTTON", L"Update GPU selection", BS_PUSHBUTTON, 220, 138, 150, 32, IDC_UPDATE);
        mk(L"BUTTON", L"Uninstall", BS_PUSHBUTTON, 380, 138, 110, 32, IDC_UNINSTALL);
        mk(L"BUTTON", L"Open game folder", BS_PUSHBUTTON, 500, 138, 170, 32, IDC_OPENDIR);

        mk(L"STATIC", L"", WS_BORDER | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | WS_VSCROLL,
           12, 182, 658, 300, IDC_LOG);

        if (font) DeleteObject(font);
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == IDC_BROWSE) BrowseForFolder();
        else if (id == IDC_REFRESH) RefreshGpuCombo();
        else if (id == IDC_OPENDIR) ShellExecuteW(hwnd, L"explore", g_gameDir, nullptr, nullptr, SW_SHOWNORMAL);
        else if (id == IDC_INSTALL || id == IDC_UPDATE || id == IDC_UNINSTALL) {
            AdapterInfo a[32];
            int n = EnumerateAdapters(a, 32);
            HWND cb = GetDlgItem(hwnd, IDC_GPU);
            int sel = (int)SendMessageW(cb, CB_GETCURSEL, 0, 0);
            if (n < 0 || sel < 0 || sel >= n) { MessageBoxW(hwnd, L"Pick a GPU from the list first.", L"upperscale", MB_ICONWARNING); return 0; }
            int fg = SendMessageW(GetDlgItem(hwnd, IDC_FG), BM_GETCHECK, 0, 0) == BST_CHECKED ? 1 : 0;
            if (id == IDC_INSTALL && !StartOp(0, a[sel], fg)) MessageBoxW(hwnd, L"An operation is already running.", L"upperscale", MB_ICONWARNING);
            else if (id == IDC_UPDATE && !StartOp(1, a[sel], 0)) MessageBoxW(hwnd, L"An operation is already running.", L"upperscale", MB_ICONWARNING);
            else if (id == IDC_UNINSTALL) {
                // pre-check: warn before touching a MODIFIED folder from the worker thread
                char selfProxy[MAX_PATH]; PathJoin(selfProxy, sizeof(selfProxy), g_selfDir, "amd_fidelityfx_dx12.dll");
                wchar_t oursW[MAX_PATH]; ToWide(selfProxy, oursW, MAX_PATH);
                wchar_t cur[MAX_PATH * 2], bak[MAX_PATH * 2];
                WPathJoin(cur, MAX_PATH * 2, g_gameDir, kProxyName);
                WPathJoin(bak, MAX_PATH * 2, g_gameDir, kBackupName);
                if (GetFileAttributesW(bak) != INVALID_FILE_ATTRIBUTES && !MD5Equal(cur, oursW) && !MD5Equal(cur, bak)) {
                    int r = MessageBoxW(hwnd, L"The current amd_fidelityfx_dx12.dll is neither our proxy nor the backed-up original.\n\nRestore the backed-up original anyway?", L"upperscale", MB_YESNO | MB_ICONWARNING);
                    if (r != IDYES) return 0;
                }
                if (!StartOp(2, a[sel], 0)) MessageBoxW(hwnd, L"An operation is already running.", L"upperscale", MB_ICONWARNING);
            }
        }
        return 0;
    }
    case WM_APP_LOG: {
        EnterCriticalSection(&g_logCs);
        std::string line = (wp > 0 && wp <= g_log.size()) ? g_log[wp - 1] : std::string();
        LeaveCriticalSection(&g_logCs);
        if (!line.empty()) AppendLogLine(line.c_str());
        return 0;
    }
    case WM_APP_DONE: {
        InterlockedExchange(&g_busy, 0);
        EnableWindow(GetDlgItem(hwnd, IDC_BROWSE), TRUE);
        RefreshStatus();   // re-enables action buttons per state
        if (wp != 0) LogLine("Operation FAILED — see messages above.");
        return 0;
    }
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

// ------------------------------------------------------------------ CLI mode --
static int CliRun(int argc, LPWSTR* argv) {
    // Find the action token wherever it sits: some launchers (PowerShell &) pass a command
    // line WITHOUT the exe path, so argv[0] may already be "install".
    int act = -1;
    for (int i = 0; i < argc && i < 4; ++i) {
        if (_wcsicmp(argv[i], L"install") == 0 || _wcsicmp(argv[i], L"update") == 0 ||
            _wcsicmp(argv[i], L"uninstall") == 0) { act = i; break; }
    }
    if (act < 0) { printf("usage: upperscale_installer.exe install|update|uninstall <gamedir> [gpuIndex] [fg]\n"); return 2; }
    int kind = _wcsicmp(argv[act], L"install") == 0 ? 0 : _wcsicmp(argv[act], L"update") == 0 ? 1 : 2;

    // gamedir is the token right after the action (already wide)
    if (act + 1 >= argc) { printf("missing <gamedir>\n"); return 2; }
    StringCchCopyW(g_gameDir, MAX_PATH * 2, argv[act + 1]);

    AdapterInfo a[32]; int n = EnumerateAdapters(a, 32);
    if (n < 0) { printf("FAIL: adapter enumeration failed\n"); return 1; }
    int sel = 0, fg = 0;
    if (kind != 2 && act + 2 < argc) sel = _wtoi(argv[act + 2]);
    if (kind == 0 && act + 3 < argc) fg = _wtoi(argv[act + 3]) != 0;
    if (sel < 0 || sel >= n) { printf("FAIL: bad gpu index %d (have %d)\n", sel, n); return 1; }

    bool ok = false;
    switch (kind) {
        case 0: ok = OpInstall(g_gameDir, a[sel], fg); break;
        case 1: ok = OpUpdate(g_gameDir, a[sel]);      break;
        case 2: ok = OpUninstall(g_gameDir);           break;
    }
    printf(ok ? "OK\n" : "FAILED\n");
    return ok ? 0 : 1;
}

// -------------------------------------------------------------------- main ----
int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR cmdLine, int) {
    g_hInst = hInst;
    InitializeCriticalSection(&g_logCs);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);

    // self dir (dist/) — needed to find our proxy DLL
    wchar_t mod[MAX_PATH]; GetModuleFileNameW((HINSTANCE)g_hInst, mod, MAX_PATH);
    wchar_t* slash = wcsrchr(mod, L'\\'); if (slash) *slash = 0;
    char selfA[MAX_PATH]; WideCharToMultiByte(CP_UTF8, 0, mod, -1, selfA, sizeof(selfA), nullptr, nullptr);
    strncpy_s(g_selfDir, sizeof(g_selfDir), selfA, _TRUNCATE);

    // CLI mode?
    int argc = 0; LPWSTR* argv = CommandLineToArgvW(cmdLine, &argc);
    if (argv && argc > 1) {
        int rc = CliRun(argc, argv);
        LocalFree(argv);
        return rc;
    }

    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_TREEVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);

    FindDefaultGameDir();

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"UpperscaleInstaller";
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    RECT r{0, 0, 682, 500};
    AdjustWindowRectEx(&r, WS_OVERLAPPEDWINDOW | WS_MINIMIZEBOX, FALSE, 0);
    g_hwnd = CreateWindowExW(0, L"UpperscaleInstaller", L"upperscale installer — cross-GPU FSR4 proxy",
                              WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, r.right - r.left, r.bottom - r.top,
                              nullptr, nullptr, hInst, nullptr);
    ShowWindow(g_hwnd, SW_SHOW);
    UpdateWindow(g_hwnd);
    RefreshGpuCombo();   // now that the combo exists
    RefreshStatus();

    MSG m;
    while (GetMessageW(&m, nullptr, 0, 0)) { TranslateMessage(&m); DispatchMessageW(&m); }
    DeleteCriticalSection(&g_logCs);
    return 0;
}

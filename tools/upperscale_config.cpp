// upperscale_config.cpp — CLI helper for the upperscale proxy.
//
// Lists the D3D12-capable GPU adapters (name, LUID, VRAM) and writes an upperscale.ini
// that the drop-in amd_fidelityfx_dx12.dll reads at load time. This is how a user picks
// which GPU runs FFX without hand-editing env vars or the ini.
//
// Usage:
//   upperscale_config.exe list
//       Enumerate adapters with their LUIDs (the value to pass to `set`).
//
//   upperscale_config.exe set <adapter-index | 0xLUID> [--dir <path>] [--enable 1|0] [--log N]
//       Write upperscale.ini selecting that GPU as the FFX target. --dir defaults to CWD.
//       The ini is written next to where you'll drop the proxy DLL (usually the game folder).
//
//   upperscale_config.exe show [--dir <path>]
//       Print the current upperscale.ini in --dir (CWD by default), or a note if absent.
//
// Build: cl /nologo /EHsc /O2 /TP tools/upperscale_config.cpp /Fe:build/upperscale_config.exe "/link" d3d12.lib dxgi.lib user32.lib

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <direct.h>   // _getcwd

struct AdapterInfo {
    ULONG luidHi, luidLo;
    char  name[512];
    double vramMB;
    bool   d3d12ok;
};

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
        HRESULT dhr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev));
        bool ok = SUCCEEDED(dhr);

        if (n < maxOut) {
            out[n].luidHi = desc.AdapterLuid.HighPart;
            out[n].luidLo = desc.AdapterLuid.LowPart;
            out[n].vramMB = (double)desc.DedicatedVideoMemory / 1048576.0;
            out[n].d3d12ok = ok;
            char nameA[512]{};
            WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, nameA, sizeof(nameA), nullptr, nullptr);
            strncpy_s(out[n].name, nameA, _TRUNCATE);
        }
        if (dev) dev->Release();
        adapter->Release();
        n++;
    }
    factory->Release();
    return n;
}

static void PrintAdapters(const AdapterInfo* a, int n) {
    printf("D3D12-capable GPU adapters:\n");
    for (int i = 0; i < n; ++i) {
        unsigned long long luid = ((unsigned long long)a[i].luidHi << 32) | a[i].luidLo;
        printf("  [%d] LUID=0x%016llX  d3d12=%s  VRAM=%.0fMB\n", i, luid, a[i].d3d12ok ? "yes" : "NO ", a[i].vramMB);
        printf("      %s\n", a[i].name);
    }
}

static int CmdList() {
    AdapterInfo a[32];
    int n = EnumerateAdapters(a, 32);
    if (n < 0) { printf("FAIL: CreateDXGIFactory1 failed\n"); return 1; }
    PrintAdapters(a, n);
    printf("\nPick the GPU that should RUN FFX (the one NOT rendering the game).\n");
    printf("Then: upperscale_config.exe set <index-or-LUID> --dir <game-folder>\n");
    return 0;
}

static int ResolveAdapter(const AdapterInfo* a, int n, const char* arg) {
    // Accept "3" (index), "0x..." hex LUID, or a bare decimal that is NOT a valid index.
    if (arg[0] == '0' && (arg[1] == 'x' || arg[1] == 'X')) {
        unsigned long long want = strtoull(arg, nullptr, 16);
        for (int i = 0; i < n; ++i) {
            unsigned long long luid = ((unsigned long long)a[i].luidHi << 32) | a[i].luidLo;
            if (luid == want || (want & 0xFFFFFFFFull) == a[i].luidLo && a[i].luidHi == 0) return i;
        }
        return -1;
    }
    // Decimal index takes PRIORITY: "1" means adapter [1], never an LUID. (Old bug: the hex
    // fallback below matched luidLo=0x27214 against input "1" and silently picked adapter 0.)
    char* end = nullptr;
    long idx = strtol(arg, &end, 10);
    if (end && *end == 0) {
        if (idx >= 0 && idx < n) return (int)idx;
        // decimal but out of range — try it as a bare LUID before giving up
        unsigned long long want = (unsigned long long)idx;
        for (int i = 0; i < n; ++i) {
            unsigned long long luid = ((unsigned long long)a[i].luidHi << 32) | a[i].luidLo;
            if (luid == want || (want & 0xFFFFFFFFull) == a[i].luidLo && a[i].luidHi == 0) return i;
        }
        return -1;
    }
    // Not pure decimal — treat as bare hex LUID.
    unsigned long long want = strtoull(arg, nullptr, 16);
    for (int i = 0; i < n; ++i) {
        unsigned long long luid = ((unsigned long long)a[i].luidHi << 32) | a[i].luidLo;
        if (luid == want || (want & 0xFFFFFFFFull) == a[i].luidLo && a[i].luidHi == 0) return i;
    }
    return -1;
}

static int CmdSet(int argc, char** argv) {
    const char* sel = nullptr;
    char dir[MAX_PATH]{};
    _getcwd(dir, MAX_PATH);
    int enable = 1, logLevel = 2, fgOnB = 0;   // fg default 0 (safe mode): FG stays native on GPU A

    for (int i = 0; i < argc; ++i) {
        if (!strcmp(argv[i], "--dir") && i + 1 < argc) strncpy_s(dir, argv[++i], _TRUNCATE);
        else if (!strcmp(argv[i], "--enable") && i + 1 < argc) enable = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--log") && i + 1 < argc) logLevel = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--fg") && i + 1 < argc) fgOnB = atoi(argv[++i]) != 0;
        else if (sel == nullptr) sel = argv[i];
    }
    if (!sel) { printf("usage: upperscale_config.exe set <index|0xLUID> [--dir path] [--enable 1|0] [--log N] [--fg 1|0]\n"); return 2; }

    AdapterInfo a[32];
    int n = EnumerateAdapters(a, 32);
    if (n < 0) { printf("FAIL: CreateDXGIFactory1 failed\n"); return 1; }
    int idx = ResolveAdapter(a, n, sel);
    if (idx < 0) {
        printf("FAIL: no adapter matches '%s'. Run 'list' to see valid indices/LUIDs.\n", sel);
        PrintAdapters(a, n);
        return 1;
    }
    // Validate the target device can actually be opened — a D3D12-open failure here means the
    // proxy would fail at runtime with "no GPU-B device" and silently run in passthrough.
    if (!a[idx].d3d12ok) {
        printf("FAIL: adapter [%d] %s could not be opened as a D3D12 device — pick another.\n", idx, a[idx].name);
        return 1;
    }

    char path[MAX_PATH * 2]{};
    snprintf(path, sizeof(path), "%s\\upperscale.ini", dir);
    FILE* f = fopen(path, "w");
    if (!f) { printf("FAIL: cannot write %s\n", path); return 1; }
    fprintf(f, "; upperscale proxy config - generated by upperscale_config.exe\n");
    fprintf(f, "; The drop-in amd_fidelityfx_dx12.dll reads this from its working directory at load.\n");
    fprintf(f, "[proxy]\n");
    fprintf(f, "enable=%d\n", enable);
    fprintf(f, "gpu_luid_low=0x%lx\n", a[idx].luidLo);
    fprintf(f, "gpu_luid_high=0x%lx\n", a[idx].luidHi);
    fprintf(f, "log=%d\n", logLevel);
    fprintf(f, "fg=%d\n", fgOnB);   // 1 = FG also on GPU B (experimental), 0 = FG native on A (default)
    fclose(f);

    printf("Wrote %s\n", path);
    printf("  GPU B (FFX target): [%d] %s  LUID low=0x%lx high=0x%lx\n", idx, a[idx].name, a[idx].luidLo, a[idx].luidHi);
    printf("  enable=%d log=%d fg=%d%s\n", enable, logLevel, fgOnB, fgOnB ? " (EXPERIMENTAL: FG on B)" : "");
    printf("\nNext: drop amd_fidelityfx_dx12.dll + upperscale_real_loader.dll into the same folder,\n");
    printf("then launch the game with FSR enabled. Check <game-dir>\\upperscale.log for 'mode=ACTIVE'.\n");
    return 0;
}

static int CmdShow(const char* dir) {
    char path[MAX_PATH * 2]{};
    snprintf(path, sizeof(path), "%s\\upperscale.ini", dir);
    FILE* f = fopen(path, "r");
    if (!f) { printf("No upperscale.ini in %s\n", dir); return 1; }
    char line[256];
    while (fgets(line, sizeof(line), f)) fputs(line, stdout);
    fclose(f);
    return 0;
}

int main(int argc, char** argv) {
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2 || !strcmp(argv[1], "list") || !strcmp(argv[1], "-l")) return CmdList();
    if (!strcmp(argv[1], "set")) return CmdSet(argc - 2, argv + 2);   // skip exe name AND "set"
    if (!strcmp(argv[1], "show")) {
        char dir[MAX_PATH]{}; _getcwd(dir, MAX_PATH);
        for (int i = 2; i < argc; ++i) if (!strcmp(argv[i], "--dir") && i + 1 < argc) strncpy_s(dir, argv[++i], _TRUNCATE);
        return CmdShow(dir);
    }
    printf("upperscale_config — pick the GPU that runs FFX for the upperscale proxy.\n");
    printf("  list                     enumerate adapters (LUIDs)\n");
    printf("  set <idx|0xLUID> [opts]  write upperscale.ini (--dir, --enable, --log, --fg)\n");
    printf("  show [--dir path]        print the current ini\n");
    return 2;
}

// dump_exports.cpp - list exported functions of a DLL by reading its PE image from disk.
// (LoadLibraryExA proved unreliable in this environment; direct file read is robust.)
#include <windows.h>
#include <stdio.h>

static void walk(const char* path) {
    HANDLE f = CreateFileA(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) { printf("open failed: %lu\n", GetLastError()); return; }
    DWORD szHi = 0;
    DWORD sz = GetFileSize(f, &szHi);
    if (sz == INVALID_FILE_SIZE || sz < sizeof(IMAGE_DOS_HEADER)) { printf("bad size\n"); CloseHandle(f); return; }
    BYTE* img = (BYTE*)malloc(sz);
    DWORD got = 0;
    BOOL ok = ReadFile(f, img, sz, &got, nullptr) && got == sz;
    CloseHandle(f);
    if (!ok) { printf("read failed\n"); free(img); return; }

    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)img;
    if (dos->e_magic != 0x5A4D || dos->e_lfanew >= sz) { printf("not a PE file\n"); free(img); return; }
    IMAGE_NT_HEADERS64* nt = (IMAGE_NT_HEADERS64*)(img + dos->e_lfanew);
    if (nt->Signature != 0x00004550) { printf("bad NT signature\n"); free(img); return; }

    // Convert RVA -> file offset via section table
    auto rva2off = [&](DWORD rva) -> DWORD {
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; ++i) {
            IMAGE_SECTION_HEADER* s = &IMAGE_FIRST_SECTION(nt)[i];
            if (rva >= s->VirtualAddress && rva < s->VirtualAddress + s->Misc.VirtualSize)
                return rva - s->VirtualAddress + s->PointerToRawData;
        }
        // fallback: assume raw == virtual (common for small export dirs in first section)
        return rva;
    };

    IMAGE_DATA_DIRECTORY expDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!expDir.VirtualAddress) { printf("no export directory\n"); free(img); return; }
    DWORD edOff = rva2off(expDir.VirtualAddress);
    if (edOff + sizeof(IMAGE_EXPORT_DIRECTORY) > sz) { printf("export dir out of bounds\n"); free(img); return; }
    IMAGE_EXPORT_DIRECTORY* ed = (IMAGE_EXPORT_DIRECTORY*)(img + edOff);

    DWORD namesOff = rva2off(ed->AddressOfNames);
    DWORD ordsOff  = rva2off(ed->AddressOfNameOrdinals);
    DWORD funcsOff = rva2off(ed->AddressOfFunctions);
    if (namesOff + ed->NumberOfNames * 4 > sz || ordsOff + ed->NumberOfNames * 2 > sz) { printf("bad name tables\n"); free(img); return; }

    DWORD* names = (DWORD*)(img + namesOff);
    WORD*  ords  = (WORD*)(img + ordsOff);
    // functions table: NumberOfFunctions entries, but only first NumberOfNames are named
    if (funcsOff + ed->NumberOfFunctions * 4 > sz) { printf("bad func table\n"); free(img); return; }
    DWORD* funcs = (DWORD*)(img + funcsOff);

    printf("DLL: %s  exports=%lu\n", path, ed->NumberOfNames);
    for (DWORD i = 0; i < ed->NumberOfNames; ++i) {
        DWORD nameRva = names[i];
        if (nameRva >= sz) continue;
        const char* name = (const char*)(img + rva2off(nameRva));
        DWORD fva = funcs[ords[i]];
        if (fva >= expDir.VirtualAddress && fva < expDir.VirtualAddress + expDir.Size) {
            // forwarded export
            printf("  %-40s FWD->%s\n", name, (const char*)(img + rva2off(fva)));
        } else {
            printf("  %s\n", name);
        }
    }
    free(img);
}

int main(int argc, char** argv) {
    if (argc < 2) return 1;
    for (int i = 1; i < argc; ++i) walk(argv[i]);
    return 0;
}

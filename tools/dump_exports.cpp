// dump_exports.cpp - list exported functions of a DLL via GetProcAddress enumeration
#include <windows.h>
#include <stdio.h>

int main(int argc, char** argv) {
    if (argc < 2) return 1;
    HMODULE h = LoadLibraryExA(argv[1], nullptr, LOAD_LIBRARY_AS_DATAFILE);
    if (!h) { printf("LoadLibraryEx failed: %lu\n", GetLastError()); return 1; }

    // Read export directory manually from the loaded image base
    BYTE* base = (BYTE*)h;
    IMAGE_DOS_HEADER* dos = (IMAGE_DOS_HEADER*)base;
    IMAGE_NT_HEADERS* nt = (IMAGE_NT_HEADERS*)(base + dos->e_lfanew);
    IMAGE_DATA_DIRECTORY expDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
    if (!expDir.VirtualAddress) { printf("no export directory\n"); return 0; }
    IMAGE_EXPORT_DIRECTORY* ed = (IMAGE_EXPORT_DIRECTORY*)(base + expDir.VirtualAddress);
    DWORD* names = (DWORD*)(base + ed->AddressOfNames);
    WORD* ords = (WORD*)(base + ed->AddressOfNameOrdinals);
    DWORD* funcs = (DWORD*)(base + ed->AddressOfFunctions);

    printf("DLL: %s  exports=%lu\n", argv[1], ed->NumberOfNames);
    for (DWORD i = 0; i < ed->NumberOfNames; ++i) {
        const char* name = (const char*)(base + names[i]);
        DWORD fva = funcs[ords[i]];
        if (fva >= expDir.VirtualAddress && fva < expDir.VirtualAddress + expDir.Size) {
            printf("  %-40s FWD->%s\n", name, (const char*)(base + fva));
        } else {
            printf("  %s\n", name);
        }
    }
    return 0;
}

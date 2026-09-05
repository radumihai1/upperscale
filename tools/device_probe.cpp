// dxgi_probe.cpp - enumerate DXGI adapters, LUIDs, D3D12 capability
#include <d3d12.h>
#include <dxgi1_6.h>
#include <stdio.h>

int main() {
    IDXGIFactory1* factory = nullptr;
    HRESULT hr = CreateDXGIFactory1(IID_PPV_ARGS(&factory));
    if (FAILED(hr)) { printf("CreateDXGIFactory1 FAILED 0x%08X\n", (unsigned)hr); return 1; }

    int count = 0;
    for (UINT i = 0; ; ++i) {
        IDXGIAdapter1* adapter = nullptr;
        hr = factory->EnumAdapters1(i, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND || FAILED(hr)) break;
        DXGI_ADAPTER_DESC1 desc{};
        adapter->GetDesc1(&desc);

        // D3D12 device check
        ID3D12Device* dev = nullptr;
        HRESULT dhr = D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev));
        bool d3d12ok = SUCCEEDED(dhr);

        // LUID as 64-bit
        unsigned long long luid = ((unsigned long long)desc.AdapterLuid.HighPart << 32) | desc.AdapterLuid.LowPart;

        wchar_t name[128];
        wcsncpy(name, desc.Description, 127);
        char nameA[512]{};
        WideCharToMultiByte(CP_UTF8, 0, name, -1, nameA, sizeof(nameA), nullptr, nullptr);

        printf("Adapter %u: LUID=0x%016llX d3d12=%s vendor=0x%04X device=0x%04X VRAM=%.1fMB\n",
               i, luid, d3d12ok ? "YES" : "no", desc.VendorId, desc.DeviceId,
               (double)desc.DedicatedVideoMemory / 1048576.0);
        printf("    %s\n", nameA);

        if (dev) dev->Release();
        adapter->Release();
        count++;
    }
    factory->Release();
    printf("Total adapters: %d\n", count);
    return 0;
}

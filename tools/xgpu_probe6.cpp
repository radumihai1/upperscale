// xgpu_probe6.cpp — Matrix sweep: buffer creation by (size x heap type x state).
// Finds the exact rule that makes some committed-buffer configs E_INVALIDARG here.
#include <d3d12.h>
#include <dxgi1_6.h>
#include <combaseapi.h>
#include <stdio.h>

static void Try(ID3D12Device* dev, D3D12_HEAP_TYPE ht, UINT64 w, D3D12_RESOURCE_STATES st) {
    const char* htn[] = {"DEF","UP ","RB "};
    int hi = (ht==D3D12_HEAP_TYPE_DEFAULT)?0:(ht==D3D12_HEAP_TYPE_UPLOAD?1:2);
    const char* stn = (st==D3D12_RESOURCE_STATE_COMMON)?"COMMON":(st==D3D12_RESOURCE_STATE_GENERIC_READ?"GENREAD":"COPYDEST");
    D3D12_HEAP_PROPERTIES hp{}; hp.Type = ht;
    D3D12_RESOURCE_DESC rd{};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = w; rd.Height = 1; rd.DepthOrArraySize = 1; rd.MipLevels = 1;
    rd.Format = DXGI_FORMAT_UNKNOWN; rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    rd.SampleDesc.Count = 1;
    ID3D12Resource* r = nullptr;
    HRESULT hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, st, nullptr, IID_PPV_ARGS(&r));
    printf("  %-4s %8llu B  %-9s -> %s\n", htn[hi], (unsigned long long)w, stn, SUCCEEDED(hr)?"OK":"FAIL");
    if (r) r->Release();
}

int main() {
    setvbuf(stdout, NULL, _IONBF, 0);
    IDXGIFactory1* f = nullptr; CreateDXGIFactory1(IID_PPV_ARGS(&f));
    IDXGIAdapter1* a = nullptr; f->EnumAdapters1(1, &a); // 7900 XTX
    ID3D12Device* dev = nullptr; D3D12CreateDevice(a, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev));

    UINT64 sizes[] = {4096, 65536, 131072, 262144, 524288, 589824, 589828, 1048576};
    D3D12_HEAP_TYPE hts[] = {D3D12_HEAP_TYPE_DEFAULT, D3D12_HEAP_TYPE_UPLOAD, D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_STATES sts[] = {D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_GENERIC_READ, D3D12_RESOURCE_STATE_COPY_DEST};

    for (int h=0;h<3;h++)
        for (int s=0;s<8;s++)
            for (int st=0;st<3;st++)
                Try(dev, hts[h], sizes[s], sts[st]);

    dev->Release(); a->Release(); f->Release();
    return 0;
}

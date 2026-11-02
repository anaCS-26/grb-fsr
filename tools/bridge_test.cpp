// Offline test of the DX11 -> DX12 bridge (src/bridge12.cpp), no game needed. Built by tools/bridge_test.sh.
// Creates a D3D11 device on the discrete GPU (or the adapter given as argument 1), the same shared textures
// the mod uses, then for many frames: D3D11 writes a new colour, D3D12 upscales it, D3D11 reads the output
// back. A stale or wrong value means the sharing or the fence hand-off is broken.
#include "../src/bridge12.h"
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>

Config g_cfg;
wchar_t g_modDir[MAX_PATH];

void Log(int level, const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
}

static uint16_t Half(float f) // exact for the values used here
{
    uint32_t u;
    memcpy(&u, &f, 4);
    if ((u & 0x7fffffff) == 0)
        return (uint16_t)(u >> 16);
    int e = (int)((u >> 23) & 0xff) - 127 + 15;
    return (uint16_t)(((u >> 16) & 0x8000) | (e << 10) | ((u >> 13) & 0x3ff));
}

typedef HRESULT(WINAPI* PFN_CreateFactory1)(REFIID, void**);

int main(int argc, char** argv)
{
    int want = argc > 1 ? atoi(argv[1]) : -1;
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    PFN_CreateFactory1 cf = (PFN_CreateFactory1)GetProcAddress(dxgi, "CreateDXGIFactory1");
    IDXGIFactory1* fac = nullptr;
    cf(__uuidof(IDXGIFactory1), (void**)&fac);
    IDXGIAdapter1* pick = nullptr;
    SIZE_T best = 0;
    IDXGIAdapter1* a = nullptr;
    for (UINT i = 0; fac->EnumAdapters1(i, &a) == S_OK; i++)
    {
        DXGI_ADAPTER_DESC1 d;
        a->GetDesc1(&d);
        printf("adapter %u: %ls (%zu MB)\n", i, d.Description, d.DedicatedVideoMemory >> 20);
        bool take = want >= 0 ? (int)i == want : (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && d.DedicatedVideoMemory > best);
        if (take)
        {
            if (pick)
                pick->Release();
            pick = a;
            best = d.DedicatedVideoMemory;
        }
        else
            a->Release();
    }

    ID3D11Device* dev = nullptr;
    ID3D11DeviceContext* imm = nullptr;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_1;
    HRESULT hr = D3D11CreateDevice(pick, D3D_DRIVER_TYPE_UNKNOWN, nullptr, 0, &fl, 1, D3D11_SDK_VERSION, &dev, nullptr, &imm);
    if (FAILED(hr))
    {
        printf("D3D11CreateDevice failed 0x%08lX\n", hr);
        return 1;
    }
    if (!BridgeInit(dev))
        return 1;

    const UINT lw = 640, lh = 360, fw = 960, fh = 540;
    const UINT rw = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS;
    ID3D12Resource *c12, *o12, *m12, *d12, *b12;
    ID3D11Texture2D* color = BridgeMakeTex(lw, lh, DXGI_FORMAT_R16G16B16A16_FLOAT, rw, &c12);
    ID3D11Texture2D* out = BridgeMakeTex(fw, fh, DXGI_FORMAT_R16G16B16A16_FLOAT, rw, &o12);
    ID3D11Texture2D* mv = BridgeMakeTex(lw, lh, DXGI_FORMAT_R16G16_FLOAT, rw, &m12);
    ID3D11Texture2D* depth = BridgeMakeTex(lw, lh, DXGI_FORMAT_R32_FLOAT, rw, &d12);
    ID3D11Texture2D* bias = BridgeMakeTex(lw, lh, DXGI_FORMAT_R8_UNORM, rw, &b12);
    if (!color || !out || !mv || !depth || !bias)
    {
        printf("FAIL: shared texture creation\n");
        return 1;
    }
    printf("shared textures OK (RGBA16F, RG16F, R32F, R8)\n");

    D3D11_TEXTURE2D_DESC sd = {};
    out->GetDesc(&sd);
    sd.Usage = D3D11_USAGE_STAGING;
    sd.BindFlags = 0;
    sd.MiscFlags = 0;
    sd.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D* staging = nullptr;
    dev->CreateTexture2D(&sd, nullptr, &staging);

    uint16_t* px = (uint16_t*)malloc(lw * lh * 8);
    int bad = 0;
    const int frames = 300;
    LARGE_INTEGER f0, f1, qf;
    QueryPerformanceFrequency(&qf);
    QueryPerformanceCounter(&f0);
    for (int f = 0; f < frames; f++)
    {
        float v = (float)(f % 64) / 64.0f; // exact in half precision
        uint16_t h = Half(v), one = Half(1.0f);
        for (UINT i = 0; i < lw * lh; i++)
        {
            px[i * 4 + 0] = h;
            px[i * 4 + 1] = h;
            px[i * 4 + 2] = h;
            px[i * 4 + 3] = one;
        }
        imm->UpdateSubresource(color, 0, nullptr, px, lw * 8, 0);
        ID3D12GraphicsCommandList* cl = BridgeBegin(imm);
        if (!cl || !BridgeTestUpscale(cl, c12, o12, fw, fh) || !BridgeEnd(imm))
        {
            printf("FAIL: bridge submission at frame %d\n", f);
            return 1;
        }
        imm->CopyResource(staging, out);
        D3D11_MAPPED_SUBRESOURCE m;
        if (FAILED(imm->Map(staging, 0, D3D11_MAP_READ, 0, &m)))
        {
            printf("FAIL: map\n");
            return 1;
        }
        int wrong = 0;
        for (UINT y = 0; y < fh; y += 37)
            for (UINT x = 0; x < fw; x += 41)
            {
                const uint16_t* p = (const uint16_t*)((const uint8_t*)m.pData + y * m.RowPitch) + x * 4;
                if (p[0] != h || p[3] != one)
                    wrong++;
            }
        if (wrong && bad < 5)
        {
            const uint16_t* p = (const uint16_t*)m.pData;
            printf("frame %d: %d wrong samples (expected 0x%04X, pixel 0 = 0x%04X)\n", f, wrong, h, p[0]);
        }
        bad += wrong ? 1 : 0;
        imm->Unmap(staging, 0);
    }
    QueryPerformanceCounter(&f1);
    printf("%d frames, %d with wrong output, %.3f ms per frame (upload + bridge + readback)\n", frames, bad,
           (double)(f1.QuadPart - f0.QuadPart) * 1000.0 / (double)qf.QuadPart / frames);
    BridgeWaitIdle();
    printf(bad ? "FAIL\n" : "PASS\n");
    return bad ? 1 : 0;
}

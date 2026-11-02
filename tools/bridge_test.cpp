// Offline test of the DX11 -> DX12 bridge (src/bridge12.cpp), no game needed. Built by tools/bridge_test.sh.
// Creates a D3D11 device on the discrete GPU (or the adapter given as argument 1), the same shared textures
// the mod uses, then for many frames: D3D11 writes a new colour, D3D12 upscales it, D3D11 reads the output
// back. A stale or wrong value means the sharing or the fence hand-off is broken.
// With "fsr <dir>" it then runs FSR (src/fsr.cpp) through the bridge, loading the FidelityFX DLLs from <dir>,
// on a flat-colour frame, and checks the output converges to that colour.
#include "../src/bridge12.h"
#include "../src/fsr.h"
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
    int want = -1;
    const char* fsrDir = nullptr;
    int fsrMajor = 0;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "fsr") && i + 1 < argc)
            fsrDir = argv[++i];
        else if (!strcmp(argv[i], "ver") && i + 1 < argc)
            fsrMajor = atoi(argv[++i]);
        else
            want = atoi(argv[i]);
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

    const UINT lw = 960, lh = 540, fw = 1920, fh = 1080; // the game at 1080p, 50% render scale
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
    printf(bad ? "bridge FAIL\n" : "bridge PASS\n");
    if (bad || !fsrDir)
        return bad ? 1 : 0;

    // ---- FSR on the bridge
    MultiByteToWideChar(CP_UTF8, 0, fsrDir, -1, g_modDir, MAX_PATH);
    size_t n = wcslen(g_modDir);
    if (n && g_modDir[n - 1] != L'\\' && g_modDir[n - 1] != L'/')
        lstrcatW(g_modDir, L"\\");
    if (!FsrLoad())
        return 1;
    void* ctx = FsrCreate(lw, lh, fw, fh, FSR_DEPTH_INVERTED, fsrMajor);
    if (!ctx)
        return 1;
    const float c = 0.5f;
    uint16_t hc = Half(c), one = Half(1.0f);
    for (UINT i = 0; i < lw * lh; i++)
    {
        px[i * 4 + 0] = hc;
        px[i * 4 + 1] = hc;
        px[i * 4 + 2] = hc;
        px[i * 4 + 3] = one;
    }
    imm->UpdateSubresource(color, 0, nullptr, px, lw * 8, 0);
    float* dz = (float*)malloc(lw * lh * 4);
    for (UINT i = 0; i < lw * lh; i++)
        dz[i] = 0.01f;
    imm->UpdateSubresource(depth, 0, nullptr, dz, lw * 4, 0);
    memset(px, 0, lw * lh * 4);
    imm->UpdateSubresource(mv, 0, nullptr, px, lw * 4, 0); // no motion
    const float jit[8][2] = {{0.0f, -0.17f}, {-0.25f, 0.17f}, {0.25f, -0.39f}, {-0.38f, -0.06f},
                             {0.12f, 0.28f},  {-0.12f, -0.28f}, {0.38f, 0.06f}, {-0.44f, 0.39f}};
    int fsrBad = 0;
    QueryPerformanceCounter(&f0);
    const int fsrFrames = 120;
    for (int f = 0; f < fsrFrames; f++)
    {
        if (f == fsrFrames / 2) // time the second half only: the first frames include FSR's warm-up
        {
            BridgeWaitIdle();
            QueryPerformanceCounter(&f0);
        }
        ID3D12GraphicsCommandList* cl = BridgeBegin(imm);
        FsrInputs in = {};
        in.color = c12;
        in.depth = d12;
        in.motion = m12;
        in.output = o12;
        in.renderW = lw;
        in.renderH = lh;
        in.outW = fw;
        in.outH = fh;
        in.jitterX = jit[f % 8][0];
        in.jitterY = jit[f % 8][1];
        in.mvScaleX = (float)lw;
        in.mvScaleY = (float)lh;
        in.frameTimeMs = 16.7f;
        in.cameraNear = 0.1f;
        in.cameraFar = 10000.0f;
        in.fovY = 1.0f;
        in.reset = f == 0;
        bool ok = cl && FsrDispatch(ctx, cl, in);
        if (!BridgeEnd(imm) || !ok)
        {
            printf("FAIL: FSR dispatch at frame %d\n", f);
            return 1;
        }
    }
    BridgeWaitIdle();
    QueryPerformanceCounter(&f1);
    imm->CopyResource(staging, out);
    D3D11_MAPPED_SUBRESOURCE m;
    imm->Map(staging, 0, D3D11_MAP_READ, 0, &m);
    float lo = 1e9f, hi = -1e9f;
    for (UINT y = 0; y < fh; y += 7)
        for (UINT x = 0; x < fw; x += 11)
        {
            uint16_t hv = ((const uint16_t*)((const uint8_t*)m.pData + y * m.RowPitch))[x * 4];
            // half -> float (normal numbers only; others count as wrong)
            int e = (hv >> 10) & 0x1f;
            float v = e == 0 || e == 31 ? -1.0f : (float)(1024 + (hv & 0x3ff)) / 1024.0f * (float)(1 << e) / 32768.0f;
            lo = v < lo ? v : lo;
            hi = v > hi ? v : hi;
            if (!(v > c - 0.02f && v < c + 0.02f))
                fsrBad++;
        }
    imm->Unmap(staging, 0);
    printf("FSR %s: %d frames, %.3f ms per frame (steady state, incl. hand-off), output range %.4f..%.4f (want %.3f), %d bad samples\n",
           FsrActiveVersion(), fsrFrames, (double)(f1.QuadPart - f0.QuadPart) * 1000.0 / (double)qf.QuadPart / (fsrFrames - fsrFrames / 2), lo, hi, c, fsrBad);
    BridgeWaitIdle();
    FsrDestroy(ctx);
    printf(fsrBad ? "FSR FAIL\n" : "FSR PASS\n");
    return fsrBad ? 1 : 0;
}

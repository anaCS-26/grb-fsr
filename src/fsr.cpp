// FSR through the FidelityFX API. See fsr.h.
#include "fsr.h"
#include "../third_party/ffx/api/include/dx12/ffx_api_dx12.h"
#include "../third_party/ffx/upscalers/include/ffx_upscale.h"

static int s_state; // 0 = not tried, 1 = ready, 2 = failed
static PfnFfxCreateContext s_create;
static PfnFfxDestroyContext s_destroy;
static PfnFfxQuery s_query;
static PfnFfxDispatch s_dispatch;
static uint64_t s_dispatches;

static void FsrMessage(uint32_t type, const wchar_t* msg)
{
    char a[1024];
    WideCharToMultiByte(CP_UTF8, 0, msg ? msg : L"", -1, a, sizeof(a), nullptr, nullptr);
    Log(type == FFX_API_MESSAGE_TYPE_ERROR ? LOG_ERR : LOG_INFO, "[FSR] %s", a);
}

bool FsrLoad()
{
    if (s_state)
        return s_state == 1;
    s_state = 2;
    wchar_t p[MAX_PATH];
    lstrcpyW(p, g_modDir);
    lstrcatW(p, L"amd_fidelityfx_loader_dx12.dll");
    HMODULE m = LoadLibraryW(p);
    if (!m)
    {
        LOGE("FSR: amd_fidelityfx_loader_dx12.dll not found next to the mod (error %lu). Copy it and "
             "amd_fidelityfx_upscaler_dx12.dll from the release into the game folder.",
             GetLastError());
        return false;
    }
    s_create = (PfnFfxCreateContext)GetProcAddress(m, "ffxCreateContext");
    s_destroy = (PfnFfxDestroyContext)GetProcAddress(m, "ffxDestroyContext");
    s_query = (PfnFfxQuery)GetProcAddress(m, "ffxQuery");
    s_dispatch = (PfnFfxDispatch)GetProcAddress(m, "ffxDispatch");
    if (!s_create || !s_destroy || !s_query || !s_dispatch)
    {
        LOGE("FSR: the FidelityFX loader is missing exports");
        return false;
    }

    // what the loader can offer on this GPU (driver-provided versions are marked with *)
    uint64_t count = 0;
    ffxQueryDescGetVersions q = {};
    q.header.type = FFX_API_QUERY_DESC_TYPE_GET_VERSIONS;
    q.createDescType = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    q.device = BridgeDevice();
    q.outputCount = &count;
    s_query(nullptr, &q.header);
    uint64_t ids[16];
    const char* names[16] = {};
    if (count > 16)
        count = 16;
    q.versionIds = ids;
    q.versionNames = names;
    if (count && s_query(nullptr, &q.header) == FFX_API_RETURN_OK)
        for (uint64_t i = 0; i < count; i++)
            LOGI("FSR: available upscaler %s", names[i] ? names[i] : "?");
    else
        LOGI("FSR: the loader reported no upscaler versions");
    s_state = 1;
    return true;
}

void* FsrCreate(UINT renderW, UINT renderH, UINT outW, UINT outH, int flags)
{
    if (s_state != 1)
        return nullptr;
    ffxCreateBackendDX12Desc be = {};
    be.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_BACKEND_DX12;
    be.device = BridgeDevice();
    ffxCreateContextDescUpscaleVersion ver = {};
    ver.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE_VERSION;
    ver.version = FFX_UPSCALER_VERSION;
    ver.header.pNext = &be.header;
    ffxCreateContextDescUpscale cd = {};
    cd.header.type = FFX_API_CREATE_CONTEXT_DESC_TYPE_UPSCALE;
    cd.header.pNext = &ver.header;
    cd.flags = ((flags & FSR_HDR) ? FFX_UPSCALE_ENABLE_HIGH_DYNAMIC_RANGE : 0) |
               ((flags & FSR_DISPLAY_RES_MV) ? FFX_UPSCALE_ENABLE_DISPLAY_RESOLUTION_MOTION_VECTORS : 0) |
               ((flags & FSR_DEPTH_INVERTED) ? FFX_UPSCALE_ENABLE_DEPTH_INVERTED : 0) |
               ((flags & FSR_DEPTH_INFINITE) ? FFX_UPSCALE_ENABLE_DEPTH_INFINITE : 0) |
               ((flags & FSR_AUTO_EXPOSURE) ? FFX_UPSCALE_ENABLE_AUTO_EXPOSURE : 0);
    cd.maxRenderSize.width = renderW;
    cd.maxRenderSize.height = renderH;
    cd.maxUpscaleSize.width = outW;
    cd.maxUpscaleSize.height = outH;
    cd.fpMessage = FsrMessage;

    ffxContext ctx = nullptr;
    ffxReturnCode_t r = s_create(&ctx, &cd.header, nullptr);
    if (r != FFX_API_RETURN_OK)
    {
        // providers older than the version descriptor reject it
        cd.header.pNext = &be.header;
        ctx = nullptr;
        r = s_create(&ctx, &cd.header, nullptr);
    }
    if (r != FFX_API_RETURN_OK || !ctx)
    {
        LOGE("FSR: context creation %ux%u -> %ux%u flags 0x%X failed (%u)", renderW, renderH, outW, outH, cd.flags, r);
        return nullptr;
    }
    ffxQueryGetProviderVersion pv = {};
    pv.header.type = FFX_API_QUERY_DESC_TYPE_GET_PROVIDER_VERSION;
    s_query(&ctx, &pv.header);
    LOGI("FSR context %ux%u -> %ux%u flags 0x%X: using %s", renderW, renderH, outW, outH, cd.flags,
         pv.versionName ? pv.versionName : "?");
    return ctx;
}

static D3D12_RESOURCE_BARRIER Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES from, D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER b = {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    return b;
}

bool FsrDispatch(void* ctx, ID3D12GraphicsCommandList* cl, const FsrInputs& in)
{
    if (!ctx || !in.color || !in.depth || !in.motion || !in.output)
        return false;
    const D3D12_RESOURCE_STATES common = D3D12_RESOURCE_STATE_COMMON, read = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,
                                uav = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
    ID3D12Resource* reads[4] = {in.color, in.depth, in.motion, in.reactive};
    UINT nReads = in.reactive ? 4 : 3;
    D3D12_RESOURCE_BARRIER b[5];
    for (UINT i = 0; i < nReads; i++)
        b[i] = Transition(reads[i], common, read);
    b[nReads] = Transition(in.output, common, uav);
    cl->ResourceBarrier(nReads + 1, b);

    ffxDispatchDescUpscale d = {};
    d.header.type = FFX_API_DISPATCH_DESC_TYPE_UPSCALE;
    d.commandList = cl;
    // the states given here are the ones FSR finds the resources in and leaves them in
    d.color = ffxApiGetResourceDX12(in.color, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.depth = ffxApiGetResourceDX12(in.depth, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.motionVectors = ffxApiGetResourceDX12(in.motion, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    if (in.reactive)
        d.reactive = ffxApiGetResourceDX12(in.reactive, FFX_API_RESOURCE_STATE_COMPUTE_READ);
    d.output = ffxApiGetResourceDX12(in.output, FFX_API_RESOURCE_STATE_UNORDERED_ACCESS);
    d.jitterOffset.x = in.jitterX;
    d.jitterOffset.y = in.jitterY;
    d.motionVectorScale.x = in.mvScaleX;
    d.motionVectorScale.y = in.mvScaleY;
    d.renderSize.width = in.renderW;
    d.renderSize.height = in.renderH;
    d.upscaleSize.width = in.outW;
    d.upscaleSize.height = in.outH;
    d.enableSharpening = in.sharpness > 0.0f;
    d.sharpness = in.sharpness > 1.0f ? 1.0f : in.sharpness;
    d.frameTimeDelta = in.frameTimeMs;
    d.preExposure = 1.0f;
    d.reset = in.reset;
    d.cameraNear = in.cameraNear;
    d.cameraFar = in.cameraFar;
    d.cameraFovAngleVertical = in.fovY;
    d.viewSpaceToMetersFactor = 1.0f;
    ffxReturnCode_t r = s_dispatch((ffxContext*)&ctx, &d.header);

    for (UINT i = 0; i < nReads; i++)
        b[i] = Transition(reads[i], read, common);
    b[nReads] = Transition(in.output, uav, common);
    cl->ResourceBarrier(nReads + 1, b);

    if (r != FFX_API_RETURN_OK)
    {
        LOGE("FSR: dispatch failed (%u)", r);
        return false;
    }
    if (++s_dispatches == 1)
        LOGI("FSR: first dispatch %ux%u -> %ux%u, near %.3f far %.1f vFOV %.1f deg", in.renderW, in.renderH, in.outW,
             in.outH, in.cameraNear, in.cameraFar, in.fovY * 57.29578f);
    return true;
}

void FsrDestroy(void*& ctx)
{
    if (!ctx)
        return;
    ffxContext c = ctx;
    ffxReturnCode_t r = s_destroy(&c, nullptr);
    LOGI("FSR context destroyed (%u)", r);
    ctx = nullptr;
}

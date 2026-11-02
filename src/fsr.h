// FSR upscaling through AMD's FidelityFX API (SDK 2.x: amd_fidelityfx_loader_dx12.dll), recorded on the DX12
// bridge's command list. The loader picks the newest FSR available: the driver's FSR 4 build when the driver
// has one (RX 6000 and newer with a current Adrenalin), else amd_fidelityfx_upscaler_dx12.dll beside it.
#pragma once
#include "bridge12.h"

enum
{
    FSR_HDR = 1 << 0,
    FSR_DISPLAY_RES_MV = 1 << 1, // motion vectors at output resolution
    FSR_DEPTH_INVERTED = 1 << 2,
    FSR_DEPTH_INFINITE = 1 << 3,
    FSR_AUTO_EXPOSURE = 1 << 4,
};

struct FsrInputs
{
    // bridge textures, all in D3D12_RESOURCE_STATE_COMMON (reactive may be null)
    ID3D12Resource *color, *depth, *motion, *reactive, *output;
    UINT renderW, renderH, outW, outH;
    float jitterX, jitterY;     // pixels
    float mvScaleX, mvScaleY;   // motion vector units -> pixels
    float frameTimeMs;
    float sharpness;            // 0 = off, up to 1
    float cameraNear, cameraFar, fovY; // fovY in radians
    bool reset;
};

bool FsrLoad();                 // once; logs the FSR versions available
bool FsrHasMajor(int major);    // the loader offers an FSR <major>.x on this GPU (after FsrLoad)
const char* FsrActiveVersion(); // version of the last context created, "" when none
// major = 0: the newest version the loader offers, else the newest <major>.x. nullptr on failure (logged)
void* FsrCreate(UINT renderW, UINT renderH, UINT outW, UINT outH, int flags, int major);
bool FsrDispatch(void* ctx, ID3D12GraphicsCommandList* cl, const FsrInputs& in);
void FsrDestroy(void*& ctx); // the GPU must be done with the context (BridgeWaitIdle)

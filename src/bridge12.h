// DX11 -> DX12 bridge: a D3D12 device on the game's GPU that shares the mod's textures and one fence with
// the game's D3D11 device, so a D3D12 upscaler (FSR) can run between the mod's prep pass and its resolve pass.
//
// Per frame, on the D3D11 immediate context:
//   prep pass (D3D11) -> BridgeBegin: D3D11 signals N, D3D12 queue waits N -> record D3D12 work ->
//   BridgeEnd: D3D12 signals N+1, D3D11 waits N+1 -> resolve pass (D3D11)
// Shared textures are in D3D12_RESOURCE_STATE_COMMON whenever D3D11 owns them; D3D12 work must transition
// them out of and back into COMMON.
#pragma once
#include "common.h"
// mingw-w64's d3d12.h only matches the MSVC ABI for methods returning structs with this defined
#define WIDL_EXPLICIT_AGGREGATE_RETURNS
#include <d3d12.h>

bool BridgeInit(ID3D11Device* dev); // once; false = unavailable (reason is logged)
bool BridgeReady();
ID3D12Device* BridgeDevice();
ID3D12CommandQueue* BridgeQueue();

// A D3D11 texture shared with the bridge's D3D12 device. *out12 receives the D3D12 resource.
ID3D11Texture2D* BridgeMakeTex(UINT w, UINT h, DXGI_FORMAT f, UINT bind, ID3D12Resource** out12);

// Hands the GPU from D3D11 to D3D12 and returns a reset command list to record into (nullptr on failure).
ID3D12GraphicsCommandList* BridgeBegin(ID3D11DeviceContext* imm);
// Submits the command list and makes the D3D11 immediate context wait for it.
bool BridgeEnd(ID3D11DeviceContext* imm);
// Blocks until all submitted D3D12 work is done (call before releasing D3D12 resources it uses).
void BridgeWaitIdle();

// Test pass for the bridge itself: bilinear upscale of src into dst (both bridge textures, src SRV-able,
// dst UAV-able), with the state transitions. Not an upscaler: the jitter is not removed.
bool BridgeTestUpscale(ID3D12GraphicsCommandList* cl, ID3D12Resource* src, ID3D12Resource* dst, UINT dstW, UINT dstH);

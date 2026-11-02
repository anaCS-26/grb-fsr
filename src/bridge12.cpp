// DX11 -> DX12 bridge. See bridge12.h.
#include "bridge12.h"

static const int kSlots = 3; // command allocators in flight

struct Slot
{
    ID3D12CommandAllocator* alloc;
    UINT64 done; // fence value that marks this allocator's last submission as finished
};

static volatile LONG s_state; // 0 = not tried, 1 = ready, 2 = failed
static ID3D12Device* s_d12;
static ID3D12CommandQueue* s_queue;
static ID3D12GraphicsCommandList* s_cl;
static Slot s_slots[kSlots];
static int s_slot;
static ID3D11Fence* s_f11; // the same fence, seen from each API
static ID3D12Fence* s_f12;
static UINT64 s_fenceVal;
static HANDLE s_event;
static ID3D11DeviceContext* s_immSeen;
static ID3D11DeviceContext4* s_imm4;
static bool s_recording;
static uint64_t s_submits;

// test pass
static int s_testState; // 0 = not tried, 1 = ready, 2 = failed
static ID3D12RootSignature* s_rs;
static ID3D12PipelineState* s_pso;
static ID3D12DescriptorHeap* s_heap;
static UINT s_descSize;

static void LogRemoved(const char* what, HRESULT hr)
{
    HRESULT why = s_d12 ? s_d12->GetDeviceRemovedReason() : S_OK;
    LOGE("DX12 bridge: %s failed 0x%08lX (device removed reason 0x%08lX)", what, hr, why);
}

static bool InitLocked(ID3D11Device* dev)
{
    ID3D11Device5* dev5 = nullptr;
    if (FAILED(dev->QueryInterface(__uuidof(ID3D11Device5), (void**)&dev5)) || !dev5)
    {
        LOGE("DX12 bridge: the D3D11 device has no ID3D11Device5 (D3D11.4 fences); Windows 10 1703+ is needed");
        return false;
    }

    IDXGIDevice* dxgiDev = nullptr;
    IDXGIAdapter* adapter = nullptr;
    dev->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev);
    if (dxgiDev)
        dxgiDev->GetAdapter(&adapter);
    SafeRelease(dxgiDev);
    if (!adapter)
    {
        LOGE("DX12 bridge: could not get the game's DXGI adapter");
        dev5->Release();
        return false;
    }
    DXGI_ADAPTER_DESC ad = {};
    adapter->GetDesc(&ad);
    char name[128];
    WideCharToMultiByte(CP_UTF8, 0, ad.Description, -1, name, sizeof(name), nullptr, nullptr);

    HMODULE m = LoadLibraryW(L"d3d12.dll");
    PFN_D3D12_CREATE_DEVICE create = m ? (PFN_D3D12_CREATE_DEVICE)GetProcAddress(m, "D3D12CreateDevice") : nullptr;
    HRESULT hr = create ? create(adapter, D3D_FEATURE_LEVEL_11_0, __uuidof(ID3D12Device), (void**)&s_d12) : E_NOINTERFACE;
    adapter->Release();
    if (FAILED(hr) || !s_d12)
    {
        LOGE("DX12 bridge: D3D12CreateDevice on %s failed 0x%08lX", name, hr);
        dev5->Release();
        return false;
    }

    D3D12_COMMAND_QUEUE_DESC qd = {};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    hr = s_d12->CreateCommandQueue(&qd, __uuidof(ID3D12CommandQueue), (void**)&s_queue);
    for (int i = 0; i < kSlots && SUCCEEDED(hr); i++)
        hr = s_d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, __uuidof(ID3D12CommandAllocator),
                                           (void**)&s_slots[i].alloc);
    if (SUCCEEDED(hr))
        hr = s_d12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, s_slots[0].alloc, nullptr,
                                      __uuidof(ID3D12GraphicsCommandList), (void**)&s_cl);
    if (FAILED(hr))
    {
        LOGE("DX12 bridge: queue / command list creation failed 0x%08lX", hr);
        dev5->Release();
        return false;
    }
    s_cl->Close();

    // fence created on the D3D11 side and opened in D3D12 (the way OptiScaler's DX11-with-DX12 mode does it)
    HANDLE h = nullptr;
    hr = dev5->CreateFence(0, D3D11_FENCE_FLAG_SHARED, __uuidof(ID3D11Fence), (void**)&s_f11);
    dev5->Release();
    if (SUCCEEDED(hr))
        hr = s_f11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &h);
    if (SUCCEEDED(hr))
    {
        hr = s_d12->OpenSharedHandle(h, __uuidof(ID3D12Fence), (void**)&s_f12);
        CloseHandle(h);
    }
    if (FAILED(hr) || !s_f12)
    {
        LOGE("DX12 bridge: shared fence creation failed 0x%08lX", hr);
        return false;
    }
    s_event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!s_event)
        return false;

    LOGI("DX12 bridge ready on %s (LUID %08lX:%08lX)", name, ad.AdapterLuid.HighPart, ad.AdapterLuid.LowPart);
    return true;
}

bool BridgeInit(ID3D11Device* dev)
{
    if (s_state)
        return s_state == 1;
    bool ok = InitLocked(dev);
    s_state = ok ? 1 : 2;
    return ok;
}

bool BridgeReady() { return s_state == 1; }
ID3D12Device* BridgeDevice() { return s_d12; }
ID3D12CommandQueue* BridgeQueue() { return s_queue; }

ID3D11Texture2D* BridgeMakeTex(UINT w, UINT h, DXGI_FORMAT f, UINT bind, ID3D12Resource** out12)
{
    *out12 = nullptr;
    if (s_state != 1)
        return nullptr;
    ID3D11Device* dev = nullptr;
    s_f11->GetDevice(&dev);
    D3D11_TEXTURE2D_DESC td = {};
    td.Width = w;
    td.Height = h;
    td.MipLevels = 1;
    td.ArraySize = 1;
    td.Format = f;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_DEFAULT;
    td.BindFlags = bind;
    td.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    ID3D11Texture2D* t = nullptr;
    HRESULT hr = dev->CreateTexture2D(&td, nullptr, &t);
    dev->Release();
    IDXGIResource1* r1 = nullptr;
    HANDLE sh = nullptr;
    if (SUCCEEDED(hr))
        hr = t->QueryInterface(__uuidof(IDXGIResource1), (void**)&r1);
    if (SUCCEEDED(hr))
        hr = r1->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr, &sh);
    SafeRelease(r1);
    if (SUCCEEDED(hr))
    {
        hr = s_d12->OpenSharedHandle(sh, __uuidof(ID3D12Resource), (void**)out12);
        CloseHandle(sh);
    }
    if (FAILED(hr) || !*out12)
    {
        LOGE("DX12 bridge: shared texture %ux%u fmt %d failed 0x%08lX", w, h, f, hr);
        SafeRelease(t);
        *out12 = nullptr;
        return nullptr;
    }
    return t;
}

static bool WaitFence(UINT64 v, DWORD ms)
{
    if (s_f12->GetCompletedValue() >= v)
        return true;
    s_f12->SetEventOnCompletion(v, s_event);
    return WaitForSingleObject(s_event, ms) == WAIT_OBJECT_0 || s_f12->GetCompletedValue() >= v;
}

ID3D12GraphicsCommandList* BridgeBegin(ID3D11DeviceContext* imm)
{
    if (s_state != 1 || s_recording)
        return nullptr;
    if (imm != s_immSeen)
    {
        SafeRelease(s_imm4);
        imm->QueryInterface(__uuidof(ID3D11DeviceContext4), (void**)&s_imm4);
        s_immSeen = imm;
        if (!s_imm4)
            LOGE("DX12 bridge: the immediate context has no ID3D11DeviceContext4");
    }
    if (!s_imm4)
        return nullptr;

    Slot& sl = s_slots[s_slot];
    if (!WaitFence(sl.done, 2000))
    {
        LogRemoved("waiting for a free command allocator", E_FAIL);
        return nullptr;
    }
    HRESULT hr = sl.alloc->Reset();
    if (SUCCEEDED(hr))
        hr = s_cl->Reset(sl.alloc, nullptr);
    if (FAILED(hr))
    {
        LogRemoved("command list reset", hr);
        return nullptr;
    }

    // D3D11 work so far (the prep pass) -> D3D12
    UINT64 v = ++s_fenceVal;
    s_imm4->Signal(s_f11, v);
    imm->Flush(); // the D3D12 queue would otherwise wait for a signal D3D11 has not submitted yet
    s_queue->Wait(s_f12, v);
    s_recording = true;
    return s_cl;
}

bool BridgeEnd(ID3D11DeviceContext* imm)
{
    if (!s_recording)
        return false;
    s_recording = false;
    HRESULT hr = s_cl->Close();
    if (SUCCEEDED(hr))
    {
        ID3D12CommandList* lists[1] = {s_cl};
        s_queue->ExecuteCommandLists(1, lists);
    }
    else
        LogRemoved("command list close", hr);
    // signal even if nothing was submitted, so the fence values stay paired
    UINT64 v = ++s_fenceVal;
    s_queue->Signal(s_f12, v);
    s_slots[s_slot].done = v;
    s_slot = (s_slot + 1) % kSlots;
    // D3D12 -> D3D11 work from here on (the resolve pass)
    s_imm4->Wait(s_f11, v);
    if (++s_submits == 1)
        LOGI("DX12 bridge: first submission (fence %I64u)", v);
    return SUCCEEDED(hr);
}

void BridgeWaitIdle()
{
    if (s_state == 1 && s_fenceVal && !WaitFence(s_fenceVal, 2000))
        LogRemoved("waiting for idle", E_FAIL);
}

// ------------------------------------------------------------------ test pass
static const char kTestCS[] = R"(
Texture2D<float4> src : register(t0);
RWTexture2D<float4> dst : register(u0);
SamplerState lin : register(s0);
cbuffer C : register(b0) { float2 invOut; float2 pad; };
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    dst[id.xy] = src.SampleLevel(lin, (float2(id.xy) + 0.5) * invOut, 0);
}
)";

typedef HRESULT(WINAPI* PFN_SerializeRootSig)(const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**,
                                              ID3DBlob**);
typedef HRESULT(WINAPI* PFN_D3DCompileB)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR, LPCSTR,
                                        UINT, UINT, ID3DBlob**, ID3DBlob**);

static bool InitTestPass()
{
    wchar_t p[MAX_PATH];
    lstrcpyW(p, g_modDir);
    lstrcatW(p, L"d3dcompiler_47.dll");
    HMODULE m = LoadLibraryW(p);
    if (!m)
        m = LoadLibraryW(L"d3dcompiler_47.dll");
    PFN_D3DCompileB compile = m ? (PFN_D3DCompileB)GetProcAddress(m, "D3DCompile") : nullptr;
    HMODULE m12 = GetModuleHandleW(L"d3d12.dll");
    PFN_SerializeRootSig serialize =
        m12 ? (PFN_SerializeRootSig)GetProcAddress(m12, "D3D12SerializeRootSignature") : nullptr;
    if (!compile || !serialize)
    {
        LOGE("DX12 bridge test: D3DCompile / D3D12SerializeRootSignature unavailable");
        return false;
    }
    ID3DBlob *cs = nullptr, *err = nullptr;
    HRESULT hr = compile(kTestCS, sizeof(kTestCS) - 1, "GRB_FSR_BridgeTest", nullptr, nullptr, "main", "cs_5_0",
                         D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &cs, &err);
    if (FAILED(hr))
    {
        LOGE("DX12 bridge test shader failed 0x%08lX: %s", hr, err ? (const char*)err->GetBufferPointer() : "?");
        SafeRelease(err);
        return false;
    }
    SafeRelease(err);

    D3D12_DESCRIPTOR_RANGE ranges[2] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 1;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
    ranges[1].NumDescriptors = 1;
    ranges[1].OffsetInDescriptorsFromTableStart = 1;
    D3D12_ROOT_PARAMETER params[2] = {};
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[0].DescriptorTable.NumDescriptorRanges = 2;
    params[0].DescriptorTable.pDescriptorRanges = ranges;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    params[1].Constants.Num32BitValues = 4;
    D3D12_STATIC_SAMPLER_DESC smp = {};
    smp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
    smp.AddressU = smp.AddressV = smp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    smp.MaxLOD = 1000.0f;
    D3D12_ROOT_SIGNATURE_DESC rsd = {};
    rsd.NumParameters = 2;
    rsd.pParameters = params;
    rsd.NumStaticSamplers = 1;
    rsd.pStaticSamplers = &smp;
    ID3DBlob* rsBlob = nullptr;
    hr = serialize(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rsBlob, &err);
    SafeRelease(err);
    if (SUCCEEDED(hr))
        hr = s_d12->CreateRootSignature(0, rsBlob->GetBufferPointer(), rsBlob->GetBufferSize(),
                                        __uuidof(ID3D12RootSignature), (void**)&s_rs);
    SafeRelease(rsBlob);
    if (SUCCEEDED(hr))
    {
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd = {};
        pd.pRootSignature = s_rs;
        pd.CS.pShaderBytecode = cs->GetBufferPointer();
        pd.CS.BytecodeLength = cs->GetBufferSize();
        hr = s_d12->CreateComputePipelineState(&pd, __uuidof(ID3D12PipelineState), (void**)&s_pso);
    }
    SafeRelease(cs);
    if (SUCCEEDED(hr))
    {
        D3D12_DESCRIPTOR_HEAP_DESC hd = {};
        hd.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
        hd.NumDescriptors = 2 * kSlots; // one SRV + UAV pair per command allocator in flight
        hd.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
        hr = s_d12->CreateDescriptorHeap(&hd, __uuidof(ID3D12DescriptorHeap), (void**)&s_heap);
        s_descSize = s_d12->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    }
    if (FAILED(hr))
    {
        LOGE("DX12 bridge test pipeline failed 0x%08lX", hr);
        return false;
    }
    LOGI("DX12 bridge test pass ready");
    return true;
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

bool BridgeTestUpscale(ID3D12GraphicsCommandList* cl, ID3D12Resource* src, ID3D12Resource* dst, UINT dstW, UINT dstH)
{
    if (!s_testState)
        s_testState = InitTestPass() ? 1 : 2;
    if (s_testState != 1 || !src || !dst)
        return false;

    // descriptors for this allocator slot: the GPU is done with them (BridgeBegin waited for the slot)
    D3D12_CPU_DESCRIPTOR_HANDLE cpu = s_heap->GetCPUDescriptorHandleForHeapStart();
    D3D12_GPU_DESCRIPTOR_HANDLE gpu = s_heap->GetGPUDescriptorHandleForHeapStart();
    cpu.ptr += (SIZE_T)s_slot * 2 * s_descSize;
    gpu.ptr += (UINT64)s_slot * 2 * s_descSize;
    s_d12->CreateShaderResourceView(src, nullptr, cpu);
    D3D12_CPU_DESCRIPTOR_HANDLE cpuU = cpu;
    cpuU.ptr += s_descSize;
    s_d12->CreateUnorderedAccessView(dst, nullptr, nullptr, cpuU);

    D3D12_RESOURCE_BARRIER in[2] = {
        Transition(src, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE),
        Transition(dst, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS)};
    cl->ResourceBarrier(2, in);
    cl->SetDescriptorHeaps(1, &s_heap);
    cl->SetComputeRootSignature(s_rs);
    cl->SetPipelineState(s_pso);
    cl->SetComputeRootDescriptorTable(0, gpu);
    float c[4] = {1.0f / (float)dstW, 1.0f / (float)dstH, 0.0f, 0.0f};
    cl->SetComputeRoot32BitConstants(1, 4, c, 0);
    cl->Dispatch((dstW + 7) / 8, (dstH + 7) / 8, 1);
    D3D12_RESOURCE_BARRIER out[2] = {
        Transition(src, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON),
        Transition(dst, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON)};
    cl->ResourceBarrier(2, out);
    return true;
}

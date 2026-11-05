#include "pch.h"
#include "../../retoque/dlssnr/DlssNr_Dx11SceneTransport.h"
#include "../../external/vulkan/include/vulkan/vulkan_core.h"

// Fixture-owned devices only. No NGX, OptiScaler, game hooks or third-party shaders.
// Every frame is produced, inverted on D3D12, consumed on D3D11, then optionally presented.
// No CPU completion observation participates in the transport protocol. Final readback and
// the independent watchdog observe completion; they never signal fences or release a dependency.
// Read-only ABI prefix already declared in OptiScaler/misc/IdentifyGpu.h. No production
// header is included (it pulls model/proxy dependencies). No Vulkan commands are submitted.
MIDL_INTERFACE("39da4e09-bd1c-4198-9fae-86bbe3be41fd")
FixtureInterop : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE GetDXGIAdapter(REFIID iid, void** object) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetInstanceExtensions(UINT * count, const char** extensions) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceExtensions(UINT * count, const char** extensions) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFeatures(const VkPhysicalDeviceFeatures2** features) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetVulkanHandles(VkInstance * instance, VkPhysicalDevice * physical,
                                                       VkDevice * device) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetVulkanQueueInfo(ID3D12CommandQueue * queue, VkQueue * nativeQueue,
                                                         UINT32 * family) = 0;
};

namespace
{
constexpr UINT W = 61, H = 37, Frames = 24, Slots = 4;
FILE* logFile = nullptr;
std::mutex logMutex;
std::atomic<const char*> phase { "setup" };
std::atomic<UINT> submitted { 0 };
std::atomic<UINT> presents { 0 };
ID3D11Device5* dev11 = nullptr;
ID3D11DeviceContext4* ctx = nullptr;
ID3D12Device* dev12 = nullptr;
ID3D12CommandQueue* nrQueue = nullptr;
ID3D12CommandQueue* presentQueue = nullptr;
ID3D11Fence *p11 = nullptr, *n11 = nullptr, *c11 = nullptr, *b11 = nullptr;
ID3D12Fence *p12 = nullptr, *n12 = nullptr, *c12 = nullptr, *b12 = nullptr;

void Log(const std::string& text)
{
    std::lock_guard<std::mutex> lock(logMutex);
    std::fprintf(logFile, "%s\n", text.c_str());
    std::fflush(logFile);
}
std::string Hex(HRESULT hr)
{
    char buf[20];
    std::snprintf(buf, sizeof(buf), "0x%08lX", static_cast<unsigned long>(hr));
    return buf;
}
UINT64 Value(ID3D12Fence* fence) { return fence ? fence->GetCompletedValue() : 0; }

// Process exit delegates unresolved GPU allocations to runtime/process teardown. In particular,
// timeout/error paths never destruct a slot or reset an allocator whose work might still execute.
[[noreturn]] void Finish(const char* status, const char* reason, int code, UINT verified = 0, UINT mismatches = 0)
{
    FILE* f = std::fopen("case.json", "w");
    if (f)
    {
        std::fprintf(f,
                     "{\"status\":\"%s\",\"reason\":\"%s\",\"frames\":%u,\"submitted\":%u,"
                     "\"verified\":%u,\"mismatches\":%u,\"phase\":\"%s\","
                     "\"P\":%llu,\"N\":%llu,\"C\":%llu,\"B\":%llu,\"presents\":%u}\n",
                     status, reason, Frames, submitted.load(), verified, mismatches, phase.load(), Value(p12),
                     Value(n12), Value(c12), Value(b12), presents.load());
        std::fclose(f);
    }
    Log(std::string(status) + ": " + reason);
    ExitProcess(static_cast<UINT>(code));
}
void Check(HRESULT hr, const char* what, bool prerequisite = false)
{
    if (FAILED(hr))
    {
        Log(std::string(what) + "=" + Hex(hr));
        Finish(prerequisite ? "SKIP" : "FAIL", what, prerequisite ? 0 : 1);
    }
}

double Now()
{
    LARGE_INTEGER tick, frequency;
    QueryPerformanceCounter(&tick);
    QueryPerformanceFrequency(&frequency);
    return double(tick.QuadPart) * 1000.0 / double(frequency.QuadPart);
}
struct Costs
{
    double producer = 0, nrSubmit = 0, consumer = 0, present = 0;
    void Report() const
    {
        Log("max CPU call-block milliseconds: producer=" + std::to_string(producer) + " nr-submit=" +
            std::to_string(nrSubmit) + " consumer=" + std::to_string(consumer) + " Present=" + std::to_string(present));
    }
};
void Maximum(double& maximum, double start)
{
    const double elapsed = Now() - start;
    if (elapsed > maximum)
        maximum = elapsed;
}

void Shared11(ID3D11Fence** f11, ID3D12Fence** f12)
{
    Check(dev11->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(f11)), "D3D11 CreateFence", true);
    HANDLE handle = nullptr;
    Check((*f11)->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &handle), "D3D11 fence export", true);
    const HRESULT hr = dev12->OpenSharedHandle(handle, IID_PPV_ARGS(f12));
    CloseHandle(handle);
    Check(hr, "D3D12 fence import", true);
}
void Shared12(ID3D12Fence** f12, ID3D11Fence** f11)
{
    Check(dev12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(f12)), "D3D12 CreateFence", true);
    HANDLE handle = nullptr;
    Check(dev12->CreateSharedHandle(*f12, nullptr, GENERIC_ALL, nullptr, &handle), "D3D12 fence export", true);
    const HRESULT hr = dev11->OpenSharedFence(handle, IID_PPV_ARGS(f11));
    CloseHandle(handle);
    Check(hr, "D3D11 fence import", true);
}

IDXGIAdapter* Devices()
{
    ID3D11Device* base = nullptr;
    ID3D11DeviceContext* context = nullptr;
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0 };
    Check(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, levels, 2, D3D11_SDK_VERSION, &base, nullptr,
                            &context),
          "D3D11 device", true);
    Check(base->QueryInterface(IID_PPV_ARGS(&dev11)), "ID3D11Device5", true);
    Check(context->QueryInterface(IID_PPV_ARGS(&ctx)), "ID3D11DeviceContext4", true);
    IDXGIDevice* dxgi = nullptr;
    IDXGIAdapter* adapter = nullptr;
    Check(base->QueryInterface(IID_PPV_ARGS(&dxgi)), "DXGI device", true);
    Check(dxgi->GetAdapter(&adapter), "DXGI adapter", true);
    DXGI_ADAPTER_DESC desc {};
    Check(adapter->GetDesc(&desc), "adapter description");
    char name[256] {};
    WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name), nullptr, nullptr);
    Log(std::string("adapter=") + name);
    Check(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev12)), "D3D12 device", true);
    D3D12_COMMAND_QUEUE_DESC q {};
    q.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    Check(dev12->CreateCommandQueue(&q, IID_PPV_ARGS(&nrQueue)), "NR queue", true);
    Check(dev12->CreateCommandQueue(&q, IID_PPV_ARGS(&presentQueue)), "Present queue", true);
    Shared11(&p11, &p12);
    Shared12(&n12, &n11);
    Shared11(&c11, &c12);
    Shared12(&b12, &b11);
    Log("shared fences: D3D11 export/D3D12 import and D3D12 export/D3D11 import supported");
    return adapter;
}

D3D11_TEXTURE2D_DESC TextureDesc()
{
    D3D11_TEXTURE2D_DESC d {};
    d.Width = W;
    d.Height = H;
    d.MipLevels = d.ArraySize = d.SampleDesc.Count = 1;
    d.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    d.Usage = D3D11_USAGE_DEFAULT;
    d.BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_UNORDERED_ACCESS | D3D11_BIND_RENDER_TARGET;
    return d;
}
HRESULT OpenTexture(ID3D11Texture2D* texture, bool nt, ID3D12Resource** opened)
{
    IDXGIResource1* resource = nullptr;
    HRESULT hr = texture->QueryInterface(IID_PPV_ARGS(&resource));
    if (FAILED(hr))
        return hr;
    HANDLE handle = nullptr;
    hr = nt ? resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr,
                                           &handle)
            : resource->GetSharedHandle(&handle);
    if (SUCCEEDED(hr))
        hr = dev12->OpenSharedHandle(handle, IID_PPV_ARGS(opened));
    if (nt && handle)
        CloseHandle(handle);
    resource->Release();
    return hr;
}
void Probe()
{
    for (bool nt : { false, true })
    {
        auto desc = TextureDesc();
        desc.MiscFlags =
            nt ? D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED : D3D11_RESOURCE_MISC_SHARED;
        ID3D11Texture2D* tex = nullptr;
        HRESULT hr = dev11->CreateTexture2D(&desc, nullptr, &tex);
        Log(std::string(nt ? "NT" : "legacy") + " texture create=" + Hex(hr));
        if (SUCCEEDED(hr))
        {
            ID3D12Resource* opened = nullptr;
            hr = OpenTexture(tex, nt, &opened);
            Log(std::string(nt ? "NT" : "legacy") + " texture D3D12 open=" + Hex(hr));
        }
    }
    auto desc = TextureDesc();
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;
    ID3D11Texture2D* tex = nullptr;
    HRESULT hr = dev11->CreateTexture2D(&desc, nullptr, &tex);
    Log("keyed texture create=" + Hex(hr));
    if (SUCCEEDED(hr))
    {
        IDXGIKeyedMutex* mutex = nullptr;
        hr = tex->QueryInterface(IID_PPV_ARGS(&mutex));
        Log("D3D11 IDXGIKeyedMutex=" + Hex(hr));
        if (hr == S_OK)
        {
            hr = mutex->AcquireSync(0, 0);
            Log("AcquireSync(0,0)=" + Hex(hr));
            if (hr == S_OK)
            {
                Log("ReleaseSync(1)=" + Hex(mutex->ReleaseSync(1)));
                Log("AcquireSync(wrong key 0, zero timeout)=" + Hex(mutex->AcquireSync(0, 0)));
            }
        }
        ID3D12Resource* opened = nullptr;
        hr = OpenTexture(tex, true, &opened);
        Log("keyed texture D3D12 open=" + Hex(hr));
        if (SUCCEEDED(hr))
        {
            IDXGIKeyedMutex* mutex12 = nullptr;
            hr = opened->QueryInterface(IID_PPV_ARGS(&mutex12));
            Log("D3D12 IDXGIKeyedMutex=" + Hex(hr));
        }
    }
    Log("keyed mutex roundtrip NOT QUALIFIED: resource opening alone supplies no D3D12 acquire/release protocol");
    Finish("PASS", "primitive probe completed", 0);
}

struct Slot
{
    ID3D11Texture2D *source = nullptr, *input = nullptr, *output = nullptr, *consumer = nullptr;
    ID3D12Resource *in12 = nullptr, *out12 = nullptr;
};
std::array<Slot, Slots> slots;
std::array<ID3D11Texture2D*, Frames> snapshots {};
ID3D12DescriptorHeap* heap = nullptr;
ID3D12RootSignature* signature = nullptr;
ID3D12PipelineState* pipeline = nullptr;
UINT increment = 0;

void Prepare()
{
    auto desc = TextureDesc();
    D3D12_DESCRIPTOR_HEAP_DESC h {};
    h.NumDescriptors = Slots * 2;
    h.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    h.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    Check(dev12->CreateDescriptorHeap(&h, IID_PPV_ARGS(&heap)), "descriptor heap");
    increment = dev12->GetDescriptorHandleIncrementSize(h.Type);
    auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
    for (auto& s : slots)
    {
        Check(dev11->CreateTexture2D(&desc, nullptr, &s.source), "producer texture", true);
        Check(dev11->CreateTexture2D(&desc, nullptr, &s.consumer), "consumer texture", true);
        auto shared = desc;
        shared.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED;
        Check(dev11->CreateTexture2D(&shared, nullptr, &s.input), "shared input", true);
        Check(dev11->CreateTexture2D(&shared, nullptr, &s.output), "shared output", true);
        Check(OpenTexture(s.input, true, &s.in12), "input import", true);
        Check(OpenTexture(s.output, true, &s.out12), "output import", true);
        D3D12_SHADER_RESOURCE_VIEW_DESC srv {};
        srv.Format = desc.Format;
        srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        srv.Texture2D.MipLevels = 1;
        dev12->CreateShaderResourceView(s.in12, &srv, cpu);
        cpu.ptr += increment;
        D3D12_UNORDERED_ACCESS_VIEW_DESC uav {};
        uav.Format = desc.Format;
        uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
        dev12->CreateUnorderedAccessView(s.out12, nullptr, &uav, cpu);
        cpu.ptr += increment;
    }
    desc.BindFlags = 0;
    desc.Usage = D3D11_USAGE_STAGING;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (auto& readback : snapshots)
        Check(dev11->CreateTexture2D(&desc, nullptr, &readback), "readback");
    const D3D12_DESCRIPTOR_RANGE ranges[] = {
        { D3D12_DESCRIPTOR_RANGE_TYPE_SRV, 1, 0, 0, 0 },
        { D3D12_DESCRIPTOR_RANGE_TYPE_UAV, 1, 0, 0, 1 },
    };
    D3D12_ROOT_PARAMETER param {};
    param.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    param.DescriptorTable = { 2, ranges };
    D3D12_ROOT_SIGNATURE_DESC root {};
    root.NumParameters = 1;
    root.pParameters = &param;
    ID3DBlob *blob = nullptr, *errors = nullptr;
    Check(D3D12SerializeRootSignature(&root, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &errors), "root serialize");
    Check(dev12->CreateRootSignature(0, blob->GetBufferPointer(), blob->GetBufferSize(), IID_PPV_ARGS(&signature)),
          "root signature");
    const char* shader = "Texture2D<float4> src:register(t0); RWTexture2D<float4> dst:register(u0);"
                         "[numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID){"
                         "if(p.x<61 && p.y<37) dst[p.xy]=1.0-src.Load(int3(p.xy,0));}";
    ID3DBlob* bytecode = nullptr;
    Check(D3DCompile(shader, std::strlen(shader), "fixture", nullptr, nullptr, "main", "cs_5_0", 0, 0, &bytecode,
                     &errors),
          "fixture shader compile");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso {};
    pso.pRootSignature = signature;
    pso.CS = { bytecode->GetBufferPointer(), bytecode->GetBufferSize() };
    Check(dev12->CreateComputePipelineState(&pso, IID_PPV_ARGS(&pipeline)), "compute PSO");
}

void Barrier(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
             D3D12_RESOURCE_STATES after)
{
    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = { resource, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, before, after };
    list->ResourceBarrier(1, &b);
}
ID3D12GraphicsCommandList* List()
{
    ID3D12CommandAllocator* allocator = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    Check(dev12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator)), "allocator");
    Check(dev12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator, nullptr, IID_PPV_ARGS(&list)), "list");
    // One allocator/list per recording, retained until process exit. No in-flight Reset or replay.
    return list;
}
void Execute(ID3D12CommandQueue* queue, ID3D12GraphicsCommandList* list)
{
    Check(list->Close(), "list close");
    ID3D12CommandList* lists[] = { list };
    queue->ExecuteCommandLists(1, lists);
}
IDXGISwapChain3* Swapchain(IDXGIAdapter* adapter)
{
    WNDCLASSW wc {};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"RetoqueTransportFixture";
    RegisterClassW(&wc);
    HWND window = CreateWindowExW(WS_EX_NOACTIVATE, wc.lpszClassName, L"Retoque transport fixture", WS_OVERLAPPEDWINDOW,
                                  0, 0, 160, 100, nullptr, nullptr, wc.hInstance, nullptr);
    if (!window)
        Finish("SKIP", "fixture window unavailable", 0);
    ShowWindow(window, SW_SHOWNOACTIVATE);
    IDXGIFactory2* factory = nullptr;
    Check(adapter->GetParent(IID_PPV_ARGS(&factory)), "factory", true);
    DXGI_SWAP_CHAIN_DESC1 desc {};
    desc.Width = W;
    desc.Height = H;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    IDXGISwapChain1* base = nullptr;
    Check(factory->CreateSwapChainForHwnd(presentQueue, window, &desc, nullptr, nullptr, &base), "swapchain", true);
    IDXGISwapChain3* swapchain = nullptr;
    Check(base->QueryInterface(IID_PPV_ARGS(&swapchain)), "swapchain3", true);
    return swapchain;
}
std::vector<uint8_t> Pattern(UINT frame)
{
    std::vector<uint8_t> bytes(W * H * 4);
    for (size_t i = 0; i < bytes.size(); ++i)
        bytes[i] = static_cast<uint8_t>((i * 37 + frame * 53 + i / (W * 4) * 11) & 255);
    return bytes;
}
void PresentWork(IDXGISwapChain3* swapchain, Slot& s, UINT64 value)
{
    phase = "Present queue Wait C / execute / Signal B";
    Check(presentQueue->Wait(c12, value), "Present Wait C");
    auto* list = List();
    ID3D12Resource* backbuffer = nullptr;
    Check(swapchain->GetBuffer(swapchain->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backbuffer)), "backbuffer");
    Barrier(list, s.out12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Barrier(list, backbuffer, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
    list->CopyResource(backbuffer, s.out12);
    Barrier(list, backbuffer, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
    Barrier(list, s.out12, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    Execute(presentQueue, list);
    Check(presentQueue->Signal(b12, value), "Present Signal B");
}

void Roundtrip(IDXGIAdapter* adapter, const std::string& mode)
{
    const bool present = mode == "present" || mode == "present-ahead" || mode == "forced-alias";
    const bool ahead = mode == "present-ahead" || mode == "forced-alias";
    const bool mutant = mode == "mutant-copy";
    if (mode == "forced-alias")
    {
        nrQueue = presentQueue;
        Log("CONTROL: same D3D12 queue forced, Wait C precedes the NR work needed to signal C");
    }
    FixtureInterop* interop = nullptr;
    const HRESULT interopHr = dev12->QueryInterface(IID_PPV_ARGS(&interop));
    Log("vkd3d DXVK interop query=" + Hex(interopHr));
    if (SUCCEEDED(interopHr))
    {
        VkQueue nr = VK_NULL_HANDLE, pr = VK_NULL_HANDLE;
        UINT32 nrFamily = 0, prFamily = 0;
        const HRESULT nrHr = interop->GetVulkanQueueInfo(nrQueue, &nr, &nrFamily);
        const HRESULT prHr = interop->GetVulkanQueueInfo(presentQueue, &pr, &prFamily);
        Log("GetVulkanQueueInfo NR=" + Hex(nrHr) + " Present=" + Hex(prHr));
        if (SUCCEEDED(nrHr) && SUCCEEDED(prHr))
            Log("native queues: same=" + std::to_string(nr == pr) + " NR-family=" + std::to_string(nrFamily) +
                " Present-family=" + std::to_string(prFamily));
        interop->Release();
    }
    IDXGISwapChain3* swapchain = present ? Swapchain(adapter) : nullptr;
    Prepare();
    Log("TOPOLOGY armed: Q11 producer P -> Qnr process N -> Q11 consumer C -> Qpresent copy B; "
        "four slots; no CPU completion checks between frames");
    // Independent observer. It never releases a wait and never drives the submission loop.
    std::thread(
        []
        {
            std::this_thread::sleep_for(std::chrono::seconds(12));
            Log("watchdog P=" + std::to_string(Value(p12)) + " N=" + std::to_string(Value(n12)) +
                " C=" + std::to_string(Value(c12)) + " B=" + std::to_string(Value(b12)));
            Finish("STALL", "watchdog expired; stall is not by itself a proven queue cycle", 3);
        })
        .detach();
    Costs costs;
    const double begin = Now();
    for (UINT f = 0; f < Frames; ++f)
    {
        const UINT64 value = f + 1;
        Slot& s = slots[f % Slots];
        Log("frame=" + std::to_string(f) + " begin");
        if (ahead)
            PresentWork(swapchain, s, value); // Future C waits ahead of NR on a second logical D3D12 queue.
        phase = "D3D11 producer / Wait previous B / Signal P / Flush";
        double start = Now();
        if (present && f >= Slots)
            Check(ctx->Wait(b11, value - Slots), "D3D11 slot reuse Wait B");
        const auto pattern = Pattern(f);
        ctx->UpdateSubresource(s.source, 0, nullptr, pattern.data(), W * 4, 0);
        ctx->CopyResource(s.input, s.source);
        Check(ctx->Signal(p11, value), "D3D11 Signal P");
        ctx->Flush();
        Maximum(costs.producer, start);
        phase = "NR queue Wait P / process / Signal N";
        start = Now();
        Check(nrQueue->Wait(p12, value), "NR Wait P");
        auto* list = List();
        if (mutant)
        {
            Barrier(list, s.in12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Barrier(list, s.out12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
            list->CopyResource(s.out12, s.in12); // Negative control: omitting inversion must fail the pixel oracle.
            Barrier(list, s.in12, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
            Barrier(list, s.out12, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
        }
        else
        {
            Barrier(list, s.in12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Barrier(list, s.out12, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            list->SetPipelineState(pipeline);
            list->SetComputeRootSignature(signature);
            list->SetDescriptorHeaps(1, &heap);
            auto gpu = heap->GetGPUDescriptorHandleForHeapStart();
            gpu.ptr += UINT64(f % Slots) * 2 * increment;
            list->SetComputeRootDescriptorTable(0, gpu);
            list->Dispatch((W + 7) / 8, (H + 7) / 8, 1);
            Barrier(list, s.in12, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
            Barrier(list, s.out12, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
        }
        Execute(nrQueue, list);
        Check(nrQueue->Signal(n12, value), "NR Signal N");
        Maximum(costs.nrSubmit, start);
        phase = "D3D11 Wait N / consumer / Signal C / Flush";
        start = Now();
        Check(ctx->Wait(n11, value), "D3D11 Wait N");
        ctx->CopyResource(s.consumer, s.output);
        ctx->CopyResource(snapshots[f], s.consumer);
        Check(ctx->Signal(c11, value), "D3D11 Signal C");
        ctx->Flush();
        Maximum(costs.consumer, start);
        submitted = f + 1;
        if (present)
        {
            if (!ahead)
                PresentWork(swapchain, s, value);
            phase = "DXGI Present";
            start = Now();
            Check(swapchain->Present(0, 0), "Present");
            ++presents;
            Maximum(costs.present, start);
            MSG msg;
            while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE))
            {
                TranslateMessage(&msg);
                DispatchMessageW(&msg);
            }
        }
    }
    Log("batch submitted CPU milliseconds=" + std::to_string(Now() - begin));
    costs.Report();
    phase = "final observation only; transport already submitted";
    // This observer barrier is after every handoff and Present. It cannot repair a missing
    // P/N/C/B edge or serialize the frames used to validate the transport.
    while (Value(c12) < Frames || (present && Value(b12) < Frames))
    {
        if (Value(p12) == UINT64_MAX || Value(n12) == UINT64_MAX || Value(c12) == UINT64_MAX ||
            Value(b12) == UINT64_MAX)
            Finish("FAIL", "device lost", 1);
        Sleep(1);
    }
    if (Value(c12) == UINT64_MAX || Value(n12) == UINT64_MAX || Value(b12) == UINT64_MAX)
        Finish("FAIL", "device lost", 1);
    UINT mismatches = 0;
    for (UINT f = 0; f < Frames; ++f)
    {
        const auto expected = Pattern(f);
        D3D11_MAPPED_SUBRESOURCE map {};
        Check(ctx->Map(snapshots[f], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &map), "completed snapshot Map");
        for (UINT y = 0; y < H; ++y)
        {
            const auto* row = static_cast<const uint8_t*>(map.pData) + y * map.RowPitch;
            for (UINT x = 0; x < W * 4; ++x)
                mismatches += row[x] != static_cast<uint8_t>(255 - expected[y * W * 4 + x]);
        }
        ctx->Unmap(snapshots[f], 0);
        Log("verified frame=" + std::to_string(f) + " cumulative byte mismatches=" + std::to_string(mismatches));
    }
    phase = "complete";
    Finish(mismatches ? "FAIL" : "PASS", mismatches ? "pixel mismatch" : "all frames exact", mismatches ? 1 : 0, Frames,
           mismatches);
}

bool ProductRecord(ID3D12GraphicsCommandList* list, ID3D12CommandQueue*,
                   const std::array<ID3D12Resource*, 4>& resources, void* opaque)
{
    const bool mutant = *static_cast<bool*>(opaque);
    D3D12_UNORDERED_ACCESS_VIEW_DESC uav {};
    uav.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    uav.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
    auto cpu = heap->GetCPUDescriptorHandleForHeapStart();
    cpu.ptr += increment; // existing root signature's u0 is descriptor 1
    dev12->CreateUnorderedAccessView(resources[0], nullptr, &uav, cpu);
    list->SetPipelineState(pipeline);
    list->SetComputeRootSignature(signature);
    list->SetDescriptorHeaps(1, &heap);
    list->SetComputeRootDescriptorTable(0, heap->GetGPUDescriptorHandleForHeapStart());
    if (!mutant)
        list->Dispatch((W + 7) / 8, (H + 7) / 8, 1);
    return true;
}
void Product(IDXGIAdapter* adapter, const std::string& mode)
{
    using DlssNr::Dx11Scene::Transport;
    auto* transport = new Transport; // Retained on any failure or timeout, like the production owner.
    const bool alias = mode == "product-alias";
    const bool missing = mode == "product-missing";
    if (!transport->Init(dev11, ctx, dev12, alias ? presentQueue : nrQueue, missing ? nullptr : presentQueue))
    {
        const std::string reason = transport->Reason();
        Log(reason);
        if (alias && reason == "Dx11SceneStage: SharedVkQueue")
            Finish("PASS", "product rejected SharedVkQueue before submission", 0);
        if (missing && reason == "Dx11SceneStage: QueueIdentityUnavailable")
            Finish("PASS", "product rejected missing queue evidence before submission", 0);
        Finish("SKIP", transport->Reason(), 0);
    }
    const auto evidence = transport->evidence;
    Log("product queues: NR/consumer same=" + std::to_string(evidence.nr == evidence.consumer) + " NR/Present same=" +
        std::to_string(evidence.nr == evidence.present) + " NR-family=" + std::to_string(evidence.nrFamily) +
        " consumer-family=" + std::to_string(evidence.consumerFamily) +
        " Present-family=" + std::to_string(evidence.presentFamily));
    if (alias || missing)
        Finish("FAIL", "product admitted unsafe topology", 1);
    Prepare();
    // Same descriptor/root layout as the experimental fixture, but an in-place shader on product-owned colour.
    const char* shader = "RWTexture2D<float4> dst:register(u0);"
                         "[numthreads(8,8,1)] void main(uint3 p:SV_DispatchThreadID){"
                         "if(p.x<61 && p.y<37) dst[p.xy]=1.0-dst[p.xy];}";
    ID3DBlob *bytecode = nullptr, *errors = nullptr;
    Check(D3DCompile(shader, std::strlen(shader), "product-fixture", nullptr, nullptr, "main", "cs_5_0", 0, 0,
                     &bytecode, &errors),
          "product shader compile");
    D3D12_COMPUTE_PIPELINE_STATE_DESC pso {};
    pso.pRootSignature = signature;
    pso.CS = { bytecode->GetBufferPointer(), bytecode->GetBufferSize() };
    Check(dev12->CreateComputePipelineState(&pso, IID_PPV_ARGS(&pipeline)), "product PSO");
    ID3D11Texture2D *depth = nullptr, *motion = nullptr;
    auto desc = TextureDesc();
    desc.Format = DXGI_FORMAT_R32_FLOAT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    Check(dev11->CreateTexture2D(&desc, nullptr, &depth), "product depth");
    desc.Format = DXGI_FORMAT_R16G16_FLOAT;
    Check(dev11->CreateTexture2D(&desc, nullptr, &motion), "product motion");
    auto* swapchain = Swapchain(adapter);
    bool mutant = mode == "product-mutant";
    std::thread(
        []
        {
            std::this_thread::sleep_for(std::chrono::seconds(12));
            Finish("STALL", "product watchdog expired", 3);
        })
        .detach();
    unsigned busy = 0;
    for (UINT f = 0; f < Frames;)
    {
        Slot& slot = slots[f % Slots];
        // Production admits bounded reuse by polling, never waiting. Busy attempts leave the game untouched.
        if (!transport->Ready())
        {
            ++busy;
            SwitchToThread();
            continue;
        }
        const UINT64 value = f + 1;
        if (f >= Slots)
            Check(ctx->Wait(b11, value - Slots), "product reuse Wait B");
        // Queue the Present dependency before production NR: this is the topology that aliased queues deadlock.
        PresentWork(swapchain, slot, value);
        auto pattern = Pattern(f);
        ctx->UpdateSubresource(slot.source, 0, nullptr, pattern.data(), W * 4, 0);
        const std::array<ID3D11Texture2D*, 4> inputs { slot.source, depth, motion, nullptr };
        phase = "production transport Run";
        const auto result = transport->Run(inputs, ProductRecord, &mutant);
        if (result != Transport::Result::Recorded)
            Finish("FAIL", transport->Reason(), 1);
        // Later D3D11 effects see the returned answer in the very same input texture.
        ctx->CopyResource(slot.consumer, slot.source);
        ctx->CopyResource(snapshots[f], slot.consumer);
        ctx->CopyResource(slot.output, slot.consumer);
        Check(ctx->Signal(c11, value), "product external consumer signal");
        ctx->Flush();
        submitted = ++f;
        Check(swapchain->Present(0, 0), "product Present");
        ++presents;
    }
    while (Value(c12) < Frames || Value(b12) < Frames || !transport->Ready())
        SwitchToThread(); // Final observation only, after every frame's handoff/Present.
    if (!transport->Retire())
        Finish("FAIL", "product retirement failed", 1);
    delete transport;
    Log("production transport: busy frames skipped=" + std::to_string(busy) + "; safe retirement observed");
    UINT mismatches = 0;
    for (UINT f = 0; f < Frames; ++f)
    {
        const auto expected = Pattern(f);
        D3D11_MAPPED_SUBRESOURCE map {};
        Check(ctx->Map(snapshots[f], 0, D3D11_MAP_READ, D3D11_MAP_FLAG_DO_NOT_WAIT, &map), "completed snapshot Map");
        for (UINT y = 0; y < H; ++y)
        {
            const auto* row = static_cast<const uint8_t*>(map.pData) + y * map.RowPitch;
            for (UINT x = 0; x < W * 4; ++x)
                mismatches += row[x] != static_cast<uint8_t>(255 - expected[y * W * 4 + x]);
        }
        ctx->Unmap(snapshots[f], 0);
        Log("verified frame=" + std::to_string(f) + " cumulative byte mismatches=" + std::to_string(mismatches));
    }
    phase = "complete";
    Finish(mismatches ? "FAIL" : "PASS", mismatches ? "pixel mismatch" : "all frames exact", mismatches ? 1 : 0, Frames,
           mismatches);
}
} // namespace

int main(int argc, char** argv)
{
    logFile = std::fopen("trace.log", "w");
    if (!logFile)
        return 1;
    const std::string mode = argc > 1 ? argv[1] : "no-present";
    Log("mode=" + mode);
    auto* adapter = Devices();
    if (mode == "probe")
        Probe();
    if (mode.rfind("product-", 0) == 0)
        Product(adapter, mode);
    Roundtrip(adapter, mode);
}

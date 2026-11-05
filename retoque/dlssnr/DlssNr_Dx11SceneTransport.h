#pragma once

#include "DlssNr_Dx11ScenePolicy.h"
#include <d3d11_4.h>
#include <d3d12.h>
#include <dxgi1_2.h>
#include <vulkan/vulkan_core.h>
#include <wrl/client.h>
#include <array>

namespace DlssNr::Dx11Scene
{
// Read-only ABI prefixes. DXVK src/dxgi/dxgi_interfaces.h and vkd3d-proton
// ID3D12DXVKInteropDevice (also declared in misc/IdentifyGpu.h). No Vulkan submission/locking.
MIDL_INTERFACE("e2ef5fa5-dc21-4af7-90c4-f67ef6a09323") ConsumerInterop : public IUnknown
{
    virtual void STDMETHODCALLTYPE GetVulkanHandles(VkInstance*, VkPhysicalDevice*, VkDevice*) = 0;
    virtual void STDMETHODCALLTYPE GetSubmissionQueue(VkQueue*, uint32_t*) = 0;
};
MIDL_INTERFACE("39da4e09-bd1c-4198-9fae-86bbe3be41fd") QueueInterop : public IUnknown
{
    virtual HRESULT STDMETHODCALLTYPE GetDXGIAdapter(REFIID, void**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetInstanceExtensions(UINT*, const char**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceExtensions(UINT*, const char**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetDeviceFeatures(const VkPhysicalDeviceFeatures2**) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetVulkanHandles(VkInstance*, VkPhysicalDevice*, VkDevice*) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetVulkanQueueInfo(ID3D12CommandQueue*, VkQueue*, UINT32*) = 0;
};

// One bounded generation. Callers serialize access, never destroy a poisoned/pending instance,
// and never create a replacement for it. No event, Map, Flush-wait, AcquireSync or CPU GPU wait.
// The renderer must track this command list's exact recording, execution and Reset independently.
class Transport
{
    template <class T> using Ptr = Microsoft::WRL::ComPtr<T>;
    Ptr<ID3D11Device5> d11;
    Ptr<ID3D11DeviceContext4> context;
    Ptr<ID3D12Device> d12;
    Ptr<ID3D12CommandQueue> queue;
    Ptr<ID3D12CommandQueue> presenter;
    Ptr<ID3D11Fence> p11, n11, c11;
    Ptr<ID3D12Fence> p12, n12, c12;
    Ptr<ID3D12CommandAllocator> allocator;
    Ptr<ID3D12GraphicsCommandList> list;
    std::array<Ptr<ID3D11Texture2D>, 4> shared;
    std::array<Ptr<ID3D12Resource>, 4> opened;
    std::array<D3D11_TEXTURE2D_DESC, 4> descriptions {};
    uint64_t serial = 0;
    bool poisoned = false;
    const char* reason = "Dx11SceneStage: Waiting";

    bool Fail(const char* why)
    {
        reason = why;
        return false;
    }
    bool Fence(bool producer, Ptr<ID3D11Fence>& f11, Ptr<ID3D12Fence>& f12)
    {
        HANDLE handle = nullptr;
        HRESULT hr;
        if (producer)
        {
            hr = d11->CreateFence(0, D3D11_FENCE_FLAG_SHARED, IID_PPV_ARGS(&f11));
            if (SUCCEEDED(hr))
                hr = f11->CreateSharedHandle(nullptr, GENERIC_ALL, nullptr, &handle);
            if (SUCCEEDED(hr))
                hr = d12->OpenSharedHandle(handle, IID_PPV_ARGS(&f12));
        }
        else
        {
            hr = d12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&f12));
            if (SUCCEEDED(hr))
                hr = d12->CreateSharedHandle(f12.Get(), nullptr, GENERIC_ALL, nullptr, &handle);
            if (SUCCEEDED(hr))
                hr = d11->OpenSharedFence(handle, IID_PPV_ARGS(&f11));
        }
        if (handle)
            CloseHandle(handle);
        return SUCCEEDED(hr);
    }
    static unsigned Bytes(DXGI_FORMAT format, unsigned index)
    {
        if (index == 0)
        {
            switch (format)
            {
            case DXGI_FORMAT_R16G16B16A16_FLOAT:
                return 8;
            case DXGI_FORMAT_R8G8B8A8_UNORM:
            case DXGI_FORMAT_R10G10B10A2_UNORM:
                return 4;
            default:
                return 0;
            }
        }
        switch (format)
        {
        case DXGI_FORMAT_R32_FLOAT:
            return 4;
        case DXGI_FORMAT_R16_FLOAT:
        case DXGI_FORMAT_R16_UNORM:
            return 2;
        case DXGI_FORMAT_R16G16_FLOAT:
            return 4;
        case DXGI_FORMAT_R32G32_FLOAT:
            return 8;
        default:
            return 0;
        }
    }
    static DXGI_FORMAT CopyFormat(DXGI_FORMAT format)
    {
        // SRV-only depth copies: sharing a depth-stencil allocation is not visible reliably on DXVK.
        if (format == DXGI_FORMAT_R32_TYPELESS || format == DXGI_FORMAT_D32_FLOAT)
            return DXGI_FORMAT_R32_FLOAT;
        if (format == DXGI_FORMAT_R16_TYPELESS || format == DXGI_FORMAT_D16_UNORM)
            return DXGI_FORMAT_R16_UNORM;
        return format;
    }
    bool Textures(const std::array<ID3D11Texture2D*, 4>& inputs)
    {
        std::array<D3D11_TEXTURE2D_DESC, 4> next {};
        uint64_t bytes = 0;
        bool changed = false;
        for (unsigned i = 0; i < next.size(); ++i)
        {
            if (!inputs[i])
            {
                if (i < 3)
                    return Fail("Dx11SceneStage: MissingGuides");
            }
            else
            {
                Ptr<ID3D11Device> owner;
                inputs[i]->GetDevice(&owner);
                if (owner.Get() != d11.Get())
                    return Fail("Dx11SceneStage: InputDeviceChanged");
                inputs[i]->GetDesc(&next[i]);
                auto& desc = next[i];
                desc.Format = CopyFormat(desc.Format);
                const auto pixel = Bytes(desc.Format, i);
                if (!pixel || !desc.Width || !desc.Height || desc.MipLevels != 1 || desc.ArraySize != 1 ||
                    desc.SampleDesc.Count != 1 || desc.Width > 16384 || desc.Height > 16384)
                    return Fail("Dx11SceneStage: UnsupportedTexture");
                bytes += uint64_t(desc.Width) * desc.Height * pixel;
                desc.Usage = D3D11_USAGE_DEFAULT;
                desc.CPUAccessFlags = 0;
                desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | (i == 0 ? D3D11_BIND_UNORDERED_ACCESS : 0);
                desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
            }
            changed |= next[i].Width != descriptions[i].Width || next[i].Height != descriptions[i].Height ||
                       next[i].Format != descriptions[i].Format;
        }
        // Charge both API views conservatively; no second live/retired generation is permitted.
        if (bytes > MaxBytes / 2)
            return Fail("Dx11SceneStage: MemoryLimit");
        if (!changed)
            return true;
        for (unsigned i = 0; i < next.size(); ++i)
        {
            opened[i].Reset();
            shared[i].Reset();
            if (!next[i].Width)
                continue;
            if (FAILED(d11->CreateTexture2D(&next[i], nullptr, &shared[i])))
                return Fail("Dx11SceneStage: SharedTextureUnavailable");
            Ptr<IDXGIResource1> resource;
            HANDLE handle = nullptr;
            HRESULT hr = shared[i].As(&resource);
            if (SUCCEEDED(hr))
                hr = resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE,
                                                  nullptr, &handle);
            if (SUCCEEDED(hr))
                hr = d12->OpenSharedHandle(handle, IID_PPV_ARGS(&opened[i]));
            if (handle)
                CloseHandle(handle);
            if (FAILED(hr))
                return Fail("Dx11SceneStage: SharedTextureImportUnavailable");
        }
        descriptions = next;
        return true;
    }
    void Barriers(bool begin)
    {
        for (unsigned i = 0; i < opened.size(); ++i)
        {
            if (!opened[i])
                continue;
            D3D12_RESOURCE_BARRIER b {};
            b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            b.Transition.pResource = opened[i].Get();
            b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            const auto working =
                i == 0 ? D3D12_RESOURCE_STATE_UNORDERED_ACCESS : D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            b.Transition.StateBefore = begin ? D3D12_RESOURCE_STATE_COMMON : working;
            b.Transition.StateAfter = begin ? working : D3D12_RESOURCE_STATE_COMMON;
            list->ResourceBarrier(1, &b);
        }
    }

  public:
    struct QueueEvidence
    {
        uintptr_t nr = 0, consumer = 0, present = 0;
        uint32_t nrFamily = 0, consumerFamily = 0, presentFamily = 0;
    };
    QueueEvidence evidence {};
    using Record = bool (*)(ID3D12GraphicsCommandList*, ID3D12CommandQueue*, const std::array<ID3D12Resource*, 4>&,
                            void*);
    enum class Result
    {
        Recorded,
        Passthrough,
        Busy,
        Failed
    };
    const char* Reason() const { return reason; }
    bool Ready() const
    {
        return !poisoned && (!serial || (c12 && Complete(c12->GetCompletedValue(), serial) &&
                                         Complete(n12->GetCompletedValue(), serial)));
    }
    // Queue identity is checked before allocation and before every submission, including after a bridge change.
    bool Queues(ID3D11Device* producer, ID3D12CommandQueue* present)
    {
        Ptr<QueueInterop> interop;
        Ptr<ConsumerInterop> consumer;
        VkQueue nr = VK_NULL_HANDLE, game = VK_NULL_HANDLE, display = VK_NULL_HANDLE;

        if (!present || FAILED(d12.As(&interop)) || FAILED(producer->QueryInterface(IID_PPV_ARGS(&consumer))))
            return Fail("Dx11SceneStage: QueueIdentityUnavailable");
        Ptr<ID3D12Device> nrDevice, presentDevice;
        if (FAILED(queue->GetDevice(IID_PPV_ARGS(&nrDevice))) ||
            FAILED(present->GetDevice(IID_PPV_ARGS(&presentDevice))) || nrDevice.Get() != d12.Get() ||
            presentDevice.Get() != d12.Get())
            return Fail("Dx11SceneStage: QueueDeviceMismatch");
        consumer->GetSubmissionQueue(&game, &evidence.consumerFamily);
        if (FAILED(interop->GetVulkanQueueInfo(queue.Get(), &nr, &evidence.nrFamily)) ||
            FAILED(interop->GetVulkanQueueInfo(present, &display, &evidence.presentFamily)))
            return Fail("Dx11SceneStage: QueueIdentityUnavailable");
        evidence.nr = reinterpret_cast<uintptr_t>(nr);
        evidence.consumer = reinterpret_cast<uintptr_t>(game);
        evidence.present = reinterpret_cast<uintptr_t>(display);
        if (auto why = QueueRefusal(reinterpret_cast<uintptr_t>(nr), reinterpret_cast<uintptr_t>(game),
                                    reinterpret_cast<uintptr_t>(display)))
            return Fail(why);
        return true;
    }
    bool Init(ID3D11Device* producer, ID3D11DeviceContext* immediate, ID3D12Device* device, ID3D12CommandQueue* nr,
              ID3D12CommandQueue* present)
    {
        d12 = device;
        queue = nr;
        presenter = present;
        if (!producer || !immediate || !device || !nr || immediate->GetType() != D3D11_DEVICE_CONTEXT_IMMEDIATE ||
            FAILED(producer->QueryInterface(IID_PPV_ARGS(&d11))) ||
            FAILED(immediate->QueryInterface(IID_PPV_ARGS(&context))))
            return Fail("Dx11SceneStage: SharedFenceInterfacesUnavailable");
        if (!Queues(producer, present))
            return false;
        if (!Fence(true, p11, p12) || !Fence(false, n11, n12) || !Fence(true, c11, c12))
            return Fail("Dx11SceneStage: SharedFencesUnavailable");
        if (FAILED(d12->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocator))) ||
            FAILED(d12->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocator.Get(), nullptr,
                                          IID_PPV_ARGS(&list))) ||
            FAILED(list->Close()))
            return Fail("Dx11SceneStage: CommandObjectsUnavailable");
        return true;
    }
    Result Run(const std::array<ID3D11Texture2D*, 4>& inputs, Record record, void* data)
    {
        if (poisoned)
            return Result::Failed;
        if (FAILED(d11->GetDeviceRemovedReason()) || FAILED(d12->GetDeviceRemovedReason()))
        {
            poisoned = true;
            Fail("Dx11SceneStage: DeviceLostRetained");
            return Result::Failed;
        }
        if (!Ready())
        {
            reason = "Dx11SceneStage: ConsumerInFlight";
            return Result::Busy;
        }
        if (!Queues(d11.Get(), presenter.Get()) || !Textures(inputs))
            return Result::Failed;
        if (FAILED(allocator->Reset()) || FAILED(list->Reset(allocator.Get(), nullptr)))
        {
            Fail("Dx11SceneStage: ResetFailed");
            return Result::Failed;
        }
        // Record before creating any GPU dependency. Even a declined callback can have recorded model creation;
        // close/execute it and seal its exact recording, but never copy a declined answer to the game.
        Barriers(true);
        std::array<ID3D12Resource*, 4> resources {};
        for (unsigned i = 0; i < resources.size(); ++i)
            resources[i] = opened[i].Get();
        const bool recorded = record(list.Get(), queue.Get(), resources, data);
        Barriers(false);
        if (FAILED(list->Close()))
        {
            poisoned = true; // unsealed renderer recording must remain owned, even without a submission
            Fail("Dx11SceneStage: CloseFailedRetained");
            return Result::Failed;
        }
        ++serial;
        poisoned = true; // any partial submission or missing signal retains all resources for process lifetime
        for (unsigned i = 0; i < inputs.size(); ++i)
            if (inputs[i])
                context->CopyResource(shared[i].Get(), inputs[i]);
        if (FAILED(context->Signal(p11.Get(), serial)))
        {
            Fail("Dx11SceneStage: ProducerSignalFailedRetained");
            return Result::Failed;
        }
        context->Flush();
        if (FAILED(queue->Wait(p12.Get(), serial)))
        {
            Fail("Dx11SceneStage: ProducerWaitFailedRetained");
            return Result::Failed;
        }
        ID3D12CommandList* commands[] = { list.Get() };
        queue->ExecuteCommandLists(1, commands);
        if (FAILED(queue->Signal(n12.Get(), serial)))
        {
            Fail("Dx11SceneStage: NrSignalFailedRetained");
            return Result::Failed;
        }
        // No future CPU submission is needed to satisfy this GPU wait: NR and its signal are already submitted.
        if (FAILED(context->Wait(n11.Get(), serial)))
        {
            Fail("Dx11SceneStage: ConsumerWaitFailedRetained");
            return Result::Failed;
        }
        if (recorded)
            context->CopyResource(inputs[0], shared[0].Get());
        if (FAILED(context->Signal(c11.Get(), serial)))
        {
            Fail("Dx11SceneStage: ConsumerSignalFailedRetained");
            return Result::Failed;
        }
        context->Flush();
        poisoned = false;
        reason = recorded ? "Dx11SceneStage: PostSrSubmitted" : "Dx11SceneStage: ModelDeclined";
        return recorded ? Result::Recorded : Result::Passthrough;
    }
    // After C completes, Reset ends the renderer's last recording lifetime before safe destruction.
    bool Retire()
    {
        if (!Ready())
            return false;
        if (list &&
            (FAILED(allocator->Reset()) || FAILED(list->Reset(allocator.Get(), nullptr)) || FAILED(list->Close())))
        {
            poisoned = true;
            return false;
        }
        return true;
    }
};
} // namespace DlssNr::Dx11Scene

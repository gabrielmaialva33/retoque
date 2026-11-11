#include "pch.h"

#include "Transport.h"

namespace RetoqueWindowHost
{
namespace
{
std::string Failure(const char* what, HRESULT hr)
{
    char text[64];
    snprintf(text, sizeof(text), " (HRESULT 0x%08X)", static_cast<unsigned>(hr));
    return std::string(what) + text;
}

bool ShareFence(ID3D12Device* d3d12, ID3D11Device5* d3d11, ComPtr<ID3D12Fence>& fence12, ComPtr<ID3D11Fence>& fence11,
                std::string& why)
{
    HRESULT hr = d3d12->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence12));
    HANDLE handle = nullptr;
    if (SUCCEEDED(hr))
        hr = d3d12->CreateSharedHandle(fence12.Get(), nullptr, GENERIC_ALL, nullptr, &handle);
    if (SUCCEEDED(hr))
    {
        hr = d3d11->OpenSharedFence(handle, IID_PPV_ARGS(&fence11));
        CloseHandle(handle);
    }
    if (FAILED(hr))
        why = Failure("a fence could not be shared between D3D12 and D3D11", hr);
    return SUCCEEDED(hr);
}
} // namespace

bool Transport::Create(ID3D11Device5* d3d11, ID3D11DeviceContext4* context, ID3D12Device* d3d12, std::string& why)
{
    d3d11_ = d3d11;
    context_ = context;
    d3d12_ = d3d12;
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (event_ == nullptr)
    {
        why = "no event for the transport's fences";
        return false;
    }
    return ShareFence(d3d12, d3d11, captured12_, captured11_, why) &&
           ShareFence(d3d12, d3d11, consumed12_, consumed11_, why);
}

bool Transport::EnsureExtent(uint32_t width, uint32_t height, std::string& why)
{
    if (width == width_ && height == height_ && slots11_[0] != nullptr)
        return true;
    for (auto& slot : slots12_)
        slot.Reset();
    for (auto& slot : slots11_)
        slot.Reset();
    width_ = height_ = 0;

    D3D11_TEXTURE2D_DESC desc {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    for (uint64_t i = 0; i < kSlots; ++i)
    {
        HRESULT hr = d3d11_->CreateTexture2D(&desc, nullptr, &slots11_[i]);
        ComPtr<IDXGIResource1> shared;
        HANDLE handle = nullptr;
        if (SUCCEEDED(hr))
            hr = slots11_[i].As(&shared);
        if (SUCCEEDED(hr))
            hr = shared->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr,
                                            &handle);
        if (SUCCEEDED(hr))
        {
            hr = d3d12_->OpenSharedHandle(handle, IID_PPV_ARGS(&slots12_[i]));
            CloseHandle(handle);
        }
        if (FAILED(hr))
        {
            why = Failure("a shared texture for the captured frame could not be created", hr);
            return false;
        }
    }
    width_ = width;
    height_ = height;
    return true;
}

uint64_t Transport::Submit(ID3D11Texture2D* source, const Geometry::Box& box)
{
    const uint64_t serial = ++serial_;
    // The slot's previous frame must have been read by D3D12 first: a GPU wait, never a CPU one.
    if (serial > kSlots)
        context_->Wait(consumed11_.Get(), serial - kSlots);
    const D3D11_BOX region { box.left, box.top, 0, box.right, box.bottom, 1 };
    context_->CopySubresourceRegion(slots11_[serial % kSlots].Get(), 0, 0, 0, 0, source, 0, &region);
    context_->Signal(captured11_.Get(), serial);
    context_->Flush();
    return serial;
}

bool Transport::WaitIdle(DWORD milliseconds)
{
    if (serial_ == 0)
        return true;
    for (ID3D12Fence* fence : { captured12_.Get(), consumed12_.Get() })
    {
        if (fence->GetCompletedValue() >= serial_)
            continue;
        if (FAILED(fence->SetEventOnCompletion(serial_, event_)) ||
            WaitForSingleObject(event_, milliseconds) != WAIT_OBJECT_0 || fence->GetCompletedValue() < serial_)
            return false;
    }
    return true;
}

void Transport::Abandon()
{
    for (auto& slot : slots12_)
        (void) slot.Detach();
    for (auto& slot : slots11_)
        (void) slot.Detach();
    (void) captured12_.Detach();
    (void) consumed12_.Detach();
    (void) captured11_.Detach();
    (void) consumed11_.Detach();
}
} // namespace RetoqueWindowHost

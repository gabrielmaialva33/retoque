#include "pch.h"

// Windows Graphics Capture of one window. The mechanics -- a free-threaded pool whose FrameArrived only signals the
// owner's wake event, the newest frame winning, the pool recreated when the content size changes, cursor and border
// settings set only where the API has them -- are re-derived from NeuRotic beae4eea
// (apps/NeuRoticWindowWorker/capture/CaptureHost.cpp), which adapted Magpie 69d6105d; both GPL-3.0, see
// Licenses/NRAnything_ATTRIBUTION.txt and Licenses/Magpie-LICENSE.txt.
//
// Unlike NeuRotic, a polled frame is not drained on the CPU before it goes back to the pool: it is held until the
// host's D3D11 fence shows that the copy which read it completed (Hold/Retire), and only then closed.

#include "Capture.h"

#include <windows.graphics.capture.interop.h>
#include <windows.graphics.directx.direct3d11.interop.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Foundation.Metadata.h>
#include <winrt/Windows.Graphics.Capture.h>
#include <winrt/Windows.Graphics.DirectX.Direct3D11.h>

#include <atomic>
#include <cstdio>
#include <deque>

namespace RetoqueWindowHost
{
namespace
{
using winrt::Windows::Foundation::Metadata::ApiInformation;
using winrt::Windows::Graphics::SizeInt32;
using winrt::Windows::Graphics::Capture::Direct3D11CaptureFrame;
using winrt::Windows::Graphics::Capture::Direct3D11CaptureFramePool;
using winrt::Windows::Graphics::Capture::GraphicsCaptureItem;
using winrt::Windows::Graphics::Capture::GraphicsCaptureSession;
using winrt::Windows::Graphics::DirectX::DirectXPixelFormat;
using winrt::Windows::Graphics::DirectX::Direct3D11::IDirect3DDevice;

// Three surfaces: one being copied, one held until that copy's fence completes, one free for the compositor.
constexpr int32_t kPoolBuffers = 3;
constexpr DirectXPixelFormat kFormat = DirectXPixelFormat::B8G8R8A8UIntNormalized;

std::string Failure(const char* what, HRESULT hr)
{
    char text[64];
    snprintf(text, sizeof(text), " (HRESULT 0x%08X)", static_cast<unsigned>(hr));
    return std::string(what) + text;
}
} // namespace

struct Capture::Impl
{
    IDirect3DDevice device { nullptr };
    GraphicsCaptureItem item { nullptr };
    Direct3D11CaptureFramePool pool { nullptr };
    GraphicsCaptureSession session { nullptr };
    winrt::event_token frameToken {}, closedToken {};
    bool frameSubscribed = false, closedSubscribed = false;
    std::shared_ptr<std::atomic<bool>> closed = std::make_shared<std::atomic<bool>>(false);
    SizeInt32 extent {};
    uint64_t nextToken = 1;

    struct Polled
    {
        uint64_t token = 0;
        Direct3D11CaptureFrame frame { nullptr };
        ComPtr<ID3D11Texture2D> texture;
        bool held = false;
        uint64_t fence = 0;
    };
    std::deque<Polled> frames;

    // A frame polled but never handed to the GPU goes straight back.
    void CloseUnheld()
    {
        for (auto it = frames.begin(); it != frames.end();)
        {
            if (!it->held)
            {
                it->texture.Reset();
                it->frame.Close();
                it = frames.erase(it);
            }
            else
                ++it;
        }
    }
};

Capture::Capture() : impl_(std::make_unique<Impl>()) {}

Capture::~Capture()
{
    if (impl_ != nullptr)
        Stop();
}

bool Capture::Supported(std::string& why)
{
    try
    {
        if (GraphicsCaptureSession::IsSupported())
            return true;
        why = "Windows Graphics Capture is not supported here (Windows 10 2004 or later is needed)";
    }
    catch (const winrt::hresult_error& e)
    {
        why = Failure("Windows Graphics Capture could not be queried", e.code());
    }
    return false;
}

bool Capture::Start(HWND window, ID3D11Device* device, bool cursor, HANDLE wake, std::string& why)
{
    Impl& p = *impl_;
    const char* phase = "the D3D11 device for capture";
    try
    {
        ComPtr<IDXGIDevice> dxgi;
        winrt::check_hresult(device->QueryInterface(IID_PPV_ARGS(&dxgi)));
        winrt::com_ptr<::IInspectable> inspectable;
        winrt::check_hresult(CreateDirect3D11DeviceFromDXGIDevice(dxgi.Get(), inspectable.put()));
        p.device = inspectable.as<IDirect3DDevice>();

        phase = "the capture item for the window";
        auto interop = winrt::get_activation_factory<GraphicsCaptureItem, IGraphicsCaptureItemInterop>();
        winrt::check_hresult(
            interop->CreateForWindow(window, winrt::guid_of<GraphicsCaptureItem>(), winrt::put_abi(p.item)));
        p.extent = p.item.Size();
        if (p.extent.Width <= 0 || p.extent.Height <= 0)
        {
            why = "the window's capture is empty";
            return false;
        }

        phase = "the frame pool";
        p.pool = Direct3D11CaptureFramePool::CreateFreeThreaded(p.device, kFormat, kPoolBuffers, p.extent);
        // The handler only wakes the owner thread; the copy happens there. The wake event outlives the process's
        // capture (Main never closes it), so a callback racing the revoke below still signals a valid handle.
        p.frameToken = p.pool.FrameArrived([wake](auto&&, auto&&) { SetEvent(wake); });
        p.frameSubscribed = true;
        p.closedToken = p.item.Closed(
            [closed = p.closed, wake](auto&&, auto&&)
            {
                closed->store(true, std::memory_order_release);
                SetEvent(wake);
            });
        p.closedSubscribed = true;

        phase = "the capture session";
        p.session = p.pool.CreateCaptureSession(p.item);
        const auto session = winrt::name_of<GraphicsCaptureSession>();
        if (ApiInformation::IsPropertyPresent(session, L"IsCursorCaptureEnabled"))
            p.session.IsCursorCaptureEnabled(cursor);
        if (ApiInformation::IsPropertyPresent(session, L"IsBorderRequired"))
        {
            // Windows 11 draws a yellow border around a captured window unless the app may opt out; an app that may
            // not keeps the border, which is only cosmetic.
            try
            {
                p.session.IsBorderRequired(false);
            }
            catch (const winrt::hresult_error&)
            {
            }
        }
        phase = "StartCapture";
        p.session.StartCapture();
        return true;
    }
    catch (const winrt::hresult_error& e)
    {
        why = Failure((std::string("Windows Graphics Capture failed at ") + phase).c_str(), e.code());
    }
    return false;
}

bool Capture::Poll(CapturedFrame& out, std::string& why)
{
    Impl& p = *impl_;
    out = {};
    if (p.pool == nullptr)
        return false;
    p.CloseUnheld();
    try
    {
        Direct3D11CaptureFrame frame = p.pool.TryGetNextFrame();
        if (frame == nullptr)
            return false;
        while (Direct3D11CaptureFrame newer = p.pool.TryGetNextFrame())
        {
            frame.Close();
            frame = std::move(newer);
            ++out.superseded;
        }
        const SizeInt32 size = frame.ContentSize();
        if (size.Width <= 0 || size.Height <= 0)
        {
            frame.Close();
            return false;
        }
        if (size.Width != p.extent.Width || size.Height != p.extent.Height)
        {
            // Frames still held keep their own surfaces alive through their texture references.
            frame.Close();
            p.pool.Recreate(p.device, kFormat, kPoolBuffers, size);
            p.extent = size;
            return false;
        }
        auto access = frame.Surface().as<::Windows::Graphics::DirectX::Direct3D11::IDirect3DDxgiInterfaceAccess>();
        Impl::Polled polled;
        winrt::check_hresult(access->GetInterface(IID_PPV_ARGS(&polled.texture)));
        polled.token = p.nextToken++;
        out.texture = polled.texture.Get();
        out.content = { size.Width, size.Height };
        out.time = frame.SystemRelativeTime().count();
        out.token = polled.token;
        polled.frame = std::move(frame);
        p.frames.push_back(std::move(polled));
        return true;
    }
    catch (const winrt::hresult_error& e)
    {
        why = Failure("a captured frame could not be read", e.code());
    }
    return false;
}

void Capture::Hold(uint64_t token, uint64_t fenceValue)
{
    for (auto& polled : impl_->frames)
        if (polled.token == token)
        {
            polled.held = true;
            polled.fence = fenceValue;
        }
}

void Capture::Retire(uint64_t completedFenceValue)
{
    auto& frames = impl_->frames;
    for (auto it = frames.begin(); it != frames.end();)
    {
        if (it->held && it->fence <= completedFenceValue)
        {
            it->texture.Reset();
            it->frame.Close();
            it = frames.erase(it);
        }
        else
            ++it;
    }
}

size_t Capture::Held() const
{
    size_t held = 0;
    for (const auto& polled : impl_->frames)
        held += polled.held ? 1 : 0;
    return held;
}

bool Capture::ItemClosed() const { return impl_->closed->load(std::memory_order_acquire); }

void Capture::Stop()
{
    Impl& p = *impl_;
    try
    {
        if (p.frameSubscribed)
            p.pool.FrameArrived(p.frameToken);
        if (p.closedSubscribed)
            p.item.Closed(p.closedToken);
        p.frameSubscribed = p.closedSubscribed = false;
        p.CloseUnheld();
        if (p.session != nullptr)
            p.session.Close();
        p.session = nullptr;
        // Held frames keep their surfaces (texture references) until Retire or Abandon; closing the pool stops new
        // frames only.
        if (p.pool != nullptr)
            p.pool.Close();
        p.pool = nullptr;
    }
    catch (const winrt::hresult_error&)
    {
    }
}

void Capture::Abandon()
{
    // Completion of the copies that read the held frames could not be proved: nothing here is released again.
    (void) impl_.release();
}
} // namespace RetoqueWindowHost

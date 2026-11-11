#include "pch.h"

#include "Output.h"
#include "Policy.h"

namespace RetoqueWindowHost
{
namespace
{
constexpr wchar_t kClass[] = L"RetoqueWindowHostPreview";
static_assert(sizeof(kClass) / sizeof(wchar_t) == sizeof(Policy::kPreviewClass), "one preview class name");
constexpr DWORD kListWaitMs = 2000;
constexpr UINT kSwapFlags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;

std::string Failure(const char* what, HRESULT hr)
{
    char text[64];
    snprintf(text, sizeof(text), " (HRESULT 0x%08X)", static_cast<unsigned>(hr));
    return std::string(what) + text;
}

void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES from,
                D3D12_RESOURCE_STATES to)
{
    D3D12_RESOURCE_BARRIER barrier {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = from;
    barrier.Transition.StateAfter = to;
    list->ResourceBarrier(1, &barrier);
}
} // namespace

LRESULT CALLBACK Output::WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
    auto* self = reinterpret_cast<Output*>(GetWindowLongPtrW(window, GWLP_USERDATA));
    switch (message)
    {
    case WM_CLOSE:
        // Closing the preview ends the session; the window itself lives until the process exits.
        if (self != nullptr)
            self->closeRequested_ = true;
        return 0;
    case WM_KEYDOWN:
        if (wparam == VK_ESCAPE && self != nullptr)
            self->closeRequested_ = true;
        return 0;
    case WM_ERASEBKGND:
        return 1;
    default:
        return DefWindowProcW(window, message, wparam, lparam);
    }
}

bool Output::Create(IDXGIFactory2* factory, ID3D12Device* device, ID3D12CommandQueue* queue, uint32_t width,
                    uint32_t height, const std::wstring& title, std::string& why)
{
    device_ = device;
    queue_ = queue;
    width_ = width;
    height_ = height;

    WNDCLASSEXW wc { sizeof(wc) };
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH));
    wc.lpszClassName = kClass;
    if (RegisterClassExW(&wc) == 0 && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
    {
        why = "the preview window class could not be registered";
        return false;
    }

    // Start at the source's size, no larger than the work area under the cursor.
    RECT frame { 0, 0, LONG(width), LONG(height) };
    AdjustWindowRectEx(&frame, WS_OVERLAPPEDWINDOW, FALSE, 0);
    POINT cursor {};
    GetCursorPos(&cursor);
    MONITORINFO monitor { sizeof(monitor) };
    LONG maxWidth = frame.right - frame.left, maxHeight = frame.bottom - frame.top;
    if (GetMonitorInfoW(MonitorFromPoint(cursor, MONITOR_DEFAULTTOPRIMARY), &monitor))
    {
        maxWidth = std::min<LONG>(maxWidth, monitor.rcWork.right - monitor.rcWork.left);
        maxHeight = std::min<LONG>(maxHeight, monitor.rcWork.bottom - monitor.rcWork.top);
    }
    window_ = CreateWindowExW(0, kClass, title.c_str(), WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, maxWidth,
                              maxHeight, nullptr, nullptr, wc.hInstance, nullptr);
    if (window_ == nullptr)
    {
        why = "the preview window could not be created";
        return false;
    }
    SetWindowLongPtrW(window_, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(this));

    DXGI_SWAP_CHAIN_DESC1 desc {};
    desc.Width = width;
    desc.Height = height;
    desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM; // what WGC captures for an SDR window
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = kBuffers;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    desc.Flags = kSwapFlags;
    ComPtr<IDXGISwapChain1> swapchain;
    HRESULT hr = factory->CreateSwapChainForHwnd(queue, window_, &desc, nullptr, nullptr, &swapchain);
    if (FAILED(hr) || FAILED(hr = swapchain.As(&swapchain_)))
    {
        why = Failure("the preview swapchain could not be created", hr);
        return false;
    }
    factory->MakeWindowAssociation(window_, DXGI_MWA_NO_ALT_ENTER);
    swapchain_->SetMaximumFrameLatency(2);
    latency_ = swapchain_->GetFrameLatencyWaitableObject();

    for (UINT i = 0; i < kLists; ++i)
    {
        hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&allocators_[i]));
        if (SUCCEEDED(hr))
            hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, allocators_[i].Get(), nullptr,
                                           IID_PPV_ARGS(&lists_[i]));
        if (SUCCEEDED(hr))
            hr = lists_[i]->Close();
        if (FAILED(hr))
        {
            why = Failure("the preview's command lists could not be created", hr);
            return false;
        }
    }
    hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_));
    event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (FAILED(hr) || event_ == nullptr)
    {
        why = Failure("the preview's fence could not be created", hr);
        return false;
    }
    ShowWindow(window_, SW_SHOWNOACTIVATE); // the source keeps the keyboard
    return true;
}

bool Output::FrameReady(DWORD milliseconds) const
{
    return latency_ == nullptr || WaitForSingleObjectEx(latency_, milliseconds, TRUE) == WAIT_OBJECT_0;
}

bool Output::Reusable(UINT index, std::string& why)
{
    if (fence_->GetCompletedValue() >= listFence_[index])
        return true;
    if (SUCCEEDED(fence_->SetEventOnCompletion(listFence_[index], event_)) &&
        WaitForSingleObject(event_, kListWaitMs) == WAIT_OBJECT_0 && fence_->GetCompletedValue() >= listFence_[index])
        return true;
    why = "a preview command list did not complete within 2 s";
    return false;
}

bool Output::Resize(uint32_t width, uint32_t height, std::string& why)
{
    const HRESULT hr = swapchain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, kSwapFlags);
    if (FAILED(hr))
    {
        why = Failure("the preview swapchain could not be resized", hr);
        return false;
    }
    width_ = width;
    height_ = height;
    return true;
}

bool Output::Consume(ID3D12Resource* slot, ID3D12Fence* captured, ID3D12Fence* consumed, uint64_t serial, bool copy,
                     std::string& why)
{
    HRESULT hr = queue_->Wait(captured, serial);
    if (FAILED(hr))
    {
        why = Failure("the queue could not wait for the captured frame", hr);
        return false;
    }
    if (copy)
    {
        const UINT index = UINT(frame_++ % kLists);
        if (!Reusable(index, why))
            return false;
        ComPtr<ID3D12Resource> backbuffer;
        hr = swapchain_->GetBuffer(swapchain_->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&backbuffer));
        if (SUCCEEDED(hr))
            hr = allocators_[index]->Reset();
        if (SUCCEEDED(hr))
            hr = lists_[index]->Reset(allocators_[index].Get(), nullptr);
        if (FAILED(hr))
        {
            why = Failure("the preview's copy could not be recorded", hr);
            return false;
        }
        ID3D12GraphicsCommandList* list = lists_[index].Get();
        // The slot is shared with D3D11, so it rests in COMMON between the two APIs.
        Transition(list, backbuffer.Get(), D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
        Transition(list, slot, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        list->CopyResource(backbuffer.Get(), slot);
        Transition(list, slot, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        Transition(list, backbuffer.Get(), D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
        hr = list->Close();
        if (FAILED(hr))
        {
            why = Failure("the preview's copy could not be closed", hr);
            return false;
        }
        ID3D12CommandList* lists[] = { list };
        queue_->ExecuteCommandLists(1, lists);
        listFence_[index] = ++fenceValue_;
        queue_->Signal(fence_.Get(), fenceValue_);
    }
    // After the copy on the same queue: D3D11 may write this slot again once this completes.
    hr = queue_->Signal(consumed, serial);
    if (FAILED(hr))
    {
        why = Failure("the queue could not release the captured frame", hr);
        return false;
    }
    return true;
}

HRESULT Output::Present(bool vsync) { return swapchain_->Present(vsync ? 1 : 0, 0); }

bool Output::Drain(DWORD milliseconds)
{
    if (queue_ == nullptr || fence_ == nullptr)
        return true;
    ++fenceValue_;
    if (FAILED(queue_->Signal(fence_.Get(), fenceValue_)))
        return false;
    if (fence_->GetCompletedValue() >= fenceValue_)
        return true;
    return SUCCEEDED(fence_->SetEventOnCompletion(fenceValue_, event_)) &&
           WaitForSingleObject(event_, milliseconds) == WAIT_OBJECT_0 && fence_->GetCompletedValue() >= fenceValue_;
}

void Output::Abandon()
{
    for (auto& list : lists_)
        (void) list.Detach();
    for (auto& allocator : allocators_)
        (void) allocator.Detach();
    (void) swapchain_.Detach();
    (void) fence_.Detach();
}
} // namespace RetoqueWindowHost

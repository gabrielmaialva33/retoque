// The preview: an ordinary window with a D3D12 flip-model HWND swapchain the size of the source's client area,
// stretched to whatever size the user gives the window. Each frame copies the transported slot into the backbuffer;
// the NR pass then runs on that backbuffer through the seam, before Present, exactly where a game's Present would
// run it. (The click-through overlay over the source is phase 3.)
#pragma once

#include <array>
#include <string>

namespace RetoqueWindowHost
{
class Output
{
  public:
    static constexpr UINT kBuffers = 3;
    static constexpr UINT kLists = 3;

    bool Create(IDXGIFactory2* factory, ID3D12Device* device, ID3D12CommandQueue* queue, uint32_t width,
                uint32_t height, const std::wstring& title, std::string& why);

    HWND Window() const { return window_; }
    bool CloseRequested() const { return closeRequested_; }
    IDXGISwapChain3* Swapchain() const { return swapchain_.Get(); }
    uint32_t Width() const { return width_; }
    uint32_t Height() const { return height_; }

    // The swapchain's frame-latency object: true when a frame may be started now.
    bool FrameReady(DWORD milliseconds) const;

    // Only after Drain and the transport's WaitIdle: no list and no copy may still touch the buffers.
    bool Resize(uint32_t width, uint32_t height, std::string& why);

    // On the queue: wait until D3D11 wrote serial's slot, copy it into the backbuffer (when copy is true and the
    // extents match), and signal that the slot was read. Every serial is consumed exactly once, copied or not.
    bool Consume(ID3D12Resource* slot, ID3D12Fence* captured, ID3D12Fence* consumed, uint64_t serial, bool copy,
                 std::string& why);

    // Present, then mark this frame's command list reusable once the queue passes it.
    HRESULT Present(bool vsync);

    // CPU: every list submitted to the queue completed, within the timeout.
    bool Drain(DWORD milliseconds);

    void Abandon();

  private:
    static LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    bool Reusable(UINT index, std::string& why);

    ID3D12Device* device_ = nullptr;
    ID3D12CommandQueue* queue_ = nullptr;
    HWND window_ = nullptr;
    bool closeRequested_ = false;
    ComPtr<IDXGISwapChain3> swapchain_;
    HANDLE latency_ = nullptr;
    std::array<ComPtr<ID3D12CommandAllocator>, kLists> allocators_;
    std::array<ComPtr<ID3D12GraphicsCommandList>, kLists> lists_;
    std::array<uint64_t, kLists> listFence_ {};
    ComPtr<ID3D12Fence> fence_;
    uint64_t fenceValue_ = 0;
    uint64_t frame_ = 0;
    HANDLE event_ = nullptr;
    uint32_t width_ = 0, height_ = 0;
};
} // namespace RetoqueWindowHost

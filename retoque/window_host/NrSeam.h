// The production OptiScaler.dll, loaded from the host's own folder and driven through its RetoqueWindow_* exports
// (retoque/dlssnr/DlssNr_WindowHost.cpp). The DLL is never freed: the present pass keeps what it built on the host's
// device until the process exits.
#pragma once

#include "../dlssnr/DlssNr_WindowHost.h"

#include <string>

struct IDXGISwapChain3;
struct ID3D12CommandQueue;

namespace RetoqueWindowHost
{
class NrSeam
{
  public:
    // Sets RETOQUE_WINDOW_HOST=1 in this process, loads OptiScaler.dll from beside the exe, checks the export version
    // and begins the session on the calling thread, which must be the one that presents. False with a reason.
    bool Load(std::string& why);

    // Before each Present of the host's swapchain, on the thread that called Load. False when the DLL refused the
    // present; the outcome says why.
    bool Present(IDXGISwapChain3* swapchain, ID3D12CommandQueue* queue, DlssNr::WindowHost::Outcome& outcome);

    void End();
    bool Loaded() const { return present_ != nullptr; }
    const std::wstring& Path() const { return path_; }

  private:
    using VersionFn = uint32_t (*)();
    using BeginFn = int (*)();
    using PresentFn = int (*)(IDXGISwapChain3*, ID3D12CommandQueue*, DlssNr::WindowHost::Outcome*);
    using EndFn = void (*)();
    PresentFn present_ = nullptr;
    EndFn end_ = nullptr;
    std::wstring path_;
};
} // namespace RetoqueWindowHost

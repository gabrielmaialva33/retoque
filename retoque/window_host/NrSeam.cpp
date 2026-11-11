#include "pch.h"

#include "NrSeam.h"
#include "Protocol.h"
#include "Sources.h"

namespace RetoqueWindowHost
{
namespace
{
std::wstring ExeFolder()
{
    std::wstring path(32768, L'\0');
    const DWORD length = GetModuleFileNameW(nullptr, path.data(), DWORD(path.size()));
    if (length == 0 || length >= path.size())
        return {};
    path.resize(length);
    const size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring {} : path.substr(0, slash);
}
} // namespace

bool NrSeam::Load(std::string& why)
{
    const std::wstring folder = ExeFolder();
    if (folder.empty())
    {
        why = "the host's own folder could not be read";
        return false;
    }
    path_ = folder + L"\\OptiScaler.dll";
    const DWORD attributes = GetFileAttributesW(path_.c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0)
    {
        why = "OptiScaler.dll is not beside RetoqueWindowHost.exe: " + Sources::Narrow(path_);
        return false;
    }

    // The seam is inert unless this process asks for it; a game never does.
    if (!SetEnvironmentVariableW(L"RETOQUE_WINDOW_HOST", L"1"))
    {
        why = "RETOQUE_WINDOW_HOST could not be set in the host's environment";
        return false;
    }
    // Its dependencies (the forwarder, the ini) resolve from its own folder.
    HMODULE module = LoadLibraryExW(path_.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (module == nullptr)
    {
        why = "OptiScaler.dll could not be loaded (Win32 error " + std::to_string(GetLastError()) + ")";
        return false;
    }

    const auto version = reinterpret_cast<VersionFn>(GetProcAddress(module, "RetoqueWindow_Version"));
    const auto begin = reinterpret_cast<BeginFn>(GetProcAddress(module, "RetoqueWindow_Begin"));
    const auto present = reinterpret_cast<PresentFn>(GetProcAddress(module, "RetoqueWindow_Present"));
    const auto end = reinterpret_cast<EndFn>(GetProcAddress(module, "RetoqueWindow_End"));
    if (version == nullptr || begin == nullptr || present == nullptr || end == nullptr)
    {
        why = "this OptiScaler.dll has no RetoqueWindow_* exports (a Retoque build with NR on any window is needed)";
        return false;
    }
    if (version() != Protocol::kSeamVersion)
    {
        why = "OptiScaler.dll's window-host export is version " + std::to_string(version()) + ", this host speaks " +
              std::to_string(Protocol::kSeamVersion);
        return false;
    }
    if (begin() != 1)
    {
        why = "OptiScaler.dll refused the window-host session";
        return false;
    }
    present_ = present;
    end_ = end;
    return true;
}

bool NrSeam::Present(IDXGISwapChain3* swapchain, ID3D12CommandQueue* queue, DlssNr::WindowHost::Outcome& outcome)
{
    outcome = {};
    if (present_ == nullptr)
        return false;
    return present_(swapchain, queue, &outcome) == 1;
}

void NrSeam::End()
{
    if (end_ != nullptr)
        end_();
    end_ = nullptr;
    present_ = nullptr;
}
} // namespace RetoqueWindowHost

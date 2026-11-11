#include "pch.h"

// Reading a window's facts for Policy.h. The candidate checks re-derive NeuRotic beae4eea's Candidate
// (apps/NeuRoticWindowWorker/capture/CaptureHost.cpp, GPL-3.0); see Licenses/NRAnything_ATTRIBUTION.txt.

#include "Sources.h"

#include <algorithm>

namespace RetoqueWindowHost::Sources
{
std::string Narrow(const std::wstring& text)
{
    if (text.empty())
        return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(size_t(bytes > 0 ? bytes : 0), '\0');
    if (bytes > 0)
        WideCharToMultiByte(CP_UTF8, 0, text.data(), int(text.size()), out.data(), bytes, nullptr, nullptr);
    return out;
}

namespace
{
std::string ClassName(HWND window)
{
    wchar_t name[256] {};
    const int length = GetClassNameW(window, name, int(std::size(name)));
    return length > 0 ? Narrow(std::wstring(name, size_t(length))) : std::string {};
}

Geometry::Rect ToRect(const RECT& r) { return { r.left, r.top, r.right, r.bottom }; }
} // namespace

std::string Title(HWND window)
{
    const int length = GetWindowTextLengthW(window);
    if (length <= 0)
        return {};
    std::wstring title(size_t(length) + 1, L'\0');
    const int copied = GetWindowTextW(window, title.data(), length + 1);
    title.resize(size_t(copied > 0 ? copied : 0));
    return Narrow(title);
}

Policy::WindowFacts Facts(HWND window)
{
    Policy::WindowFacts w;
    w.selfPid = GetCurrentProcessId();
    w.exists = IsWindow(window) != FALSE;
    if (!w.exists)
        return w;
    w.topLevel = GetAncestor(window, GA_ROOT) == window;
    w.desktopOrShell = window == GetDesktopWindow() || window == GetShellWindow();
    w.visible = IsWindowVisible(window) != FALSE;
    w.minimized = IsIconic(window) != FALSE;
    const LONG_PTR ex = GetWindowLongPtrW(window, GWL_EXSTYLE);
    w.transparent = (ex & WS_EX_TRANSPARENT) != 0;
    w.toolWindow = (ex & WS_EX_TOOLWINDOW) != 0;
    w.noActivate = (ex & WS_EX_NOACTIVATE) != 0;
    w.appWindow = (ex & WS_EX_APPWINDOW) != 0;
    w.owned = GetWindow(window, GW_OWNER) != nullptr;
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    w.pid = pid;
    DWORD cloaked = 0;
    w.cloakKnown = SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked)));
    w.cloaked = cloaked != 0;
    RECT bounds {};
    if (SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds))))
    {
        w.boundsKnown = true;
        w.width = bounds.right - bounds.left;
        w.height = bounds.bottom - bounds.top;
    }
    w.hasTitle = GetWindowTextLengthW(window) > 0;
    w.className = ClassName(window);
    DWORD affinity = 0;
    w.affinityKnown = GetWindowDisplayAffinity(window, &affinity) != FALSE;
    w.affinity = affinity;
    return w;
}

bool ReadIdentity(HWND window, Policy::Identity& identity)
{
    identity = {};
    DWORD pid = 0;
    GetWindowThreadProcessId(window, &pid);
    if (pid == 0)
        return false;
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (process == nullptr)
        return false;
    FILETIME created {}, exited {}, kernel {}, user {};
    std::wstring path(32768, L'\0');
    DWORD length = DWORD(path.size());
    const bool read = GetProcessTimes(process, &created, &exited, &kernel, &user) &&
                      QueryFullProcessImageNameW(process, 0, path.data(), &length);
    CloseHandle(process);
    // The window must still belong to that process once its creation time is read.
    DWORD again = 0;
    GetWindowThreadProcessId(window, &again);
    if (!read || again != pid || IsWindow(window) == FALSE)
        return false;
    path.resize(length);
    identity.hwnd = reinterpret_cast<uint64_t>(window);
    identity.pid = pid;
    identity.processCreation = (uint64_t(created.dwHighDateTime) << 32) | created.dwLowDateTime;
    identity.className = ClassName(window);
    identity.executable = Narrow(path);
    return identity.Complete();
}

std::vector<Entry> Catalog(size_t& refused)
{
    struct Walk
    {
        std::vector<Entry> entries;
        size_t refused = 0;
    } walk;
    EnumWindows(
        [](HWND window, LPARAM parameter) -> BOOL
        {
            auto& w = *reinterpret_cast<Walk*>(parameter);
            const Policy::WindowFacts facts = Facts(window);
            Entry entry;
            if (Policy::CandidateRefusal(facts) != nullptr || !ReadIdentity(window, entry.identity))
            {
                ++w.refused;
                return TRUE;
            }
            entry.title = Title(window);
            entry.width = facts.width;
            entry.height = facts.height;
            w.entries.push_back(std::move(entry));
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&walk));
    refused = walk.refused;
    return walk.entries;
}

Policy::SourceFacts SourceState(HWND window, const Policy::Identity& expected, bool itemClosed)
{
    Policy::SourceFacts s;
    s.itemClosed = itemClosed;
    s.exists = IsWindow(window) != FALSE;
    if (!s.exists)
        return s;
    Policy::Identity now;
    s.sameIdentity = ReadIdentity(window, now) && Policy::SameSource(expected, now);
    s.visible = IsWindowVisible(window) != FALSE;
    s.minimized = IsIconic(window) != FALSE;
    DWORD cloaked = 0;
    s.cloaked = SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_CLOAKED, &cloaked, sizeof(cloaked))) && cloaked != 0;
    return s;
}

bool CropInputs(HWND window, Geometry::CropInputs& in)
{
    RECT client {};
    if (!GetClientRect(window, &client))
        return false;
    SetLastError(ERROR_SUCCESS);
    if (MapWindowPoints(window, nullptr, reinterpret_cast<POINT*>(&client), 2) == 0 && GetLastError() != ERROR_SUCCESS)
        return false;
    RECT bounds {};
    if (FAILED(DwmGetWindowAttribute(window, DWMWA_EXTENDED_FRAME_BOUNDS, &bounds, sizeof(bounds))))
        return false;
    in.client = ToRect(client);
    in.frameBounds = ToRect(bounds);
    in.maximized = IsZoomed(window) != FALSE;
    MONITORINFO monitor { sizeof(monitor) };
    in.monitorWorkKnown = GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &monitor) != FALSE;
    if (in.monitorWorkKnown)
        in.monitorWork = ToRect(monitor.rcWork);
    BOOL rendering = TRUE;
    in.ncRenderingKnown =
        SUCCEEDED(DwmGetWindowAttribute(window, DWMWA_NCRENDERING_ENABLED, &rendering, sizeof(rendering)));
    in.ncRendering = rendering != FALSE;
    return true;
}

AdapterChoice AdapterFor(HWND window, IDXGIFactory1* factory)
{
    AdapterChoice choice;
    const HMONITOR monitor = MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST);
    ComPtr<IDXGIAdapter1> adapter;
    for (UINT a = 0; factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a)
    {
        ComPtr<IDXGIOutput> output;
        for (UINT o = 0; adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o)
        {
            DXGI_OUTPUT_DESC desc {};
            if (SUCCEEDED(output->GetDesc(&desc)) && desc.Monitor == monitor)
            {
                DXGI_ADAPTER_DESC1 adapterDesc {};
                adapter->GetDesc1(&adapterDesc);
                choice.outputFound = true;
                choice.vendorId = adapterDesc.VendorId;
                choice.luid = adapterDesc.AdapterLuid;
                choice.adapter = adapter;
                choice.name = Narrow(adapterDesc.Description);
                return choice;
            }
            output.Reset();
        }
        adapter.Reset();
    }
    return choice;
}
} // namespace RetoqueWindowHost::Sources

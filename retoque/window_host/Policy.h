// Which windows the host may capture, when a running capture pauses or stops, and which adapter may process it.
// Pure and Windows-free: Sources.cpp reads the facts off the window, these rules decide, and
// tests/nr-window-host-rules holds them on the host.
//
// The candidate filter re-derives NeuRotic beae4eea (apps/NeuRoticWindowWorker/capture/CaptureHost.cpp, Candidate),
// GPL-3.0; see docs/dlssnr/design/nr-anything.md. Display affinity and the adapter rule are ours.

#pragma once

#include <cstdint>
#include <string>

namespace RetoqueWindowHost::Policy
{
// The preview window's class: never offered as a source.
inline constexpr char kPreviewClass[] = "RetoqueWindowHostPreview";

// SetWindowDisplayAffinity's values (winuser.h), restated so this header stays Windows-free.
inline constexpr uint32_t kAffinityNone = 0x00;
inline constexpr uint32_t kAffinityMonitor = 0x01;
inline constexpr uint32_t kAffinityExcludeFromCapture = 0x11;

inline constexpr int32_t kMinimumSide = 50;
inline constexpr uint32_t kNvidia = 0x10DE;

struct WindowFacts
{
    bool exists = false;   // IsWindow
    bool topLevel = false; // its own root (GetAncestor GA_ROOT)
    bool desktopOrShell = false;
    bool visible = false;
    bool minimized = false;
    bool cloakKnown = false;
    bool cloaked = false;
    bool transparent = false; // WS_EX_TRANSPARENT
    bool toolWindow = false;  // WS_EX_TOOLWINDOW
    bool noActivate = false;  // WS_EX_NOACTIVATE
    bool appWindow = false;   // WS_EX_APPWINDOW
    bool owned = false;       // GetWindow GW_OWNER
    uint32_t pid = 0;
    uint32_t selfPid = 0;
    bool boundsKnown = false;
    int32_t width = 0, height = 0; // DWM extended frame bounds
    bool hasTitle = false;
    std::string className;
    bool affinityKnown = false;
    uint32_t affinity = kAffinityNone;
};

// Null when the window may be captured; else why not, a literal. A paused source keeps its identity but is hidden,
// minimized or cloaked for now: allowUnavailable lets those through, for the running capture's checks.
inline const char* CandidateRefusal(const WindowFacts& w, bool allowUnavailable = false)
{
    if (!w.exists || !w.topLevel)
        return "not a top-level window";
    if (w.desktopOrShell)
        return "the desktop and the shell are not captured";
    if (!allowUnavailable && (!w.visible || w.minimized))
        return "the window is hidden or minimized";
    if (w.transparent || w.toolWindow || w.noActivate)
        return "transparent, tool and non-activating windows are not captured";
    if (w.owned && !w.appWindow)
        return "owned pop-up windows are not captured";
    if (w.pid == 0 || w.pid == w.selfPid)
        return "the host's own windows are not captured";
    if (!w.cloakKnown || (!allowUnavailable && w.cloaked))
        return "the window is cloaked or unknown to the compositor";
    if (!w.boundsKnown || w.width < kMinimumSide || w.height < kMinimumSide)
        return "the window is smaller than 50 pixels on a side";
    if (!w.hasTitle)
        return "the window has no title";
    if (w.className == kPreviewClass || w.className == "Progman" || w.className == "WorkerW" ||
        w.className == "Shell_TrayWnd" || w.className == "Xaml_WindowedPopupClass")
        return "shell, pop-up and host windows are not captured";
    if (w.affinityKnown && w.affinity != kAffinityNone)
        return "the window asks not to be captured (display affinity); it is never worked around";
    return nullptr;
}

// What makes a window the same source from frame to frame: a handle can be reused by another window after the first
// closes.
struct Identity
{
    uint64_t hwnd = 0;
    uint32_t pid = 0;
    uint64_t processCreation = 0; // FILETIME of the owning process's creation
    std::string className;
    std::string executable;
    bool Complete() const { return hwnd != 0 && pid != 0 && processCreation != 0; }
};

inline bool SameSource(const Identity& a, const Identity& b)
{
    return a.Complete() && b.Complete() && a.hwnd == b.hwnd && a.pid == b.pid &&
           a.processCreation == b.processCreation && a.className == b.className && a.executable == b.executable;
}

enum class Action
{
    Run,
    Pause, // keep the session and the preview's last frame; capture nothing
    Stop,  // end the session
};

struct Decision
{
    Action action = Action::Run;
    const char* reason = nullptr;
};

struct SourceFacts
{
    bool exists = true;
    bool itemClosed = false; // WGC's GraphicsCaptureItem.Closed fired
    bool sameIdentity = true;
    bool visible = true;
    bool minimized = false;
    bool cloaked = false;
};

// Run, pause or stop, checked before each capture poll. Preview mode: menus, dialogs and focus never pause it (the
// overlay's interaction policy is phase 3).
inline Decision DecideSource(const SourceFacts& s)
{
    if (!s.exists || s.itemClosed)
        return { Action::Stop, "the source window closed" };
    if (!s.sameIdentity)
        return { Action::Stop, "the window handle now belongs to another window" };
    if (s.minimized)
        return { Action::Pause, "the source is minimized" };
    if (!s.visible)
        return { Action::Pause, "the source is hidden" };
    if (s.cloaked)
        return { Action::Pause, "the source is cloaked (another virtual desktop, or suspended)" };
    return { Action::Run, nullptr };
}

// Capture, transport, NR and the preview all run on the adapter driving the monitor the source is on; version 1 never
// copies across adapters (a source on an integrated GPU's display is refused, as NeuRotic does).
inline const char* AdapterRefusal(bool outputFound, uint32_t vendorId)
{
    if (!outputFound)
        return "the monitor showing the window is on no adapter this host can see";
    if (vendorId != kNvidia)
        return "the monitor showing the window is driven by a non-NVIDIA adapter; version 1 does not copy across "
               "adapters";
    return nullptr;
}
} // namespace RetoqueWindowHost::Policy

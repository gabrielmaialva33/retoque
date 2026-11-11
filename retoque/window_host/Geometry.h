// Where the source's client area is inside a Windows Graphics Capture frame. Pure and Windows-free, so
// tests/nr-window-host-rules can hold it on the host.
//
// WGC captures a window as DWM composes it: its extended frame bounds (DWMWA_EXTENDED_FRAME_BOUNDS), which include
// the title bar and borders, not just the client area NR should see. The crop is the client rectangle relative to
// those bounds, used only when the frame's own size confirms which bounds it was taken from. Re-derived from
// NeuRotic beae4eea (apps/NeuRoticWindowWorker/capture/CaptureHost.cpp, ClientCrop), which adapted Magpie 69d6105d
// (CalcWindowCapturedFrameBounds), both GPL-3.0; see Licenses/Magpie-LICENSE.txt and
// docs/dlssnr/design/nr-anything.md.

#pragma once

#include <cstdint>

namespace RetoqueWindowHost::Geometry
{
struct Rect
{
    int32_t left = 0, top = 0, right = 0, bottom = 0;
    int64_t Width() const { return int64_t(right) - left; }
    int64_t Height() const { return int64_t(bottom) - top; }
    bool Empty() const { return Width() <= 0 || Height() <= 0; }
};

struct Size
{
    int32_t width = 0, height = 0;
};

// A D3D11_BOX's x and y: the region of the capture texture that becomes the frame NR sees.
struct Box
{
    uint32_t left = 0, top = 0, right = 0, bottom = 0;
    uint32_t Width() const { return right - left; }
    uint32_t Height() const { return bottom - top; }
    bool operator==(const Box&) const = default;
};

struct CropInputs
{
    Rect client;      // the source's client rectangle, in screen coordinates
    Rect frameBounds; // DWMWA_EXTENDED_FRAME_BOUNDS, in screen coordinates
    Size content;     // the WGC frame's ContentSize
    bool maximized = false;
    bool monitorWorkKnown = false;
    Rect monitorWork; // the work area of the monitor the window is on
    bool ncRenderingKnown = false;
    bool ncRendering = true; // DWMWA_NCRENDERING_ENABLED
};

struct Crop
{
    bool ok = false;
    Box box;
    const char* reason = nullptr; // a literal when !ok
};

inline Rect Intersect(const Rect& a, const Rect& b)
{
    Rect r { a.left > b.left ? a.left : b.left, a.top > b.top ? a.top : b.top, a.right < b.right ? a.right : b.right,
             a.bottom < b.bottom ? a.bottom : b.bottom };
    if (r.Empty())
        return {};
    return r;
}

inline bool SizeMatches(const Rect& r, Size content)
{
    return r.Width() == content.width && r.Height() == content.height;
}

inline Crop ClientCrop(const CropInputs& in)
{
    Crop result;
    if (in.content.width <= 0 || in.content.height <= 0)
    {
        result.reason = "the captured frame is empty";
        return result;
    }
    if (in.client.Empty())
    {
        result.reason = "the source window has no client area";
        return result;
    }

    Rect bounds = in.frameBounds;
    if (!SizeMatches(bounds, in.content))
    {
        // Magpie's maximized-window correction (before Windows 11 24H2 a maximized window's frame bounds reach past
        // the monitor): the client when it starts above the work area, else the bounds cut to the work area. Only
        // when the captured size confirms it.
        bool matched = false;
        if (in.maximized && in.monitorWorkKnown)
        {
            const Rect alternate =
                in.client.top < in.monitorWork.top ? in.client : Intersect(in.frameBounds, in.monitorWork);
            if (SizeMatches(alternate, in.content))
            {
                bounds = alternate;
                matched = true;
            }
        }
        // A window DWM draws no frame for is captured as its client area.
        if (!matched && in.ncRenderingKnown && !in.ncRendering && SizeMatches(in.client, in.content))
        {
            bounds = in.client;
            matched = true;
        }
        if (!matched)
        {
            result.reason = "the captured size does not confirm where the client area starts";
            return result;
        }
    }

    if (in.client.left < bounds.left || in.client.top < bounds.top || in.client.right > bounds.right ||
        in.client.bottom > bounds.bottom)
    {
        result.reason = "the client area lies outside the captured frame";
        return result;
    }

    result.ok = true;
    result.box = { uint32_t(int64_t(in.client.left) - bounds.left), uint32_t(int64_t(in.client.top) - bounds.top),
                   uint32_t(int64_t(in.client.right) - bounds.left), uint32_t(int64_t(in.client.bottom) - bounds.top) };
    return result;
}
} // namespace RetoqueWindowHost::Geometry

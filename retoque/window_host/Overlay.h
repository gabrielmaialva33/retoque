// Pure overlay decisions. The interaction and rectangle-following ideas were re-derived from NeuRotic beae4eea
// (InteractionPolicy.h and CaptureHost.cpp), which adapted Magpie 69d6105d, both GPL-3.0. No source or tables copied;
// see Licenses/NRAnything_ATTRIBUTION.txt. The fresh-frame visibility gate is Retoque's.
#pragma once

#include "Geometry.h"
#include "Policy.h"

#include <climits>

namespace RetoqueWindowHost::Overlay
{
struct Interaction
{
    bool disabled = false, dialog = false, menu = false, moveSize = false;
    bool minimized = false, cloaked = false, hidden = false, unknown = false;
    bool exclusive = false, topmost = false;
};

inline Policy::Decision Decide(const Interaction& s)
{
    using Policy::Action;
    if (s.exclusive)
        return { Action::Stop, "exclusive-fullscreen" };
    if (s.topmost)
        return { Action::Stop, "source-topmost" };
    if (s.unknown)
        return { Action::Pause, "source-state-unavailable" };
    if (s.hidden)
        return { Action::Pause, "source-hidden" };
    if (s.minimized)
        return { Action::Pause, "source-minimized" };
    if (s.cloaked)
        return { Action::Pause, "source-cloaked" };
    if (s.disabled)
        return { Action::Pause, "source-disabled" };
    if (s.dialog)
        return { Action::Pause, "source-dialog" };
    if (s.menu)
        return { Action::Pause, "source-menu" };
    if (s.moveSize)
        return { Action::Pause, "source-move-size" };
    return {};
}

inline const char* StartRefusal(bool cursorControl)
{
    if (!cursorControl)
        return "cursor-capture-control-unavailable";
    return nullptr;
}

inline bool SameRect(const Geometry::Rect& a, const Geometry::Rect& b)
{
    return a.left == b.left && a.top == b.top && a.right == b.right && a.bottom == b.bottom;
}

inline bool Usable(const Geometry::Rect& r) { return !r.Empty() && r.Width() <= INT32_MAX && r.Height() <= INT32_MAX; }

// Positions and timestamps are physical screen pixels and QPC-derived 100 ns ticks, just as in the capture.
// A change invalidates the old image, including a same-size move. Resume also rejects frames queued during pause.
class Visibility
{
  public:
    bool Observe(const Geometry::Rect& client, bool allowed, int64_t now)
    {
        allowed = allowed && Usable(client);
        if (!allowed || !allowed_ || !SameRect(client_, client))
        {
            accepted_ = false;
            earliest_ = now;
        }
        client_ = client;
        allowed_ = allowed;
        return Visible();
    }

    bool Accept(const Geometry::Rect& capturedClient, Geometry::Size frame, int64_t timestamp)
    {
        accepted_ = allowed_ && SameRect(client_, capturedClient) && Geometry::SizeMatches(client_, frame) &&
                    timestamp >= earliest_;
        return Visible();
    }

    bool Visible() const { return allowed_ && accepted_; }

  private:
    Geometry::Rect client_;
    int64_t earliest_ = 0;
    bool allowed_ = false, accepted_ = false;
};
} // namespace RetoqueWindowHost::Overlay

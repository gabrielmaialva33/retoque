#pragma once
#include "DlssNr_PresentGuideRing.h"
#include <array>
#include <cstdint>

// Streamline tag guides: ideas from MFGAdaUnlock-RenoDx 7dabaa6 (MIT), reimplemented with explicit identity.
// No resource pointer, timeout, GPU wait or retirement rule lives here.
namespace DlssNr::SlPresent
{
struct Recording
{
    bool open = false, inside = false, tainted = true;
    void Reset(bool success)
    {
        open = success;
        inside = false;
        tainted = !success;
    }
    void Begin(bool ordinary)
    {
        tainted |= !open || inside || !ordinary;
        inside = true;
    }
    void End()
    {
        tainted |= !open || !inside;
        inside = false;
    }
    void Close() { open = false; }
    bool CanCopy() const { return open && !inside && !tainted; }
};

struct Marker
{
    uint32_t token = 0;
    bool valid = false, ambiguous = false;
    void Start(uint32_t value)
    {
        ambiguous |= valid;
        token = value;
        valid = true;
    }
    void End()
    {
        valid = false;
        ambiguous = false;
    }
    bool Take(uint32_t& value)
    {
        value = token;
        const bool result = valid && !ambiguous;
        End();
        return result;
    }
};

class Frames
{
  public:
    static constexpr size_t Capacity = PresentGuides::kRingSize;
    struct Entry
    {
        uint32_t token = 0, viewport = 0;
        uint64_t revision = 0, captureEpoch = 0;
        bool constants = false, tags = false, captured = false, invalid = false;
    };

  private:
    std::array<Entry, Capacity> entries {};
    uint64_t next = 0;
    uint32_t viewport = 0, lastPresent = 0;
    bool viewportKnown = false, ambiguous = false, presented = false;
    bool View(uint32_t value)
    {
        if (value == UINT32_MAX)
        {
            ambiguous = true;
            return false;
        }
        if (viewportKnown && value != viewport)
            ambiguous = true;
        viewportKnown = true;
        viewport = value;
        return !ambiguous;
    }

  public:
    Entry& Get(uint32_t token) { return entries[token % Capacity]; }
    const Entry& Get(uint32_t token) const { return entries[token % Capacity]; }
    uint64_t Constants(uint32_t token, uint32_t view, bool valid)
    {
        if (!View(view) || (presented && token <= lastPresent))
            return 0;
        auto& e = Get(token);
        if (e.revision && e.token == token)
        {
            e.invalid = true;
            return 0;
        }
        e = { token, view, ++next, 0, valid, false, false, !valid };
        return valid ? e.revision : 0;
    }
    uint64_t Tags(uint32_t token, uint32_t view, bool complete)
    {
        if (!View(view))
            return 0;
        auto& e = Get(token);
        if (!e.revision || e.token != token)
        {
            e = { token, view, ++next, 0, false, true, false, true };
            return 0;
        }
        if (e.tags || !complete || !e.constants || (presented && token <= lastPresent))
            e.invalid = true;
        e.tags = true;
        return e.invalid ? 0 : e.revision;
    }
    void Invalidate(uint32_t token)
    {
        auto& e = Get(token);
        if (e.token == token)
            e.invalid = true;
    }
    void Tokenless()
    {
        for (auto& e : entries)
            e.invalid = true;
    }
    bool Capture(uint32_t token, uint64_t revision, uint64_t epoch)
    {
        auto& e = Get(token);
        if (!epoch || !revision || e.token != token || e.revision != revision || !e.constants || !e.tags || e.invalid ||
            ambiguous || e.captured)
            return false;
        e.captured = true;
        e.captureEpoch = epoch;
        return true;
    }
    bool Present(uint32_t token, uint64_t epoch, bool explicitToken)
    {
        if (!explicitToken || (presented && token <= lastPresent))
            return false;
        presented = true;
        lastPresent = token;
        const auto& e = Get(token);
        return !ambiguous && e.token == token && e.captured && !e.invalid && e.captureEpoch + 1 == epoch;
    }
    bool Matches(uint32_t token, uint64_t revision) const
    {
        const auto& e = Get(token);
        return !ambiguous && e.token == token && e.revision == revision && e.captured && !e.invalid;
    }
};
// No same-frame GPU guard is specified/implemented in b1. Unknown MUST refuse HUD-less use.
constexpr bool HudlessAllowed(bool sameFrameGuard, uint64_t changed, uint64_t pixels)
{
    return sameFrameGuard && pixels > 0 && changed <= pixels &&
           static_cast<long double>(changed) / static_cast<long double>(pixels) <= 0.30L;
}
} // namespace DlssNr::SlPresent

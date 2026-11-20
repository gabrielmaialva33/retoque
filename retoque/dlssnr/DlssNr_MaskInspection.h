#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string_view>
#include <type_traits>

// Mask inspection, slice 0: which protections one recorded frame of the D3D12 present pass was sent with, said in the
// log. docs/dlssnr/design/character-inspector.md (section 4), tested by tests/nr-mask-inspection. The idea of an
// inspection surface is NeuRotic's; nothing here is taken from it, and nothing here detects a character.
//
// [DlssNr] MaskInspection=log asks for it; off, auto, unset and anything else are off. Off is the config test and one
// bool of the observer, read under the lock Dispatch already holds: no allocation, lock, clock, GPU command, hook,
// model query or log line. On, it is still values only: a fixed-size snapshot built on Dispatch's stack from what the
// pass decided for itself, compared with the last line said, and one line when that changed, at most every kMinGapMs
// and kMaxLines times per process. No pixel, texture, resource pointer, path or name is read, kept or said.
//
// What a line states is what the CPU recorded on the command list, not what the GPU completed or what a pixel shows:
// whether the UI alpha was handed over (never how much of the picture it covers), the skin settings each pass was
// given (the model's own skin mask is internal and is not exposed), and which composition branches the resolve was
// sent with (never how many pixels took them).
//
// Not synchronized: one Observer belongs to one caller's lock (g_nrMutex for the D3D12 pass).
namespace DlssNr::MaskInspection
{
// A line is said at most this often, and only when what it states changed.
inline constexpr uint64_t kMinGapMs = 2000;
// And at most this many times per process; the last one says it is the last.
inline constexpr uint32_t kMaxLines = 256;
// PassConfig::MaxPasses, held equal by a static_assert where the pass publishes.
inline constexpr uint32_t kMaxPasses = 10;
inline constexpr size_t kLineSize = 2048;

using Line = std::array<char, kLineSize>;

// The request. Config::readString lowercases the key, so Log and LOG ask as well; any other text is off.
inline bool IsLog(std::string_view value) { return value == "log"; }

// Neither option is copied: a malformed value of any length costs a comparison, not an allocation.
template <class Config> bool Requested(const Config& cfg)
{
    return cfg.DlssNrEnabled.value_or_default() && cfg.DlssNrMaskInspection.has_value() &&
           IsLog(*cfg.DlssNrMaskInspection);
}

// The names are the comparison capture's (PairCapture::FrameFacts::route).
enum class Route : uint8_t
{
    Present,
    Bridge,
    BeforeUpscale,
    AfterRayReconstruction,
    AfterUpscale,
};

// The one route with a producer in this slice. Every other D3D12 route says it is unavailable, once per generation.
inline constexpr bool Observed(Route route) { return route == Route::Present; }

inline constexpr Route RouteOf(bool presentSource, bool bridgeHost, bool beforeUpscale, bool afterRayReconstruction)
{
    if (presentSource)
        return bridgeHost ? Route::Bridge : Route::Present;
    if (beforeUpscale)
        return Route::BeforeUpscale;
    return afterRayReconstruction ? Route::AfterRayReconstruction : Route::AfterUpscale;
}

inline constexpr const char* RouteName(Route route)
{
    switch (route)
    {
    case Route::Present:
        return "d3d12-present";
    case Route::Bridge:
        return "d3d11-bridge";
    case Route::BeforeUpscale:
        return "before-upscale";
    case Route::AfterRayReconstruction:
        return "after-rr";
    default:
        return "after-upscale";
    }
}

enum class Outcome : uint8_t
{
    NotRecorded, // the pass declined or failed before its resolve: the frame goes out as the game drew it
    Recorded,    // the resolve was recorded after a model call
    Carried,     // the resolve was recorded on a model cadence frame that calls no model
    DriverProxy, // the driver ran the model; this pass composed nothing
};

enum class Ui : uint8_t
{
    NotDecided,
    Off,         // UiProtection is off
    NotProduced, // UiProtection is on and the mask pass recorded nothing
    Handed,      // the mask was recorded and handed over as UI and UIAlpha
};

// Settings are kept at the precision they are printed with, so a line changes exactly when its text does and a
// non-finite value from a hand-edited ini is one stable value rather than one that never equals itself.
inline constexpr int32_t kNotFinite = INT32_MIN;

inline int32_t Centi(float value)
{
    if (!std::isfinite(value))
        return kNotFinite;
    return static_cast<int32_t>(std::llround(std::clamp(static_cast<double>(value), -1.0e6, 1.0e6) * 100.0));
}

struct Pass
{
    int32_t skinStructure = 0; // Centi; negative follows LocalStructure
    bool autoMask = false;
    bool operator==(const Pass&) const = default;
};

// One Dispatch, as values. Nothing in it names a resource.
struct Snapshot
{
    // Provenance: the present this was recorded for. Never part of what makes a line new.
    uint64_t sequence = 0;

    // The model feature's generation: a resize, a device change and a rebuild each make a new one.
    uint64_t modelGeneration = 0;
    uint32_t width = 0; // the model's working extent
    uint32_t height = 0;
    uint32_t requestedPasses = 0;
    uint32_t evaluatedPasses = 0; // on a carried frame, the passes that made the edit being carried
    std::array<Pass, kMaxPasses> passes {};

    // What the resolve was sent, as DlssNrConstants carries it.
    int32_t detail = 0;        // Centi(TransferStrength)
    int32_t maxRatio = 0;      // Centi(MaxRatio)
    int32_t residualLimit = 0; // Centi(ResidualLimit), stops; 0 is off
    int32_t minRatio = 0;      // Centi(MinRatio); 0 is off
    uint32_t compare = 0;
    uint32_t debugView = 0;
    uint32_t reversibleMode = 0;

    Route route = Route::Present;
    Outcome outcome = Outcome::NotRecorded;
    Ui ui = Ui::NotDecided;
    bool applyModel = false;
    bool passthrough = false;
    bool highlightRecovery = false;

    bool operator==(const Snapshot&) const = default;
};
static_assert(std::is_trivially_copyable_v<Snapshot>);

inline constexpr bool IsRecording(const Snapshot& s)
{
    return Observed(s.route) && (s.outcome == Outcome::Recorded || s.outcome == Outcome::Carried);
}

// What a line may state, or false when the sample cannot be trusted and is skipped. A frame with no recording keeps
// its route, its outcome and its sequence, and nothing a later step may have half written.
inline bool Normalize(const Snapshot& raw, Snapshot& out)
{
    out = {};
    out.sequence = raw.sequence;
    out.route = raw.route;
    if (!Observed(raw.route))
        return true;
    out.outcome = raw.outcome;
    if (!IsRecording(raw))
        return true;
    if (raw.width == 0 || raw.height == 0 || raw.requestedPasses == 0 || raw.requestedPasses > kMaxPasses ||
        raw.evaluatedPasses == 0 || raw.evaluatedPasses > raw.requestedPasses || raw.ui == Ui::NotDecided)
        return false;
    out = raw;
    for (uint32_t i = raw.evaluatedPasses; i < kMaxPasses; ++i)
        out.passes[i] = {};
    return true;
}

// Whether two snapshots would be said in the same words, the frame number aside. Model cadence alternates model
// frames and carried ones by design, so which of the two a line happened to see is provenance as well.
inline bool SameStatus(Snapshot a, Snapshot b)
{
    a.sequence = b.sequence = 0;
    if (a.outcome == Outcome::Carried)
        a.outcome = Outcome::Recorded;
    if (b.outcome == Outcome::Carried)
        b.outcome = Outcome::Recorded;
    return a == b;
}

inline constexpr bool SameContext(const Snapshot& a, const Snapshot& b)
{
    return a.modelGeneration == b.modelGeneration && a.width == b.width && a.height == b.height &&
           a.requestedPasses == b.requestedPasses;
}

// Which branches of the resolve (dlssnr.hlsl, mode Resolve) the recorded constants select, in the shader's own order.
// Applicability only: a branch that applies may still leave every pixel as it was.
struct Composition
{
    const char* notShown = nullptr; // why the model's edit does not reach the picture; nullptr when it is composed
    bool replace = false;           // ReversibleMode 2 or 4: the model's picture is taken whole
    bool guard = false;             // the luminance guard (MaxRatio) and the near-black hue trust
    bool residualLimit = false;
    bool highlightRecovery = false;
    bool darkeningFloor = false;
};

inline constexpr Composition Compose(const Snapshot& s)
{
    Composition c;
    if (!IsRecording(s))
    {
        c.notShown = "nothing was recorded";
        return c;
    }
    if (s.compare == 0 && s.debugView == 0 && s.detail <= 0 && s.detail != kNotFinite)
    {
        c.notShown = "detail strength 0 returns the frame exactly";
        return c;
    }
    if (!s.applyModel)
    {
        c.notShown = "ApplyModel is off";
        return c;
    }
    if (s.debugView >= 1 && s.debugView <= 3)
    {
        c.notShown = "a debug view replaces the picture";
        return c;
    }
    c.replace = s.reversibleMode == 2 || s.reversibleMode == 4;
    c.guard = !c.replace;
    c.residualLimit = !c.replace && s.residualLimit > 0;
    c.highlightRecovery = s.highlightRecovery && !s.passthrough;
    c.darkeningFloor = s.minRatio > 0;
    return c;
}

// Bounded text: what does not fit is dropped and reported, never written past the end. Integers only, so the line
// does not depend on the process locale.
class Writer
{
    Line& _line;
    size_t _used = 0;
    bool _cut = false;

  public:
    explicit Writer(Line& line) : _line(line) { _line[0] = '\0'; }

    Writer& Text(std::string_view text)
    {
        const size_t room = _line.size() - 1 - _used;
        const size_t count = std::min(text.size(), room);
        _cut = _cut || count != text.size();
        std::memcpy(_line.data() + _used, text.data(), count);
        _used += count;
        _line[_used] = '\0';
        return *this;
    }

    Writer& Number(uint64_t value)
    {
        char digits[24];
        const auto end = std::to_chars(digits, digits + sizeof(digits), value).ptr;
        return Text(std::string_view(digits, static_cast<size_t>(end - digits)));
    }

    Writer& Fixed(int32_t centi)
    {
        if (centi == kNotFinite)
            return Text("not finite");
        const uint64_t magnitude = static_cast<uint64_t>(centi < 0 ? -static_cast<int64_t>(centi) : centi);
        if (centi < 0)
            Text("-");
        Number(magnitude / 100).Text(".");
        if (magnitude % 100 < 10)
            Text("0");
        return Number(magnitude % 100);
    }

    bool Complete() const { return !_cut; }
};

// The line for one normalized snapshot. False when it did not fit (it is still terminated).
inline bool Format(const Snapshot& s, uint64_t generation, bool last, Line& line)
{
    Writer w(line);
    w.Text("DLSS-NR mask inspection (log only; recorded on the CPU, not GPU-completed; no pixel read): route ")
        .Text(RouteName(s.route))
        .Text(", frame ")
        .Number(s.sequence)
        .Text(", generation ")
        .Number(generation);

    if (!Observed(s.route))
    {
        w.Text(", unavailable on this route: only d3d12-present is observed, and no other source is looked for");
    }
    else if (s.outcome == Outcome::DriverProxy)
    {
        w.Text(", unavailable with the driver proxy: the driver runs the model and this pass composes nothing");
    }
    else if (!IsRecording(s))
    {
        w.Text(", nothing recorded for this frame: the pass declined or failed before its resolve (the DLSS-NR status "
               "line says why), so the frame is as the game drew it");
    }
    else
    {
        const bool carried = s.outcome == Outcome::Carried;
        w.Text(", model generation ").Number(s.modelGeneration).Text(", working ").Number(s.width).Text("x");
        w.Number(s.height);
        if (carried)
            w.Text(", carried frame (no model call; the last edit, made by ").Number(s.evaluatedPasses);
        else
            w.Text(", model call recorded (").Number(s.evaluatedPasses);
        w.Text(" of ").Number(s.requestedPasses).Text(s.requestedPasses == 1 ? " pass)" : " passes)");

        w.Text(" | UI alpha: ");
        switch (s.ui)
        {
        case Ui::Handed:
            w.Text(carried ? "handed to the carry (coverage unknown)"
                           : "handed to pass 1 as UI and UIAlpha (coverage unknown)");
            break;
        case Ui::NotProduced:
            w.Text("not handed: the UI mask pass recorded nothing");
            break;
        default:
            w.Text("not handed: UiProtection is off");
            break;
        }

        // Consecutive passes with the same settings are said together.
        w.Text(" | skin: ");
        for (uint32_t first = 0; first < s.evaluatedPasses;)
        {
            uint32_t next = first + 1;
            while (next < s.evaluatedPasses && s.passes[next] == s.passes[first])
                ++next;
            if (next - first == 1)
                w.Text("pass ").Number(first + 1);
            else
                w.Text("passes ").Number(first + 1).Text("-").Number(next);
            w.Text(s.passes[first].autoMask ? " AutoMask on, SkinStructure " : " AutoMask off, SkinStructure ");
            if (s.passes[first].skinStructure < 0 && s.passes[first].skinStructure != kNotFinite)
                w.Text("follows LocalStructure");
            else
                w.Fixed(s.passes[first].skinStructure);
            w.Text("; ");
            first = next;
        }
        w.Text("the model's skin mask is internal and not exposed");

        const Composition c = Compose(s);
        w.Text(" | composition: ");
        if (c.notShown != nullptr)
        {
            w.Text("the model's edit is not shown (").Text(c.notShown).Text("), so no guard applies");
        }
        else
        {
            w.Text(c.replace ? "the model's picture is taken whole (replace mode)" : "the model's edit is composed");
            w.Text("; luminance guard ");
            if (c.guard)
                w.Text("applies (").Fixed(s.maxRatio).Text("x)");
            else
                w.Text("does not apply (replace mode)");
            w.Text("; residual limit ");
            if (c.residualLimit)
                w.Text("applies (").Fixed(s.residualLimit).Text(" stops)");
            else
                w.Text(c.replace && s.residualLimit > 0 ? "does not apply (replace mode)" : "off");
            w.Text("; highlight recovery ");
            if (c.highlightRecovery)
                w.Text("applies");
            else
                w.Text(s.passthrough ? "does not apply (the frame arrived tone mapped)" : "off");
            w.Text("; darkening floor ");
            if (c.darkeningFloor)
                w.Text("applies (").Fixed(s.minRatio).Text("x)");
            else
                w.Text("off");
            if (s.compare != 0)
                w.Text("; compare mode ").Number(s.compare).Text(" shows one side as it arrived");
            if (s.debugView != 0)
                w.Text("; debug view ").Number(s.debugView);
        }
        w.Text(" | these are the branches the frame was sent with, not how many pixels took them");
    }

    if (last)
        w.Text(" | line budget reached: mask inspection says nothing more in this process");
    return w.Complete();
}

// The gate, the generations and the rate. Constant-initialized, fixed size, trivially destructible.
class Observer
{
  public:
    // Off reads `_live` and returns: nothing else is touched unless inspection was on a moment ago. Turning it off
    // ends the generation, so nothing observed before can be said after it is turned on again. True when this
    // Dispatch is to be observed.
    bool Gate(bool requested)
    {
        if (!requested)
        {
            if (_live)
            {
                _live = false;
                Restart();
            }
            return false;
        }
        if (_lines >= kMaxLines)
            return false;
        _live = true;
        return true;
    }

    // What came before no longer describes what follows (inspection or the pass was switched off and on, the pass's
    // history was reset): the next observation begins a generation, before which the snapshot held for a decline and
    // the line last said are both dropped, and is said as the first of it.
    void Restart() { _restart = true; }

    // One Dispatch's snapshot. True when `line` is to be said. A sample that cannot be trusted changes nothing.
    bool Observe(const Snapshot& raw, uint64_t nowMs, Line& line)
    {
        if (!_live || _lines >= kMaxLines)
            return false;
        Snapshot s;
        if (!Normalize(raw, s))
            return false;

        const bool recording = IsRecording(s);
        if (_restart || !_haveRoute || s.route != _route || (recording && _haveContext && !SameContext(s, _context)))
        {
            ++_generation;
            _restart = false;
            _haveSaid = false;
            _haveHeld = false;
            _haveContext = false;
        }
        _route = s.route;
        _haveRoute = true;

        // A decline between two recordings (a second Dispatch refused at the same present, a settling chain) is not
        // the frame's status while a recording is less than one gap old: that recording still is. One that lasts is.
        const Snapshot* status = &s;
        if (recording)
        {
            _context = s;
            _haveContext = true;
            _held = s;
            _heldAtMs = nowMs;
            _haveHeld = true;
        }
        else if (_haveHeld && Observed(s.route) && s.outcome == Outcome::NotRecorded && nowMs >= _heldAtMs &&
                 nowMs - _heldAtMs < kMinGapMs)
        {
            status = &_held;
        }
        else
        {
            _haveHeld = false;
        }

        if (_haveSaid && SameStatus(*status, _said))
            return false;

        if (_haveSaidAt)
        {
            // A clock that went back waits one whole gap from where it is now: never early, never silent for longer.
            if (nowMs < _saidAtMs)
            {
                _saidAtMs = nowMs;
                return false;
            }
            if (nowMs - _saidAtMs < kMinGapMs)
                return false;
        }

        ++_lines;
        Format(*status, _generation, _lines >= kMaxLines, line);
        _said = *status;
        _haveSaid = true;
        _saidAtMs = nowMs;
        _haveSaidAt = true;
        return true;
    }

    bool Live() const { return _live; }
    uint64_t Generation() const { return _generation; }
    uint32_t Lines() const { return _lines; }

  private:
    Snapshot _said {};
    Snapshot _held {};
    Snapshot _context {};
    uint64_t _generation = 0;
    uint64_t _saidAtMs = 0;
    uint64_t _heldAtMs = 0;
    uint32_t _lines = 0;
    Route _route = Route::Present;
    bool _live = false;
    bool _restart = false;
    bool _haveRoute = false;
    bool _haveContext = false;
    bool _haveHeld = false;
    bool _haveSaid = false;
    bool _haveSaidAt = false;
};
static_assert(std::is_trivially_copyable_v<Observer> && std::is_trivially_destructible_v<Observer>);

// One Dispatch's side of it, on the stack. Off, it holds an empty optional and its destructor returns: the clock is
// not read and nothing is said. On, the snapshot is published when the Dispatch ends, whichever return it took, so a
// frame the pass declined is never described by an earlier one.
template <class Clock, class Say> class Scope
{
    Observer* _observer = nullptr;
    std::optional<Snapshot> _value;
    Clock _clock;
    Say _say;

  public:
    Scope(Observer& observer, bool requested, Clock clock, Say say) : _clock(clock), _say(say)
    {
        if (!observer.Gate(requested))
            return;
        _observer = &observer;
        _value.emplace();
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

    explicit operator bool() const { return _value.has_value(); }
    Snapshot& operator*() { return *_value; }
    Snapshot* operator->() { return &*_value; }

    void Restart()
    {
        if (_value)
            _observer->Restart();
    }

    ~Scope()
    {
        if (!_value)
            return;
        Line line;
        try
        {
            if (_observer->Observe(*_value, _clock(), line))
                _say(line.data());
        }
        catch (...)
        {
        }
    }
};

// A route with no producer of its own (native Vulkan) says so once per process, and only while log is requested. Off,
// the flag is not read.
class RouteNotice
{
    std::atomic<bool> _said { false };

  public:
    bool Take(bool requested)
    {
        return requested && !_said.load(std::memory_order_relaxed) && !_said.exchange(true, std::memory_order_relaxed);
    }
};

inline constexpr char kVulkanUnavailable[] =
    "DLSS-NR mask inspection (log only): unavailable on native Vulkan: only the D3D12 present route is observed, and "
    "no other source is looked for";
} // namespace DlssNr::MaskInspection

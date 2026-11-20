#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <iomanip>
#include <locale>
#include <sstream>
#include <string>
#include <string_view>

// CPU evidence only. No D3D types, borrowed resource access or model inputs.
// Present-depth-without-upscaler.md: R1 and R2 have independent decisions and denominators.
namespace DlssNr::PresentDepth
{
constexpr size_t kResources = 512;
constexpr size_t kEvents = 1024;
using Id = uint64_t; // generation, never a resource address
inline bool Enabled(bool nr, unsigned method, std::string_view value) { return nr && method == 2 && value == "log"; }
enum class Reason : uint8_t
{
    None,
    Extent,
    Multisampled,
    Tie,
    Direction,
    State,
    Cleared,
    Aliased,
    OtherQueue,
    Unobserved,
    Accepted,
    Count
};
inline constexpr const char* kReasons[] = { "none",    "extent",  "multisampled", "tie",        "direction", "state",
                                            "cleared", "aliased", "other-queue",  "unobserved", "accepted" };
enum class Placement : uint8_t
{
    Unknown,
    Committed,
    Placed
};
enum class StateSource : uint8_t
{
    Unknown,
    Creation,
    Barrier
};
struct Shape
{
    uint32_t width = 0, height = 0, format = 0, samples = 1;
    bool texture2D = true, singleSlice = true;
    Placement placement = Placement::Unknown;
};
struct Resource
{
    Id id = 0;
    Shape shape {};
    uint64_t heap = 0;
    uint32_t state = 0, viewFormat = 0;
    StateSource source = StateSource::Unknown;
    int direction = -1;
    float clearValue = 0;
    bool lifetime = false, destroyed = false, bound = false, unobserved = false, otherQueue = false;
    uint64_t queue = 0, bind = 0, use = 0, clear = 0, alias = 0, draws = 0;
    int64_t boundNs = 0;
};
enum class Kind : uint8_t
{
    Bind,
    Draw,
    Clear,
    Transition,
    Alias,
    Unsupported,
    Enhanced
};
struct Event
{
    Kind kind = Kind::Unsupported;
    Id id = 0;
    uint64_t count = 0;
    uint32_t before = 0, after = 0, viewFormat = 0;
    float depth = 0;
    bool complete = false; // full clear / whole transition, no split flags
    int64_t timeNs = 0;
};
struct Choice
{
    Reason reason = Reason::None;
    Resource pick {}, runner {};
    bool Accepted() const { return reason == Reason::Accepted; }
};
struct Frame
{
    Choice r1 {}, r2 {};
    uint32_t width = 0, height = 0;
    uint64_t epoch = 0, commits = 0, calls = 0, sampledNs = 0, samples = 0;
    int64_t nowNs = 0;
    std::array<Id, kResources> bound {}, extent {};
    std::array<Placement, kResources> placement {};
    size_t boundCount = 0, extentCount = 0;
    bool eligible = false;
    Id truth = 0;
    bool inverted = false;
};
inline Reason Refusal(const Resource& r, uint64_t queue, bool unknown)
{
    if (!r.lifetime || unknown)
        return Reason::Unobserved;
    if (r.direction < 0)
        return Reason::Direction;
    if (r.source == StateSource::Unknown)
        return Reason::State;
    if (r.clear > r.use)
        return Reason::Cleared;
    if (r.alias > r.use)
        return Reason::Aliased;
    if (r.otherQueue || r.queue != queue)
        return Reason::OtherQueue;
    if (!r.lifetime || r.unobserved || unknown)
        return Reason::Unobserved;
    return Reason::Accepted;
}
class Model
{
    std::array<Resource, kResources> _resources {};
    uint64_t _serial = 0, _next = 0, _epoch = 0;

  public:
    Resource* Find(Id id)
    {
        if (!id)
            return nullptr;
        auto& r = _resources[(id - 1) % kResources];
        return r.id == id ? &r : nullptr;
    }
    Id Create(const Shape& shape, bool lifetime, bool known, uint32_t state, uint64_t heap = 0)
    {
        // Bounded, do not evict a live identity. Advancing by kResources preserves the slot and changes generation.
        for (size_t i = 0; i < kResources; ++i)
        {
            if (_next == UINT64_MAX)
                return 0;
            const Id id = ++_next;
            auto& r = _resources[(id - 1) % kResources];
            if (r.id)
                continue;
            r = {};
            r.id = id;
            r.shape = shape;
            r.lifetime = lifetime;
            r.state = state;
            r.heap = heap;
            r.source = known ? StateSource::Creation : StateSource::Unknown;
            return id;
        }
        return 0;
    }
    void Forget(Id id)
    {
        if (auto* r = Find(id))
        {
            r->lifetime = false;
            r->destroyed = true;
        }
    }
    void UnknownSubmission()
    {
        // Keep identities/candidates; unseen commands may have changed any state or clear direction.
        for (auto& r : _resources)
            if (r.id)
            {
                r.source = StateSource::Unknown;
                r.direction = -1;
            }
    }
    void Apply(const Event& e, uint64_t queue)
    {
        ++_serial;
        if (e.kind == Kind::Alias && !e.id)
        {
            for (auto& r : _resources)
                if (r.id)
                    r.alias = _serial;
            return;
        }
        auto* r = Find(e.id);
        if (!r)
            return;
        if (r->queue && r->queue != queue)
            r->otherQueue = true;
        if (!r->queue)
            r->queue = queue;
        switch (e.kind)
        {
        case Kind::Bind:
            r->bound = true;
            r->bind = _serial;
            r->boundNs = e.timeNs;
            r->viewFormat = e.viewFormat;
            break;
        case Kind::Draw:
            r->use = _serial;
            r->draws += std::min(e.count, UINT64_MAX - r->draws);
            break;
        case Kind::Clear:
            r->clear = _serial;
            r->draws = 0;
            r->clearValue = e.depth;
            r->direction = e.complete && e.depth == 0.0f ? 1 : e.complete && e.depth == 1.0f ? 0 : -1;
            break;
        case Kind::Transition:
            if (e.complete && r->source != StateSource::Unknown && r->state == e.before)
            {
                r->state = e.after;
                r->source = StateSource::Barrier;
            }
            else
                r->source = StateSource::Unknown;
            break;
        case Kind::Alias:
            r->alias = _serial;
            // Aliasing either member of a known placed heap invalidates its other candidates too.
            if (r->heap)
                for (auto& peer : _resources)
                    if (peer.id && peer.heap == r->heap)
                        peer.alias = _serial;
            break;
        case Kind::Enhanced:
            r->source = StateSource::Unknown;
            break;
        case Kind::Unsupported:
            r->unobserved = true;
            break;
        }
    }
    Frame Present(uint32_t width, uint32_t height, uint64_t queue, bool unknown, int64_t nowNs)
    {
        Frame f;
        f.width = width;
        f.height = height;
        f.epoch = ++_epoch;
        f.nowNs = nowNs;
        bool base = false, extent = false, sampled = false;
        for (const auto& r : _resources)
        {
            if (!r.id || !r.bound)
                continue;
            f.placement[f.boundCount] = r.shape.placement;
            f.bound[f.boundCount++] = r.id;
            const bool size = r.shape.width == width && r.shape.height == height;
            if (size)
                f.extent[f.extentCount++] = r.id;
            if (!r.shape.texture2D || !r.shape.singleSlice)
                continue;
            base = true;
            if (!size)
                continue;
            extent = true;
            if (r.shape.samples != 1)
                continue;
            sampled = true;
            if (!f.r1.pick.id || r.bind > f.r1.pick.bind)
                f.r1.pick = r;
            if (!f.r2.pick.id || r.draws > f.r2.pick.draws)
            {
                f.r2.runner = f.r2.pick;
                f.r2.pick = r;
            }
            else if (!f.r2.runner.id || r.draws > f.r2.runner.draws)
                f.r2.runner = r;
        }
        const auto absent = !base ? Reason::None : !extent ? Reason::Extent : Reason::Multisampled;
        if (!sampled)
            f.r1.reason = f.r2.reason = unknown ? Reason::Unobserved : absent;
        else
        {
            f.r1.reason = Refusal(f.r1.pick, queue, unknown);
            f.r2.reason = !f.r2.pick.draws || f.r2.runner.draws > f.r2.pick.draws / 2
                              ? Reason::Tie
                              : Refusal(f.r2.pick, queue, unknown);
            // Unknown-list amendment overrides candidate-local confidence: the entire interval is unobserved.
            if (unknown)
                f.r1.reason = f.r2.reason = Reason::Unobserved;
            if (!f.r2.pick.lifetime)
                f.r2.reason = Reason::Unobserved;
        }
        // The R1 detail also describes the R2 winner/runner even if off-extent; it never makes that resource eligible.
        Resource detail;
        for (const auto& r : _resources)
            if (r.id && r.bound && r.id != f.r1.pick.id && (!detail.id || r.draws > detail.draws))
                detail = r;
        f.r1.runner = detail;
        for (auto& r : _resources)
        {
            r.bound = false;
            r.draws = 0;
            r.bind = r.use = r.clear = r.alias = 0;
            r.unobserved = false;
            if (r.destroyed)
                r = {};
        }
        return f;
    }
};
struct Recording
{
    std::array<Event, kEvents> events; // no braces: Event's members all carry initializers (see ListSlot)
    size_t size = 0;
    Id bound = 0;
    bool known = false, closed = false, overflow = false;
    uint64_t stamp = 0;
    void Reset(uint64_t token, bool success)
    {
        size = 0;
        bound = 0;
        known = success;
        closed = false;
        overflow = false;
        stamp = token;
    }
    void Add(Event e)
    {
        if (!known || closed)
        {
            overflow = true;
            return;
        }
        if (e.kind == Kind::Draw && size && events[size - 1].kind == Kind::Draw && events[size - 1].id == e.id)
        {
            auto& n = events[size - 1].count;
            n += std::min(e.count, UINT64_MAX - n);
            return;
        }
        if (size == events.size())
        {
            overflow = true;
            return;
        }
        events[size++] = e;
    }
    bool Usable(uint64_t token) const { return known && closed && !overflow && stamp && stamp == token; }
};
inline std::string FormatName(uint32_t f)
{
    switch (f)
    {
    case 39:
        return "R32_TYPELESS";
    case 40:
        return "D32_FLOAT";
    case 19:
        return "R32G8X24_TYPELESS";
    case 20:
        return "D32_FLOAT_S8X24_UINT";
    case 44:
        return "R24G8_TYPELESS";
    case 45:
        return "D24_UNORM_S8_UINT";
    case 53:
        return "R16_TYPELESS";
    case 55:
        return "D16_UNORM";
    default:
        return "format " + std::to_string(f);
    }
}
inline std::string PickLine(const Frame& f)
{
    const auto& r = f.r1.pick;
    std::ostringstream out;
    out.imbue(std::locale::classic());
    out << "DLSS-NR present depth, would pick " << r.shape.width << 'x' << r.shape.height << ' '
        << FormatName(r.shape.format) << " (DSV " << FormatName(r.viewFormat) << "), "
        << (r.shape.placement == Placement::Committed ? "committed"
            : r.shape.placement == Placement::Placed  ? "placed"
                                                      : "unknown placement")
        << ", " << r.shape.samples << " sample: cleared to " << std::fixed << std::setprecision(1) << r.clearValue
        << (r.direction == 1 ? " (reversed)" : " (forward)") << ", state "
        << (r.state == 16   ? "DEPTH_WRITE"
            : r.state == 32 ? "DEPTH_READ"
                            : "bits " + std::to_string(r.state))
        << " (" << (r.source == StateSource::Creation ? "creation" : "barrier") << ", present queue), " << r.draws
        << " draws while bound writable (runner-up ";
    const auto& other = f.r1.runner;
    if (!other.id)
        out << "none";
    else
        out << other.shape.width << 'x' << other.shape.height << ' ' << FormatName(other.shape.format) << " at "
            << other.draws
            << (other.shape.width == f.width && other.shape.height == f.height ? ", at the backbuffer's extent"
                                                                               : ", not at the backbuffer's extent");
    out << "), last bound " << double(std::max<int64_t>(0, f.nowNs - r.boundNs)) / 1000000.0
        << " ms before present; backbuffer " << f.width << 'x' << f.height;
    return out.str();
}
struct Score
{
    uint64_t comparisons = 0, matches = 0, directions = 0;
};
class Window
{
    int64_t _start = -1;
    bool _said = false, _ever = false;
    uint64_t _signature = 0, _frames = 0, _commits = 0, _pick = 0, _agree = 0, _changed = 0;
    uint64_t _calls = 0, _ns = 0, _samples = 0;
    std::array<uint64_t, size_t(Reason::Count)> _refuse {}, _r2Refuse {};
    std::array<Id, kResources * 4> _bound {}, _extent {};
    size_t _bounds = 0, _extents = 0;
    std::array<size_t, 3> _placements {};
    Score _r1 {}, _r2 {};
    Resource _previous {}, _alternate {};
    static bool SameShape(const Resource& a, const Resource& b)
    {
        return a.shape.width == b.shape.width && a.shape.height == b.shape.height && a.shape.format == b.shape.format &&
               a.direction == b.direction;
    }
    static bool Unique(std::array<Id, kResources * 4>& values, size_t& count, Id id)
    {
        if (std::find(values.begin(), values.begin() + count, id) == values.begin() + count && count < values.size())
        {
            values[count++] = id;
            return true;
        }
        return false;
    }
    static void Compare(Score& s, const Choice& c, const Frame& f)
    {
        if (f.eligible && c.Accepted())
        {
            ++s.comparisons;
            if (c.pick.id == f.truth)
            {
                ++s.matches;
                if (c.pick.direction == int(f.inverted))
                    ++s.directions;
            }
        }
    }
    void Clear(int64_t now)
    {
        _start = now;
        _frames = _commits = _pick = _agree = _changed = _calls = _ns = _samples = 0;
        _refuse = {};
        _r2Refuse = {};
        _bounds = _extents = 0;
        _placements = {};
        _r1 = {};
        _r2 = {};
    }

  public:
    // Pure clock-injected 2..30-second window. Nothing emitted before any writable depth bind.
    bool Add(const Frame& f, std::string* line, std::string* changed)
    {
        if (_start < 0 || f.nowNs < _start)
            Clear(f.nowNs);
        _ever |= f.boundCount != 0;
        ++_frames;
        _commits += f.commits;
        _calls += f.calls;
        _ns += f.sampledNs;
        _samples += f.samples;
        ++_refuse[size_t(f.r1.reason)];
        ++_r2Refuse[size_t(f.r2.reason)];
        for (size_t i = 0; i < f.boundCount; ++i)
            if (Unique(_bound, _bounds, f.bound[i]))
                ++_placements[size_t(f.placement[i])];
        for (size_t i = 0; i < f.extentCount; ++i)
            Unique(_extent, _extents, f.extent[i]);
        if (f.r1.Accepted())
        {
            ++_pick;
            if (f.r2.Accepted() && f.r1.pick.id == f.r2.pick.id)
                ++_agree;
            const auto& p = f.r1.pick;
            if (_previous.id && p.id != _previous.id)
            {
                if (!(p.id == _alternate.id && SameShape(p, _previous)))
                    ++_changed;
                _alternate = _previous;
            }
            if (p.id != _previous.id || !SameShape(p, _previous) || p.source != _previous.source ||
                p.state != _previous.state || p.viewFormat != _previous.viewFormat)
                *changed = PickLine(f);
            _previous = p;
        }
        Compare(_r1, f.r1, f);
        Compare(_r2, f.r2, f);
        const int64_t elapsed = f.nowNs - _start;
        if (!_ever || elapsed < 2000000000LL)
            return false;
        uint64_t signature = 0;
        for (size_t i = 0; i < size_t(Reason::Count); ++i)
        {
            if (_refuse[i])
                signature |= 1ull << i;
            if (_r2Refuse[i])
                signature |= 1ull << (i + 12);
        }
        const auto category = [](uint64_t n, uint64_t d) { return n == 0 ? 0ull : n == d ? 2ull : 1ull; };
        signature |= category(_agree, _pick) << 24;
        signature |= category(_r1.matches, _r1.comparisons) << 26;
        signature |= category(_r2.matches, _r2.comparisons) << 28;
        signature |= category(_r1.directions, _r1.comparisons) << 30;
        signature |= category(_r2.directions, _r2.comparisons) << 32;
        signature |= (_commits != 0 ? 1ull : 0ull) << 34;
        signature |= uint64_t(std::min<size_t>(_bounds, 1023)) << 35;
        signature |= uint64_t(std::min<size_t>(_extents, 1023)) << 45;
        if (_said && signature == _signature && elapsed < 30000000000LL)
            return false;
        std::ostringstream out;
        out.imbue(std::locale::classic());
        out << "DLSS-NR present depth, candidate scan (log only, nothing used), D3D12 present: " << _frames
            << " presents in " << std::fixed << std::setprecision(1) << double(elapsed) / 1e9 << " s, " << _commits
            << " upscaler captures";
        if (_commits || _r1.comparisons || _r2.comparisons)
            out << " | the upscaler's depth: R1 picked " << _r1.matches << " of " << _r1.comparisons
                << " eligible comparisons, direction agrees " << _r1.directions << "; R2 picked " << _r2.matches
                << " of " << _r2.comparisons << " eligible comparisons, direction agrees " << _r2.directions;
        out << " | depth targets bound " << _bounds << ", " << _extents << " at the backbuffer's extent"
            << " (committed " << _placements[size_t(Placement::Committed)] << ", placed "
            << _placements[size_t(Placement::Placed)] << ", unknown placement "
            << _placements[size_t(Placement::Unknown)] << ")"
            << " | would pick " << _pick << ", last bound and most draws agree " << _agree << " | would refuse "
            << _frames - _pick << ':';
        for (size_t i = 0; i < size_t(Reason::Accepted); ++i)
            out << ' ' << kReasons[i] << ' ' << _refuse[i];
        out << " | R2 would pick " << _r2Refuse[size_t(Reason::Accepted)] << ", refuse:";
        for (size_t i = 0; i < size_t(Reason::Accepted); ++i)
            out << ' ' << kReasons[i] << ' ' << _r2Refuse[i];
        out << " | pick changed " << _changed << " | observer " << _calls / _frames << " calls a frame, "
            << std::setprecision(3)
            << (_samples ? double(_ns) * double(_calls) / double(_samples) / double(_frames) / 1e6 : 0.0)
            << " ms a frame (sampled, " << _samples << " calls)";
        if (_bounds == _bound.size() || _extents == _extent.size())
            out << " | distinct-target counter saturated";
        *line = out.str();
        _said = true;
        _signature = signature;
        Clear(f.nowNs);
        return true;
    }
};
} // namespace DlssNr::PresentDepth

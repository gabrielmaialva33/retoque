#pragma once

// Zero-motion detection (docs/dlssnr/design/zero-motion-detection.md): whether the motion vectors a game handed over
// are a field nobody writes. Two readings in a row of a probe whose every motion sample is exactly zero while the
// depth read with it moved more than TAA jitter can explain, and the field is called dead; the first probe with one
// non-zero sample, or a probe of another shape, and it is not. While it is dead, a frame with the game's guides drops
// the model's history as ZeroGuideReset drops it on zero guides, and only when that key is on.
//
// The GPU half (dlssnr_motion_probe.hlsl, DlssNr_MotionProbe_Dx12) writes a kGrid x kGrid grid over the game's
// region: per cell, the depth at its centre, that depth's largest step to a direct neighbour, and how many of a
// kMotionTaps x kMotionTaps lattice of motion samples are not exactly zero. This header turns grids into the verdict.
//
// Pure C++ (stdlib only): tests/nr-zero-motion drives it on the host. The renderer feeds it readbacks in the order
// they were recorded; nothing here knows about a device.

#include <cmath>
#include <cstdint>
#include <format>
#include <optional>
#include <string>

namespace DlssNr::MotionProbe
{
// The GPU's grid: kGrid x kGrid cells, each (depth, depth slope, non-zero motion samples, motion samples) as RGBA32F.
constexpr unsigned kGrid = 32;
constexpr unsigned kCells = kGrid * kGrid;
constexpr unsigned kGridFloats = kCells * 4;
// Motion samples per cell, per axis.
constexpr unsigned kMotionTaps = 4;

// One probe every kInterval evaluates with the game's guides, so a slow pan has that many frames to move depth past
// the jitter bound. A comparison with a probe more than kMaxGapIntervals intervals older is not made: the route
// paused or skipped in between, and the two probes show different moments.
constexpr uint64_t kInterval = 8;
constexpr uint64_t kMaxGapIntervals = 4;

// A sample moved when its depth changed by more than kJitterPixels of its local slope (sub-pixel jitter cannot do
// that) and by more than kRelative of itself (float noise).
constexpr float kJitterPixels = 2.0f;
constexpr float kRelative = 1e-4f;

// A moving scene: at least kMovedPercent of the samples that can tell moved, of at least kMinTelling that can.
constexpr unsigned kMinTelling = 64;
constexpr unsigned kMovedPercent = 50;

// Readings of an all-zero field in a moving scene before it is called dead.
constexpr unsigned kReadingsForVerdict = 2;

// Whether this evaluate (counted from 0 over the evaluates with the game's guides) is probed.
constexpr bool Due(uint64_t evaluate) { return evaluate % kInterval == 0; }

// Where the shader reads, the same integer expressions as dlssnr_motion_probe.hlsl: a cell's centre and a motion
// tap's position inside an extent, both in [0, extent) for any extent > 0.
constexpr uint32_t Centre(uint32_t cell, uint32_t extent) { return ((2u * cell + 1u) * extent) / (2u * kGrid); }
constexpr uint32_t TapAt(uint32_t cell, uint32_t tap, uint32_t extent)
{
    return (((cell * kMotionTaps + tap) * 2u + 1u) * extent) / (2u * kGrid * kMotionTaps);
}

// What is probed. Another shape is another buffer, or the same one read differently: readings and verdict start over.
struct Shape
{
    uint32_t depthWidth = 0;
    uint32_t depthHeight = 0;
    uint32_t depthBaseX = 0;
    uint32_t depthBaseY = 0;
    uint32_t motionWidth = 0;
    uint32_t motionHeight = 0;
    uint32_t motionBaseX = 0;
    uint32_t motionBaseY = 0;
    bool motionLowResolution = false;

    bool operator==(const Shape&) const = default;
};

// One probe, read out of its grid.
struct Probe
{
    uint64_t seq = 0;           // the evaluate it was recorded at
    bool valid = false;         // every cell carried its motion count
    uint32_t nonZeroMotion = 0; // motion samples that are not exactly (0, 0): NaN and infinity included
    uint32_t motionSamples = 0; // motion samples read
    float depth[kCells] = {};   // the depth at each cell's centre
    float slope[kCells] = {};   // that depth's largest step to a direct neighbour
};

inline void FromGrid(const float* grid, uint64_t seq, Probe& out)
{
    out = Probe {};
    out.seq = seq;
    out.valid = grid != nullptr;
    if (grid == nullptr)
        return;

    for (unsigned i = 0; i < kCells; ++i)
    {
        const float* texel = grid + i * 4;
        out.depth[i] = texel[0];
        out.slope[i] = texel[1];

        // Counts are small whole numbers in a float; anything else is not a grid this shader wrote.
        const float moving = texel[2];
        const float read = texel[3];
        if (!(read == static_cast<float>(kMotionTaps * kMotionTaps)) || !(moving >= 0.0f) || !(moving <= read))
        {
            out.valid = false;
            continue;
        }
        out.nonZeroMotion += static_cast<uint32_t>(moving);
        out.motionSamples += static_cast<uint32_t>(read);
    }
}

// How the depth changed between two probes of one shape.
struct DepthChange
{
    unsigned telling = 0; // samples that can tell: finite in both, and not a flat, unchanged neighbourhood
    unsigned moved = 0;   // of those, the ones that moved past jitter

    bool Moving() const { return telling >= kMinTelling && moved * 100u >= telling * kMovedPercent; }
};

inline DepthChange CompareDepth(const Probe& before, const Probe& now)
{
    DepthChange change;
    for (unsigned i = 0; i < kCells; ++i)
    {
        const float d0 = before.depth[i], d1 = now.depth[i];
        const float g0 = before.slope[i], g1 = now.slope[i];
        if (!std::isfinite(d0) || !std::isfinite(d1) || !std::isfinite(g0) || !std::isfinite(g1))
            continue;
        // Sky, a far-plane clear, a wall square to a still camera: the same value with nothing around it to move
        // into. It cannot say whether the camera moved.
        if (d0 == d1 && g0 == 0.0f && g1 == 0.0f)
            continue;

        ++change.telling;
        const float step = std::fabs(d1 - d0);
        const float slope = std::fmax(std::fabs(g0), std::fabs(g1));
        const float scale = std::fmax(std::fabs(d0), std::fabs(d1));
        if (step > kJitterPixels * slope && step > kRelative * scale)
            ++change.moved;
    }
    return change;
}

// One comparison of a probe with the one before it.
struct Reading
{
    bool allZero = false;
    bool compared = false; // a previous probe of the same shape, recent enough, was there to compare with
    DepthChange depth;

    bool Counts() const { return allZero && compared && depth.Moving(); }
};

enum class Event : uint8_t
{
    None,
    Dead,     // the verdict formed on this probe
    Alive,    // a non-zero sample ended it
    Reshaped, // what is probed changed shape while it held: judged afresh
};

class Detector
{
  public:
    // A probe, in the order probes were recorded. Returns what changed, for the log.
    Event Add(const Shape& shape, const Probe& probe)
    {
        if (!probe.valid || probe.motionSamples == 0)
            return Event::None;

        if (!_shape || !(*_shape == shape))
        {
            const bool was = _dead;
            _shape = shape;
            _dead = false;
            _streak = 0;
            _last = probe;
            _haveLast = true;
            _reading = Reading {};
            return was ? Event::Reshaped : Event::None;
        }

        Reading reading;
        reading.allZero = probe.nonZeroMotion == 0;
        reading.compared = _haveLast && probe.seq > _last.seq && probe.seq - _last.seq <= kInterval * kMaxGapIntervals;
        if (reading.compared)
            reading.depth = CompareDepth(_last, probe);
        _reading = reading;
        _last = probe;
        _haveLast = true;

        if (!reading.allZero)
        {
            _streak = 0;
            if (_dead)
            {
                _dead = false;
                return Event::Alive;
            }
            return Event::None;
        }

        // An all-zero field in a still scene is an honest one: it neither counts nor breaks the count.
        if (reading.Counts())
            ++_streak;

        if (!_dead && _streak >= kReadingsForVerdict)
        {
            _dead = true;
            return Event::Dead;
        }
        return Event::None;
    }

    bool Dead() const { return _dead; }
    unsigned Streak() const { return _streak; }
    const Reading& LastReading() const { return _reading; }

    void Reset()
    {
        _shape.reset();
        _dead = false;
        _streak = 0;
        _haveLast = false;
        _reading = Reading {};
    }

  private:
    std::optional<Shape> _shape;
    Probe _last;
    bool _haveLast = false;
    bool _dead = false;
    unsigned _streak = 0;
    Reading _reading;
};

// Whether a frame drops the model's history for a dead field: while the verdict holds, on a frame with the game's
// guides, when ZeroGuideReset asks for it on zero guides. A reset the game asked for that frame stays the game's.
constexpr bool PolicyReset(bool dead, bool gameGuides, bool zeroGuideReset, bool gameReset)
{
    return dead && gameGuides && zeroGuideReset && !gameReset;
}

inline std::string EventLine(Event event, const Reading& reading, bool zeroGuideReset)
{
    switch (event)
    {
    case Event::Dead:
        return std::format("DLSS-NR motion probe: the game's motion vectors read all zero in {} readings of a moving "
                           "scene ({} of {} depth samples moved past jitter), so they are not motion. {}",
                           kReadingsForVerdict, reading.depth.moved, reading.depth.telling,
                           zeroGuideReset ? "The model's history is dropped on every frame while they stay zero, as "
                                            "ZeroGuideReset does on zero guides."
                                          : "The model keeps its history and may ghost while the camera moves; "
                                            "ZeroGuideReset=true drops it while they stay zero.");
    case Event::Alive:
        return "DLSS-NR motion probe: the game's motion vectors carry motion again; the model keeps its history";
    case Event::Reshaped:
        return "DLSS-NR motion probe: the guides changed shape while the motion vectors read all zero; judging the "
               "new ones afresh";
    case Event::None:
        break;
    }
    return {};
}
} // namespace DlssNr::MotionProbe

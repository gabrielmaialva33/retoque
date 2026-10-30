#pragma once

// Scene-cut detection (docs/dlssnr/design/scene-cut-detection.md): whether the game's frame cut to another picture
// between two consecutive presents, for a game that did not say so with its own reset flag. A missed cut warps the
// model's history of one picture into the next; the game's flag is the authority, and this is only for its absence.
//
// The input is a 16x16 grid of the game's frame's mean luminance per tile -- the grid the pulse pass already writes
// (dlssnr_pulse.hlsl, channel 1 over channel 2), or a pass of the same shape. Never the pass's own output: a statistic
// of what this pass writes is a feedback loop. Per frame pair, three numbers:
//
// - incoherence: the tiles' ranks (their order by light, 0..1) compared with the previous frame's moved by the best
//   global shift of up to two tiles, in half-tile steps; the mean change of rank there. Ranks do not move under any
//   change of light that keeps order -- exposure, a white flash, a fade, a tone curve -- and the shift lines a pan up.
//   A cut reorders the tiles: two unrelated orders differ by a third on average.
// - breadth: the share of the overlapping tiles whose rank moved more than a quarter. A cut changes almost all of
//   them; an explosion or a muzzle flash changes the part of the frame it lights.
// - distribution: how far the two frames' distributions of light moved, in stops, after each is centred on its
//   median (1-Wasserstein over quantiles). It says the light really changed shape, which noise and grain in a dark or
//   flat frame, the cases where ranks reorder for no reason, do not.
//
// A pair is a cut when all three pass their threshold. Around that, hysteresis: the first kWarmupPairs comparisons are
// not believed, nothing is a cut within kCooldownFrames of the last cut or of a reset the game asked for, frames are
// compared only when consecutive and under kGapMs apart, and a frame with no picture (black, flat, or noise) is never
// compared. A cut found on a readback arrives frames late; Pending turns it into at most one request for a later
// frame, within kMaxLateFrames, and says what it did when the request is off (shadow mode).
//
// Pure C++ (stdlib only): tests/nr-scene-cut drives it on the host. Phase 1: nothing in the renderer calls it yet.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <optional>
#include <string>

namespace DlssNr::SceneCut
{
// The grid: kGrid x kGrid tiles. The pulse pass's grid is four floats a tile: (luminance out, luminance in, pixels
// read, 0).
constexpr unsigned kGrid = 16;
constexpr unsigned kTiles = kGrid * kGrid;
constexpr unsigned kPulseGridFloats = kTiles * 4;

// A frame needs this many tiles that measured something finite; fewer and it cannot be compared.
constexpr unsigned kMinTiles = kTiles * 3 / 4;
// Tiles darker than this far below the frame's mean are held at it: log2 of a black tile is -inf, and ten stops
// down is black for every purpose here.
constexpr double kFloorStops = 10.0;
// No picture: tiles spread less than this (mean |log2 luminance - median|, in stops) -- a black or flat frame -- or
// neighbouring tiles' ranks differ by more than this on average, which is noise (unrelated ranks differ by a third)
// rather than a picture, whose light is smooth at this scale.
constexpr double kBlankStops = 0.05;
constexpr double kNoiseRoughness = 0.22;

// Global shifts tried, in half tiles each way: up to two tiles, an eighth of the frame, per frame.
constexpr int kMaxShiftHalfTiles = 4;
// A shift is scored only where it leaves this many tiles overlapping.
constexpr unsigned kMinOverlap = kTiles / 2;

// A tile changed when its rank moved by more than this.
constexpr double kTileChanged = 0.25;

// The cut: all three. Calibrated on synthetic sequences only (tests/nr-scene-cut prints the margins); the design
// note says what real captures must show before any of them is trusted.
constexpr double kCutIncoherence = 0.10;
constexpr double kCutBreadth = 0.06;
constexpr double kCutDistributionStops = 0.05;
// ... and the light changed both ways: a burst, a muzzle flash, a lamp switched on or off changes the tiles it lights
// in one direction, a new picture changes them in both. Each tile's change against the frame's median change counts
// by how far it went past kLightChangedStops; polarity is the brightening share of that, and a cut needs it inside
// [kCutPolarity, 1 - kCutPolarity]. Weighed by size, not counted: a burst's few stops outweigh the grain and the
// misregistration around it, which a count of tiles does not.
constexpr double kLightChangedStops = 0.5;
constexpr double kCutPolarity = 0.05;

// Comparisons not believed after a start or Reset.
constexpr uint64_t kWarmupPairs = 3;
// Frames after a cut, or a reset the game asked for, in which no cut is believed: half a second at 60 fps.
constexpr uint64_t kCooldownFrames = 30;
// No comparison across a pause this long; the cadence's and the pulse metric's own gap rule.
constexpr int64_t kGapMs = 250;
// A request is honoured up to this many frames after the cut; later, the model has rebuilt its history already.
constexpr uint64_t kMaxLateFrames = 4;

// One frame, read out of its grid.
struct Frame
{
    bool valid = false;       // enough tiles measured
    bool blank = false;       // no picture: black, flat or noise
    unsigned tiles = 0;       // tiles measured
    double median = 0.0;      // of log2 luminance
    double spread = 0.0;      // mean |log2 luminance - median|, stops
    double roughness = 0.0;   // mean |rank difference| of neighbouring tiles
    float level[kTiles] = {}; // log2 luminance minus the median; NaN where the tile measured nothing
    float rank[kTiles] = {};  // 0..1 by light, ties sharing their mean; NaN where the tile measured nothing
};

// From per-tile mean luminance, in any unit: only ratios and order are used. A NaN, infinite or negative tile was not
// measured.
inline Frame FromMeans(const float* means)
{
    Frame f;
    for (unsigned i = 0; i < kTiles; ++i)
    {
        f.level[i] = NAN;
        f.rank[i] = NAN;
    }
    if (means == nullptr)
        return f;

    double sum = 0.0;
    for (unsigned i = 0; i < kTiles; ++i)
    {
        if (std::isfinite(means[i]) && means[i] >= 0.0f)
        {
            ++f.tiles;
            sum += means[i];
        }
    }
    if (f.tiles < kMinTiles)
        return f;
    f.valid = true;

    const double mean = sum / f.tiles;
    if (!(mean > 0.0) || !std::isfinite(mean))
    {
        f.blank = true;
        return f;
    }

    const double floor = mean * std::exp2(-kFloorStops);
    double log2Luma[kTiles];
    unsigned order[kTiles];
    unsigned n = 0;
    for (unsigned i = 0; i < kTiles; ++i)
    {
        if (!(std::isfinite(means[i]) && means[i] >= 0.0f))
            continue;
        log2Luma[i] = std::log2(std::fmax(static_cast<double>(means[i]), floor));
        order[n++] = i;
    }
    std::sort(order, order + n, [&](unsigned a, unsigned b)
              { return log2Luma[a] < log2Luma[b] || (log2Luma[a] == log2Luma[b] && a < b); });

    f.median = (n % 2 == 1) ? log2Luma[order[n / 2]] : 0.5 * (log2Luma[order[n / 2 - 1]] + log2Luma[order[n / 2]]);

    double spread = 0.0;
    for (unsigned k = 0; k < n; ++k)
    {
        const unsigned i = order[k];
        f.level[i] = static_cast<float>(log2Luma[i] - f.median);
        spread += std::fabs(log2Luma[i] - f.median);
    }
    f.spread = spread / n;

    // Ranks, ties sharing the mean of the places they hold.
    for (unsigned k = 0; k < n;)
    {
        unsigned end = k + 1;
        while (end < n && log2Luma[order[end]] == log2Luma[order[k]])
            ++end;
        const float r = static_cast<float>((0.5 * (k + end - 1)) / (n - 1));
        for (unsigned j = k; j < end; ++j)
            f.rank[order[j]] = r;
        k = end;
    }

    double rough = 0.0;
    unsigned pairs = 0;
    for (unsigned y = 0; y < kGrid; ++y)
    {
        for (unsigned x = 0; x < kGrid; ++x)
        {
            const float r = f.rank[y * kGrid + x];
            if (std::isnan(r))
                continue;
            if (x + 1 < kGrid && !std::isnan(f.rank[y * kGrid + x + 1]))
            {
                rough += std::fabs(r - f.rank[y * kGrid + x + 1]);
                ++pairs;
            }
            if (y + 1 < kGrid && !std::isnan(f.rank[(y + 1) * kGrid + x]))
            {
                rough += std::fabs(r - f.rank[(y + 1) * kGrid + x]);
                ++pairs;
            }
        }
    }
    f.roughness = pairs > 0 ? rough / pairs : 0.0;
    f.blank = f.spread < kBlankStops || f.roughness > kNoiseRoughness;
    return f;
}

// From the pulse pass's grid: the game's frame only, luminance in over pixels read. A tile that read no pixels was
// not measured.
inline Frame FromPulseGrid(const float* grid)
{
    if (grid == nullptr)
        return FromMeans(nullptr);

    float means[kTiles];
    for (unsigned i = 0; i < kTiles; ++i)
    {
        const float* tile = grid + i * 4;
        means[i] = tile[2] > 0.0f ? tile[1] / tile[2] : NAN;
    }
    return FromMeans(means);
}

// One frame pair, compared.
struct Scores
{
    bool compared = false;     // both frames measured and with a picture
    double incoherence = 0.0;  // mean |rank change| at the best shift
    double breadth = 0.0;      // share of overlapping tiles whose rank moved past kTileChanged, at that shift
    double distribution = 0.0; // stops: how far the median-centred distributions of light moved
    double polarity = 0.5;     // of the tiles whose light changed against the frame's, the share that brightened
    int shiftX = 0;            // the best shift, in half tiles: the previous frame is read this far along
    int shiftY = 0;
    unsigned overlap = 0; // tiles compared at that shift

    bool Cut() const
    {
        return compared && incoherence >= kCutIncoherence && breadth >= kCutBreadth &&
               distribution >= kCutDistributionStops && polarity >= kCutPolarity && polarity <= 1.0 - kCutPolarity;
    }
};

namespace Detail
{
// A per-tile field (a frame's ranks or levels) at a position in half tiles, bilinear between the tiles around it;
// false outside the grid or next to a tile that measured nothing.
inline bool At(const float* field, int x2, int y2, double& out)
{
    const int x0 = x2 >= 0 ? x2 / 2 : (x2 - 1) / 2;
    const int y0 = y2 >= 0 ? y2 / 2 : (y2 - 1) / 2;
    const bool halfX = (x2 - 2 * x0) != 0;
    const bool halfY = (y2 - 2 * y0) != 0;
    const int x1 = halfX ? x0 + 1 : x0;
    const int y1 = halfY ? y0 + 1 : y0;
    if (x0 < 0 || y0 < 0 || x1 >= static_cast<int>(kGrid) || y1 >= static_cast<int>(kGrid))
        return false;

    const float a = field[y0 * kGrid + x0], b = field[y0 * kGrid + x1];
    const float c = field[y1 * kGrid + x0], d = field[y1 * kGrid + x1];
    if (std::isnan(a) || std::isnan(b) || std::isnan(c) || std::isnan(d))
        return false;
    out = 0.25 * (static_cast<double>(a) + b + c + d);
    return true;
}

// The level at quantile p of a frame's measured tiles, from its levels sorted ascending.
inline double Quantile(const double* sorted, unsigned n, double p)
{
    const double at = p * (n - 1);
    const unsigned lo = static_cast<unsigned>(at);
    const unsigned hi = std::min(lo + 1, n - 1);
    return sorted[lo] + (sorted[hi] - sorted[lo]) * (at - lo);
}

inline unsigned SortedLevels(const Frame& f, double* out)
{
    unsigned n = 0;
    for (unsigned i = 0; i < kTiles; ++i)
    {
        if (!std::isnan(f.level[i]))
            out[n++] = f.level[i];
    }
    std::sort(out, out + n);
    return n;
}
} // namespace Detail

inline Scores Compare(const Frame& before, const Frame& now)
{
    Scores s;
    if (!before.valid || !now.valid || before.blank || now.blank)
        return s;

    bool found = false;
    for (int sy = -kMaxShiftHalfTiles; sy <= kMaxShiftHalfTiles; ++sy)
    {
        for (int sx = -kMaxShiftHalfTiles; sx <= kMaxShiftHalfTiles; ++sx)
        {
            double sum = 0.0;
            unsigned overlap = 0, changed = 0;
            for (unsigned y = 0; y < kGrid; ++y)
            {
                for (unsigned x = 0; x < kGrid; ++x)
                {
                    const float r = now.rank[y * kGrid + x];
                    double was = 0.0;
                    if (std::isnan(r) ||
                        !Detail::At(before.rank, 2 * static_cast<int>(x) + sx, 2 * static_cast<int>(y) + sy, was))
                        continue;
                    const double step = std::fabs(r - was);
                    sum += step;
                    ++overlap;
                    changed += step > kTileChanged ? 1u : 0u;
                }
            }
            if (overlap < kMinOverlap)
                continue;

            const double incoherence = sum / overlap;
            // Ties go to the smaller shift: a still frame is not a pan.
            const bool better =
                !found || incoherence < s.incoherence ||
                (incoherence == s.incoherence && std::abs(sx) + std::abs(sy) < std::abs(s.shiftX) + std::abs(s.shiftY));
            if (better)
            {
                found = true;
                s.incoherence = incoherence;
                s.breadth = static_cast<double>(changed) / overlap;
                s.shiftX = sx;
                s.shiftY = sy;
                s.overlap = overlap;
            }
        }
    }
    if (!found)
        return s;

    // Which way the light went, at that shift. Each tile's change in stops, less the frame's median change (the
    // exposure, a flash over everything); then the brightening share of what went past kLightChangedStops, by size.
    double change[kTiles];
    unsigned n = 0;
    for (unsigned y = 0; y < kGrid; ++y)
    {
        for (unsigned x = 0; x < kGrid; ++x)
        {
            const float l = now.level[y * kGrid + x];
            double was = 0.0;
            if (std::isnan(l) ||
                !Detail::At(before.level, 2 * static_cast<int>(x) + s.shiftX, 2 * static_cast<int>(y) + s.shiftY, was))
                continue;
            change[n++] = (l + now.median) - (was + before.median);
        }
    }
    double sortedChange[kTiles];
    std::copy(change, change + n, sortedChange);
    std::sort(sortedChange, sortedChange + n);
    const double medianChange = Detail::Quantile(sortedChange, n, 0.5);
    double up = 0.0, down = 0.0;
    for (unsigned k = 0; k < n; ++k)
    {
        const double d = change[k] - medianChange;
        up += std::fmax(d - kLightChangedStops, 0.0);
        down += std::fmax(-d - kLightChangedStops, 0.0);
    }
    s.polarity = (up + down) > 0.0 ? up / (up + down) : 0.5;

    double a[kTiles], b[kTiles];
    const unsigned na = Detail::SortedLevels(before, a);
    const unsigned nb = Detail::SortedLevels(now, b);
    constexpr unsigned kQuantiles = 64;
    double distance = 0.0;
    for (unsigned q = 0; q < kQuantiles; ++q)
    {
        const double p = (q + 0.5) / kQuantiles;
        distance += std::fabs(Detail::Quantile(a, na, p) - Detail::Quantile(b, nb, p));
    }
    s.distribution = distance / kQuantiles;
    s.compared = true;
    return s;
}

enum class Outcome : uint8_t
{
    Ignored,    // not newer than a frame already fed: a late readback; neither compared nor kept
    Unpaired,   // nothing to compare with: the first frame, or after a hole in the sequence or a pause
    Unmeasured, // too little of the frame measured: breaks the chain
    Blank,      // this frame or the one before has no picture
    Same,       // compared: the same scene
    Cut,        // compared: a cut
    Warmup,     // a cut by the scores, among the first kWarmupPairs comparisons: not believed
    Cooldown,   // a cut by the scores, within kCooldownFrames of the last cut or game reset: not believed
    GameReset,  // the game asked for a reset on this frame: its own, whatever the scores say
};

struct Verdict
{
    Outcome outcome = Outcome::Unpaired;
    uint64_t seq = 0;
    Scores scores {};
};

// Since the last Reset. "cuts" are believed cuts; warmup and cooldown are cuts by the scores that were not.
struct Counters
{
    uint64_t frames = 0;
    uint64_t compared = 0;
    uint64_t cuts = 0;
    uint64_t warmup = 0;
    uint64_t cooldown = 0;
    uint64_t gameResets = 0;
    uint64_t blank = 0;
};

// Pairs consecutive frames and keeps the hysteresis. `seq` counts presents at which the pass was asked to run, measured
// or not (the pulse metric's sequence), so a frame that could not be measured breaks the pair.
class Detector
{
    Frame _last {};
    uint64_t _lastSeq = 0;
    int64_t _lastMs = 0;
    bool _haveAny = false;
    bool _haveLast = false;
    std::optional<uint64_t> _lastReset; // the last believed cut or game reset
    Counters _counts {};

  public:
    // What is measured changed -- the frame's size or format, the device, the feature: start over, warm-up included.
    void Reset()
    {
        _last = Frame {};
        _haveAny = false;
        _haveLast = false;
        _lastReset.reset();
        _counts = {};
    }

    Verdict Add(uint64_t seq, int64_t ms, const Frame& frame, bool gameReset)
    {
        Verdict v;
        v.seq = seq;
        if (_haveAny && seq <= _lastSeq)
        {
            v.outcome = Outcome::Ignored;
            return v;
        }

        const bool paired = _haveLast && seq == _lastSeq + 1 && ms >= _lastMs && ms - _lastMs <= kGapMs;
        _haveAny = true;
        _lastSeq = seq;
        _lastMs = ms;
        ++_counts.frames;

        if (gameReset)
        {
            ++_counts.gameResets;
            _lastReset = seq;
        }

        if (!frame.valid)
        {
            _haveLast = false;
            v.outcome = gameReset ? Outcome::GameReset : Outcome::Unmeasured;
            return v;
        }

        const Frame before = _last;
        _last = frame;
        _haveLast = true;

        if (!paired)
        {
            v.outcome = gameReset ? Outcome::GameReset : Outcome::Unpaired;
            return v;
        }
        if (frame.blank || before.blank)
        {
            ++_counts.blank;
            v.outcome = gameReset ? Outcome::GameReset : Outcome::Blank;
            return v;
        }

        v.scores = Compare(before, frame);
        ++_counts.compared;
        if (gameReset)
        {
            v.outcome = Outcome::GameReset;
            return v;
        }
        if (!v.scores.Cut())
        {
            v.outcome = Outcome::Same;
            return v;
        }
        if (_counts.compared <= kWarmupPairs)
        {
            ++_counts.warmup;
            v.outcome = Outcome::Warmup;
            return v;
        }
        if (_lastReset && seq - *_lastReset < kCooldownFrames)
        {
            ++_counts.cooldown;
            v.outcome = Outcome::Cooldown;
            return v;
        }

        ++_counts.cuts;
        _lastReset = seq;
        v.outcome = Outcome::Cut;
        return v;
    }

    const Counters& Counts() const { return _counts; }
};

// What became of a believed cut, at the frame it was taken.
enum class Request : uint8_t
{
    None,        // nothing armed, or not yet a frame after the cut
    Reset,       // drop the model's history on this frame
    Shadow,      // would have: the request is off, so only said
    Late,        // too many frames after the cut: the model has rebuilt its history on the new scene
    GameAlready, // the game asked for a reset on this frame itself
};

// A cut found on a readback, held until the first frame after it that reads the reset. At most one request per cut;
// a newer cut replaces an older one.
class Pending
{
    std::optional<uint64_t> _cut;

  public:
    void Arm(uint64_t cutSeq) { _cut = cutSeq; }
    bool Armed() const { return _cut.has_value(); }
    void Clear() { _cut.reset(); }

    // Once per frame, where the frame's reset is read. `enabled` is the request's key; `gameReset` the game's own flag.
    Request Take(uint64_t nowSeq, bool enabled, bool gameReset)
    {
        if (!_cut || nowSeq <= *_cut)
            return Request::None;
        const uint64_t cut = *_cut;
        _cut.reset();
        if (gameReset)
            return Request::GameAlready;
        if (nowSeq - cut > kMaxLateFrames)
            return Request::Late;
        return enabled ? Request::Reset : Request::Shadow;
    }
};

// The log's line for a believed cut and what became of it.
inline std::string LogLine(const Verdict& v, Request request)
{
    std::string line =
        std::format("DLSS-NR scene cut: the game's frame cut at present {} (incoherence {:.2f}, breadth {:.2f}, "
                    "distribution {:.2f} stops)",
                    v.seq, v.scores.incoherence, v.scores.breadth, v.scores.distribution);
    switch (request)
    {
    case Request::Reset:
        line += "; the model's history is dropped on the next frame that reads it";
        break;
    case Request::Shadow:
        line += "; history kept, SceneCutReset is off (shadow mode)";
        break;
    case Request::Late:
        line += "; found too late to act on, history kept";
        break;
    case Request::GameAlready:
        line += "; the game asked for a reset there itself";
        break;
    case Request::None:
        break;
    }
    return line;
}
} // namespace DlssNr::SceneCut

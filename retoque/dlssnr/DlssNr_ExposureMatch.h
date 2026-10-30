#pragma once

// Which exposure scan candidate follows the scene's light the way an exposure does ([DlssNr] ScanMatch,
// docs/dlssnr/design/exposure-scan-match.md). Opt-in; off, the scan picks by "widest travel" exactly as before.
//
// Adapted from janblade's OptiScaler-F5-DLSSNR-Multipass (GPL-3.0), DlssNr_ExposureMatch.h at 0a80f61d, 3e25cc36,
// 0357684c and b003b7e8: the fit, the rejections and their thresholds are his, measured on Cyberpunk 2077, Doom Eternal
// and NBA 2K27 recordings (tests/nr-exposure-match/data holds trimmed copies). What differs here is the yardstick, the
// recorder that feeds it and what a match is used for -- see the design note.
//
// The yardstick is a meter: kMiddleGrey over the game's frame's mean luminance with its pre-exposure divided out
// (Meter), the frame read off the pulse pass's grid (DlssNr_Pulse.h, MeanInputLuma). An exposure multiplier in the
// same sense as janblade's Automatic reading -- bigger = a darker scene -- so his recordings are fitted unchanged. It
// is the game's frame, never this pass's output: a statistic of what the pass writes is a feedback loop.
//
// Against it, on log scales:
// - slope ~ +1: the candidate is a multiplier;
// - slope ~ -1: a divisor (a white-point-style value, or an adapted luminance);
// - linear in log2 of the meter: a stop value (log2, ln or log10, either sign).
// The fit is tried at a range of row lags, because a game's eye adaptation trails the meter's raw reading. A game may
// adapt only part of the way (a slope down to Settings::minExposureSlope) and clamp at a floor or ceiling; the rows a
// candidate spends on such a plateau, or frozen while the game is paused, are left out of its fit (detail::Plateaus).
//
// Not an exposure, rejected before any fit: Steady (changes in under 2% of rows), NotFloat (integer bits read as a
// float), Timer (rises or falls on almost every changing row), Noisy (jumps across most of its range row to row). And
// after the fit: a divisor or falling stop value at a lag of a row or less is the scene's luminance -- the meter's own
// input -- not an exposure the game adapted (Luminance).
//
// Confidence: nothing before minRows rows with a reading and minTravelEv of meter travel over minLevels brightness
// levels; the fit must hold in each half of the recording that has light to show, and a best lag at either end of the
// searched range is no fit. A meter that barely moves while a candidate swings is the other answer: the game exposed
// the colour itself (Baked).
//
// Pure C++ (stdlib only): tests/nr-exposure-match drives it on synthetic traces and the recordings.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace DlssNr::ExposureMatch
{
enum class Kind
{
    Unsure,     // too few rows with a reading to say
    Steady,     // changes in under 2% of rows
    NotFloat,   // integer bits, not a float value
    Timer,      // counts up or down
    Noisy,      // jumps across its range row to row
    Unrelated,  // moves smoothly, but not with the light
    Multiplier, // an exposure: slope ~ +1 in log-log
    Divisor,    // slope ~ -1 in log-log
    Stops,      // linear in log2 of the meter: a stop value; `scale` = its stops per log2 stop of the meter
    Luminance   // a divisor or falling stop value at lag 0-1: the scene's luminance, the meter's own input
};

struct Fit
{
    Kind kind = Kind::Unsure;
    float slope = 0.0f; // log-log for Multiplier/Divisor, value per log2 stop for Stops
    float corr = 0.0f;  // signed
    int lag = 0;        // rows the candidate trails the meter by
    float scale = 0.0f; // Stops only: +-1 (log2), +-ln 2 (ln), +-log10 2 (log10)
};

enum class What
{
    NotSure, // not enough rows or brightness variety yet
    Found,   // `index` is the exposure, `fit` its units
    Baked,   // the colour is already exposed: the meter is flat while `index` swings
    None     // enough variety, nothing follows the light
};

struct Verdict
{
    What what = What::NotSure;
    int index = -1;
    Fit fit;
    size_t rows = 0;            // rows with a meter reading
    float meterTravelEv = 0.0f; // 5th..95th percentile of the meter, in stops
};

// janblade's values, kept: his recordings run at about 10 rows a second, the scan's recorder at about 7.5 at 60 fps
// (kRowFrames), so a row lag here is a little longer in time and the lag range a little wider.
struct Settings
{
    size_t minRows = 200;          // ~25 s at the recorder's ~7.5 rows a second
    float minTravelEv = 2.0f;      // meter travel before units can be told
    float halfTravelEv = 1.0f;     // a half with less than this is not asked to agree
    int minLevels = 4;             // brightness levels visited (detail::Levels), for the meter and a baked candidate
    float minCorr = 0.9f;          // whole recording
    float halfCorr = 0.8f;         // each half
    float bakedMeterEv = 0.5f;     // meter at most this flat ...
    float bakedCandidateEv = 2.0f; // ... while a candidate swings at least this much
    int minLag = -5;               // rows; a little lead is allowed, a reading may arrive a frame late
    int maxLag = 30;               // ~3-4 s of eye adaptation
    // A multiplier's or divisor's log-log slope against the meter. Under 1: the game adapts only part of the way (Doom
    // Eternal ~0.5-0.6); under ~0.3 is a stop value with a large offset, fitted as one.
    float minExposureSlope = 0.3f;
    float maxExposureSlope = 1.5f;
    size_t frozenRows = 20; // rows holding one exact value: paused, not following (detail::Plateaus)
    // A best lag at either end of [minLag, maxLag] is no fit. Off for the halves Match asks to agree.
    bool edgeLagRejects = true;
};

// The meter: middle grey over the frame's scene luminance (its mean luminance with the game's pre-exposure divided
// out). 0 -- no reading -- for a black, empty or non-finite frame; the matcher leaves such rows out.
constexpr float kMiddleGrey = 0.18f;

inline float Meter(double meanLuma, float preExposure)
{
    const double pre = std::isfinite(preExposure) && preExposure > 1e-6f ? (double) preExposure : 1.0;
    const double scene = meanLuma / pre;
    if (!std::isfinite(scene) || !(scene > 1e-9))
        return 0.0f;
    const double m = kMiddleGrey / scene;
    return std::isfinite(m) && m < 1e12 ? (float) m : 0.0f;
}

namespace detail
{
inline float Percentile(std::vector<float> v, float p)
{
    if (v.empty())
        return 0.0f;
    const size_t i = std::min(v.size() - 1, (size_t) (p * (float) (v.size() - 1) + 0.5f));
    std::nth_element(v.begin(), v.begin() + (std::ptrdiff_t) i, v.end());
    return v[i];
}

inline float SpreadEv(const std::vector<float>& logs) { return Percentile(logs, 0.95f) - Percentile(logs, 0.05f); }

// Least squares of y on x with y shifted `lag` rows later; slope and correlation, 0 when either side is flat. Rows of y
// marked in `skip` are left out.
inline void Regress(const std::vector<float>& x, const std::vector<float>& y, int lag, float& slope, float& corr,
                    const std::vector<char>* skip = nullptr)
{
    slope = 0.0f;
    corr = 0.0f;
    const int n = (int) std::min(x.size(), y.size());
    const int from = std::max(0, lag), to = std::min(n, n + lag);
    if (to - from < 3)
        return;

    double sx = 0, sy = 0, sxx = 0, syy = 0, sxy = 0;
    int used = 0;
    for (int i = from; i < to; ++i)
    {
        if (skip != nullptr && (*skip)[(size_t) i])
            continue;
        const double a = x[(size_t) (i - lag)], b = y[(size_t) i];
        sx += a;
        sy += b;
        sxx += a * a;
        syy += b * b;
        sxy += a * b;
        ++used;
    }
    if (used < 3)
        return;
    const double m = used;
    const double vx = sxx - sx * sx / m, vy = syy - sy * sy / m, cxy = sxy - sx * sy / m;
    if (vx <= 1e-12 || vy <= 1e-12)
        return;
    slope = (float) (cxy / vx);
    corr = (float) (cxy / std::sqrt(vx * vy));
}

inline void BestLag(const std::vector<float>& x, const std::vector<float>& y, const Settings& s, float& slope,
                    float& corr, int& lag, const std::vector<char>* skip = nullptr)
{
    slope = corr = 0.0f;
    lag = 0;
    for (int l = s.minLag; l <= s.maxLag; ++l)
    {
        float sl, r;
        Regress(x, y, l, sl, r, skip);
        if (std::fabs(r) > std::fabs(corr) + 1e-6f ||
            (std::fabs(r) > std::fabs(corr) - 1e-6f && std::abs(l) < std::abs(lag)))
        {
            slope = sl;
            corr = r;
            lag = l;
        }
    }
}

// The rows where a series does not follow anything: where it sits at its own lowest or highest value, when that value
// holds at least 5% of the rows (a clamped exposure), and runs of at least `frozenRows` rows holding one exact value
// (the game paused, in a menu or a fade). Nothing is marked when that would leave under 60% of the rows: a value that
// is mostly still is a stepped one, and fitting only its moves picks whatever changes when a scene does.
inline std::vector<char> Plateaus(const std::vector<float>& y, size_t frozenRows)
{
    std::vector<char> skip(y.size(), 0);
    if (y.empty())
        return skip;

    const auto [lo, hi] = std::minmax_element(y.begin(), y.end());
    const float low = *lo, high = *hi;
    if (!(high > low))
        return skip;

    const size_t atLow = (size_t) std::count(y.begin(), y.end(), low);
    const size_t atHigh = (size_t) std::count(y.begin(), y.end(), high);
    const bool skipLow = atLow * 20 >= y.size(), skipHigh = atHigh * 20 >= y.size();

    for (size_t i = 0; i < y.size(); ++i)
        skip[i] = (skipLow && y[i] == low) || (skipHigh && y[i] == high) ? 1 : 0;

    for (size_t from = 0; from < y.size();)
    {
        size_t to = from + 1;
        while (to < y.size() && y[to] == y[from])
            ++to;
        if (frozenRows > 0 && to - from >= frozenRows)
            std::fill(skip.begin() + (std::ptrdiff_t) from, skip.begin() + (std::ptrdiff_t) to, (char) 1);
        from = to;
    }

    const size_t left = (size_t) std::count(skip.begin(), skip.end(), (char) 0);
    if (left * 5 < y.size() * 3)
        std::fill(skip.begin(), skip.end(), (char) 0);

    return skip;
}

// A stop value's scale: its slope against log2 of the meter, snapped to log2 / ln / log10 when within 25%.
inline float SnapScale(float slope)
{
    const float sign = slope < 0.0f ? -1.0f : 1.0f;
    const float a = std::fabs(slope);
    if (!(a > 0.0f))
        return 0.0f;
    float best = 0.0f, bestDist = 1e9f;
    for (float c : { 1.0f, 0.69314718f, 0.30103f })
    {
        const float d = std::fabs(std::log(a / c));
        if (d < bestDist)
        {
            bestDist = d;
            best = c;
        }
    }
    return bestDist <= std::log(1.25f) ? sign * best : 0.0f;
}

// Classify one candidate on rows that all carry a meter reading: logMeter = log2 of the reading.
inline Fit ClassifyRows(const std::vector<float>& logMeter, const std::vector<float>& v, const Settings& s)
{
    Fit f;
    const size_t n = std::min(v.size(), logMeter.size());
    if (n < 20)
        return f;

    size_t nonzero = 0, broken = 0, positive = 0;
    for (size_t i = 0; i < n; ++i)
    {
        const float a = v[i];
        if (a != 0.0f)
            ++nonzero;
        if (!std::isfinite(a) || (a != 0.0f && (std::fabs(a) < 1e-30f || std::fabs(a) > 1e30f)))
            ++broken;
        if (a > 0.0f)
            ++positive;
    }
    if (broken * 10 > nonzero && broken > 0)
    {
        f.kind = Kind::NotFloat;
        return f;
    }

    size_t changes = 0, ups = 0, downs = 0;
    std::vector<float> steps;
    steps.reserve(n);
    for (size_t i = 1; i < n; ++i)
    {
        if (v[i] != v[i - 1])
        {
            ++changes;
            (v[i] > v[i - 1] ? ups : downs) += 1;
        }
        steps.push_back(std::fabs(v[i] - v[i - 1]));
    }
    if (changes * 50 < n - 1)
    {
        f.kind = Kind::Steady;
        return f;
    }
    if (changes * 2 >= n - 1 && (ups * 50 >= changes * 49 || downs * 50 >= changes * 49))
    {
        f.kind = Kind::Timer;
        return f;
    }
    const std::vector<float> head(v.begin(), v.begin() + (std::ptrdiff_t) n);
    const float range = Percentile(head, 0.95f) - Percentile(head, 0.05f);
    if (!(range > 0.0f))
    {
        f.kind = Kind::Steady;
        return f;
    }
    if (Percentile(steps, 0.5f) > 0.2f * range)
    {
        f.kind = Kind::Noisy;
        return f;
    }

    // Log-log, when the value is a positive quantity (a few unread zeros allowed).
    float logSlope = 0, logCorr = 0;
    int logLag = 0;
    const bool canLog = positive * 20 >= n * 19;
    if (canLog)
    {
        std::vector<float> x, y;
        x.reserve(n);
        y.reserve(n);
        for (size_t i = 0; i < n; ++i)
        {
            // An unread 0 repeats the value before it, so the rows stay aligned for the lag.
            if (v[i] <= 0.0f && y.empty())
                continue;
            x.push_back(logMeter[i]);
            y.push_back(v[i] > 0.0f ? std::log2(v[i]) : y.back());
        }
        const std::vector<char> skip = Plateaus(y, s.frozenRows);
        BestLag(x, y, s, logSlope, logCorr, logLag, &skip);
    }

    // Linear in log2 of the meter: a stop value.
    float linSlope = 0, linCorr = 0;
    int linLag = 0;
    const std::vector<char> linSkip = Plateaus(head, s.frozenRows);
    BestLag(logMeter, head, s, linSlope, linCorr, linLag, &linSkip);

    // Each fit's own verdict: log-log as a multiplier or divisor (a slope in the exposure band), linear as a stop value
    // (a slope that snaps to a log base). Both hold: the tighter fit wins. One holds: that is the answer. Neither:
    // Unrelated, reported with the tighter fit. A best lag at either end of the range is no fit: the best may lie past
    // it (janblade, 3e25cc36: Cyberpunk after a quality change fitted at lag -5 with a level three times the real one).
    const auto inside = [&s](int lag) { return !s.edgeLagRejects || (lag > s.minLag && lag < s.maxLag); };
    const bool logHolds = canLog && std::fabs(logCorr) >= s.minCorr && std::fabs(logSlope) >= s.minExposureSlope &&
                          std::fabs(logSlope) <= s.maxExposureSlope && inside(logLag);
    const float scale = SnapScale(linSlope);
    const bool linHolds = std::fabs(linCorr) >= s.minCorr && scale != 0.0f && inside(linLag);

    bool useLin;
    if (logHolds && linHolds)
        useLin = std::fabs(linCorr) > std::fabs(logCorr) + 0.01f;
    else if (logHolds || linHolds)
        useLin = linHolds;
    else
        useLin = !canLog || std::fabs(linCorr) > std::fabs(logCorr);

    if (useLin)
    {
        f.slope = linSlope;
        f.corr = linCorr;
        f.lag = linLag;
        f.kind = linHolds ? (scale < 0.0f && std::abs(linLag) <= 1 ? Kind::Luminance : Kind::Stops) : Kind::Unrelated;
        if (f.kind == Kind::Stops)
            f.scale = scale;
        return f;
    }

    f.slope = logSlope;
    f.corr = logCorr;
    f.lag = logLag;
    f.kind = !logHolds               ? Kind::Unrelated
             : logSlope > 0.0f       ? Kind::Multiplier
             : std::abs(logLag) <= 1 ? Kind::Luminance
                                     : Kind::Divisor;
    return f;
}

// How many brightness levels a log2 series visits: 0.5 EV bins holding at least 2% of its rows. One cut between two
// levels moves the travel but proves nothing about units.
inline int Levels(const std::vector<float>& logs)
{
    if (logs.empty())
        return 0;

    const float low = *std::min_element(logs.begin(), logs.end());
    std::vector<size_t> bins;

    for (float l : logs)
    {
        const size_t bin = (size_t) ((l - low) * 2.0f);
        if (bin >= bins.size())
            bins.resize(bin + 1, 0);
        ++bins[bin];
    }

    int levels = 0;
    for (size_t count : bins)
        levels += count * 50 >= logs.size() ? 1 : 0;
    return levels;
}

inline bool IsMatch(Kind k) { return k == Kind::Multiplier || k == Kind::Divisor || k == Kind::Stops; }
} // namespace detail

// One candidate against the meter, row by row. Rows without a meter reading (0) are left out.
inline Fit Classify(const std::vector<float>& meter, const std::vector<float>& value, const Settings& s = {})
{
    std::vector<float> lm, v;
    for (size_t i = 0; i < meter.size() && i < value.size(); ++i)
        if (meter[i] > 0.0f && std::isfinite(meter[i]))
        {
            lm.push_back(std::log2(meter[i]));
            v.push_back(value[i]);
        }
    return detail::ClassifyRows(lm, v, s);
}

inline Verdict Match(const std::vector<float>& meter, const std::vector<std::vector<float>>& candidates,
                     const Settings& s = {})
{
    using namespace detail;
    Verdict out;

    std::vector<size_t> rows;
    std::vector<float> lm;
    for (size_t i = 0; i < meter.size(); ++i)
        if (meter[i] > 0.0f && std::isfinite(meter[i]))
        {
            rows.push_back(i);
            lm.push_back(std::log2(meter[i]));
        }
    out.rows = rows.size();
    out.meterTravelEv = SpreadEv(lm);
    if (rows.size() < s.minRows)
        return out;

    auto column = [&](const std::vector<float>& c, size_t from, size_t to)
    {
        std::vector<float> v;
        v.reserve(to - from);
        for (size_t k = from; k < to; ++k)
            v.push_back(rows[k] < c.size() ? c[rows[k]] : 0.0f);
        return v;
    };

    // Flat meter: either the game already exposed the colour (a candidate swings), or nothing to go on.
    if (out.meterTravelEv < s.bakedMeterEv)
    {
        float widest = 0.0f;
        for (size_t c = 0; c < candidates.size(); ++c)
        {
            const std::vector<float> v = column(candidates[c], 0, rows.size());
            const Kind k = ClassifyRows(lm, v, s).kind;
            if (k != Kind::Unrelated && !IsMatch(k))
                continue;
            std::vector<float> logs;
            for (float a : v)
                if (a > 0.0f)
                    logs.push_back(std::log2(a));
            if (logs.size() * 20 < v.size() * 19)
                continue;
            const float swing = SpreadEv(logs);
            if (swing >= s.bakedCandidateEv && Levels(logs) >= s.minLevels && swing > widest)
            {
                widest = swing;
                out.what = What::Baked;
                out.index = (int) c;
            }
        }
        return out;
    }
    if (out.meterTravelEv < s.minTravelEv || Levels(lm) < s.minLevels)
        return out;

    const size_t half = rows.size() / 2;
    const std::vector<float> lm1(lm.begin(), lm.begin() + (std::ptrdiff_t) half),
        lm2(lm.begin() + (std::ptrdiff_t) half, lm.end());
    const bool ask1 = SpreadEv(lm1) >= s.halfTravelEv, ask2 = SpreadEv(lm2) >= s.halfTravelEv;
    Settings halfSettings = s;
    halfSettings.minCorr = s.halfCorr;
    halfSettings.edgeLagRejects = false;

    out.what = What::None;
    for (size_t c = 0; c < candidates.size(); ++c)
    {
        const Fit f = ClassifyRows(lm, column(candidates[c], 0, rows.size()), s);
        if (!IsMatch(f.kind) || (out.index >= 0 && std::fabs(f.corr) <= std::fabs(out.fit.corr)))
            continue;
        if (ask1 && ClassifyRows(lm1, column(candidates[c], 0, half), halfSettings).kind != f.kind)
            continue;
        if (ask2 && ClassifyRows(lm2, column(candidates[c], half, rows.size()), halfSettings).kind != f.kind)
            continue;
        out.what = What::Found;
        out.index = (int) c;
        out.fit = f;
    }
    return out;
}

namespace detail
{
// One candidate's rows from row `from` on, against the meter's same rows.
inline Verdict MatchFrom(const std::vector<float>& meter, const std::vector<float>& candidate, size_t from,
                         const Settings& s)
{
    from = std::min(from, meter.size());
    const std::vector<float> m(meter.begin() + (std::ptrdiff_t) from, meter.end());
    std::vector<float> v;
    v.reserve(m.size());
    for (size_t i = from; i < meter.size(); ++i)
        v.push_back(i < candidate.size() ? candidate[i] : 0.0f);
    return Match(m, { v }, s);
}
} // namespace detail

// Match over candidates adopted at different times, as the scan takes them: candidate c has values from row first[c]
// on. Each is judged on its own rows: Found, the tightest of those found; else Baked; else None once every candidate
// had enough rows and light to say; else NotSure. Two found candidates are compared on the rows both have, so one made
// late and judged on a few hundred lucky rows does not beat the exposure judged on thousands; when the shared rows
// cannot say, the longer history wins. Baked needs the meter flat over the whole recording.
inline Verdict MatchEach(const std::vector<float>& meter, const std::vector<std::vector<float>>& candidates,
                         const std::vector<size_t>& first, const Settings& s = {})
{
    using namespace detail;

    auto from = [&](size_t c) { return c < first.size() ? std::min(first[c], meter.size()) : (size_t) 0; };

    std::vector<float> lm;
    for (float a : meter)
        if (a > 0.0f && std::isfinite(a))
            lm.push_back(std::log2(a));
    const bool flat = SpreadEv(lm) < s.bakedMeterEv;

    Verdict best, baked;
    bool allSaid = !candidates.empty();

    for (size_t c = 0; c < candidates.size(); ++c)
    {
        Verdict one = MatchFrom(meter, candidates[c], from(c), s);
        if (one.index >= 0)
            one.index = (int) c;

        if (one.what == What::Found)
        {
            bool wins = best.what != What::Found;

            if (!wins)
            {
                const size_t shared = std::max(from(c), from((size_t) best.index));
                const Verdict mine = MatchFrom(meter, candidates[c], shared, s);
                const Verdict theirs = MatchFrom(meter, candidates[(size_t) best.index], shared, s);

                if (mine.what == What::Found && theirs.what == What::Found)
                    wins = std::fabs(mine.fit.corr) > std::fabs(theirs.fit.corr);
                else if (mine.what == What::Found || theirs.what == What::Found)
                    wins = mine.what == What::Found;
                else
                    wins = from(c) < from((size_t) best.index);
            }

            if (wins)
                best = one;
        }
        else if (one.what == What::Baked && flat && baked.what != What::Baked)
        {
            baked = one;
        }

        allSaid = allSaid && one.what != What::NotSure && (one.what != What::Baked || flat);
    }

    if (best.what == What::Found)
        return best;
    if (baked.what == What::Baked)
        return baked;

    Verdict out;
    out.what = allSaid ? What::None : What::NotSure;
    return out;
}

// A candidate's value as an exposure multiplier in the meter's sense (bigger = darker), up to a constant; 0 when the
// fit gives it none or the value cannot be one.
inline float AsExposure(const Fit& f, float value)
{
    if (!std::isfinite(value))
        return 0.0f;
    switch (f.kind)
    {
    case Kind::Multiplier:
        return value > 0.0f ? value : 0.0f;
    case Kind::Divisor:
        return value > 0.0f ? 1.0f / value : 0.0f;
    case Kind::Stops:
    {
        if (f.scale == 0.0f)
            return 0.0f;
        const float stops = value / f.scale;
        return stops > -120.0f && stops < 120.0f ? std::exp2(stops) : 0.0f;
    }
    default:
        return 0.0f;
    }
}

// What the scan offers for a matched candidate, from it as an exposure. A multiplier or a divisor is offered in its
// own units, as "widest travel" offers a candidate, so an anchor placed on the same buffer under either rule means
// the same number. A stop value has no ratio of its own -- a ratio of two EV numbers is not a ratio of light -- so it
// is offered as the exposure multiplier it stands for. 0 for nothing usable.
inline float Offered(const Fit& f, float exposure)
{
    if (!std::isfinite(exposure) || !(exposure > 0.0f))
        return 0.0f;
    return f.kind == Kind::Divisor ? 1.0f / exposure : exposure;
}

// The flash fix (janblade, 3e25cc36): a match at a lag of one row or less, leading ones included, moves with the
// frame's raw light -- the game's per-frame target before its own eye adaptation, which jumps up to ~1 EV between
// frames when the camera pans and stops. That one is eased, in stops, faster toward a brighter scene than a darker one
// as eye adaptation is; a match that trails the meter is the game's eased value already and is used as it is.
inline bool NeedsEasing(const Fit& f) { return f.lag <= 1; }

// One frame of easing in log2 of the exposure. `now` 0 = nothing yet: start at the target.
inline float Ease(float now, float target, float brighterFrames, float darkerFrames)
{
    if (!std::isfinite(target) || !(target > 0.0f))
        return 0.0f;
    if (!std::isfinite(now) || !(now > 0.0f))
        return target;
    // A lower exposure is a brighter scene.
    const float frames = target < now ? brighterFrames : darkerFrames;
    const float blend = frames > 1.0f ? 1.0f - std::exp(-1.0f / frames) : 1.0f;
    const float l = std::log2(now) + (std::log2(target) - std::log2(now)) * blend;
    return std::exp2(l);
}

// The scan's recording: one row per sampled sweep, the meter beside every held candidate's latest value, candidates
// keyed by the scan's id and adopted at any row. Bounded: past `capacity` rows the oldest `capacity / 4` go at once.
class History
{
  public:
    explicit History(size_t capacity = 1200) : capacity_(std::max<size_t>(capacity, 8)) {}

    // `values`: (id, latest) of every candidate read so far. One not in the list this row repeats its last value.
    void AddRow(float meter, const std::vector<std::pair<unsigned, float>>& values)
    {
        const size_t row = meter_.size();
        for (const auto& [id, value] : values)
        {
            Column* c = Find(id);
            if (c == nullptr)
            {
                columns_.push_back({ id, row, std::vector<float>(row, 0.0f) });
                c = &columns_.back();
            }
            c->values.resize(row, c->values.empty() ? 0.0f : c->values.back());
            c->values.push_back(std::isfinite(value) ? value : 0.0f);
        }
        meter_.push_back(std::isfinite(meter) && meter > 0.0f ? meter : 0.0f);
        for (Column& c : columns_)
            c.values.resize(meter_.size(), c.values.empty() ? 0.0f : c.values.back());

        if (meter_.size() > capacity_)
            Drop(capacity_ / 4);
    }

    // Forget every column whose id is not in `held`: let go of by the game, released or retired by the scan.
    void Keep(const std::vector<unsigned>& held)
    {
        columns_.erase(std::remove_if(columns_.begin(), columns_.end(), [&](const Column& c)
                                      { return std::find(held.begin(), held.end(), c.id) == held.end(); }),
                       columns_.end());
    }

    void Clear()
    {
        meter_.clear();
        columns_.clear();
    }

    size_t Rows() const { return meter_.size(); }
    size_t Columns() const { return columns_.size(); }

    // Rows with a meter reading, and the meter's travel over them in stops: what the status line says before a
    // verdict.
    size_t MeteredRows() const
    {
        return (size_t) std::count_if(meter_.begin(), meter_.end(), [](float m) { return m > 0.0f; });
    }

    float TravelEv() const
    {
        std::vector<float> lm;
        for (float m : meter_)
            if (m > 0.0f)
                lm.push_back(std::log2(m));
        return detail::SpreadEv(lm);
    }

    struct Judgement
    {
        Verdict verdict;
        unsigned id = 0; // the scan's id of verdict.index, 0 for none
    };

    Judgement Judge(const Settings& s = {}) const
    {
        Judgement j;
        if (columns_.empty())
        {
            j.verdict.rows = MeteredRows();
            j.verdict.meterTravelEv = TravelEv();
            return j;
        }

        std::vector<std::vector<float>> candidates;
        std::vector<size_t> first;
        candidates.reserve(columns_.size());
        first.reserve(columns_.size());
        for (const Column& c : columns_)
        {
            candidates.push_back(c.values);
            first.push_back(c.first);
        }

        j.verdict = MatchEach(meter_, candidates, first, s);
        if (j.verdict.index >= 0 && (size_t) j.verdict.index < columns_.size())
            j.id = columns_[(size_t) j.verdict.index].id;
        j.verdict.rows = MeteredRows();
        j.verdict.meterTravelEv = TravelEv();
        return j;
    }

  private:
    struct Column
    {
        unsigned id = 0;
        size_t first = 0; // the row of its first reading
        std::vector<float> values;
    };

    Column* Find(unsigned id)
    {
        for (Column& c : columns_)
            if (c.id == id)
                return &c;
        return nullptr;
    }

    void Drop(size_t rows)
    {
        rows = std::min(rows, meter_.size());
        meter_.erase(meter_.begin(), meter_.begin() + (std::ptrdiff_t) rows);
        for (Column& c : columns_)
        {
            c.values.erase(c.values.begin(), c.values.begin() + (std::ptrdiff_t) std::min(rows, c.values.size()));
            c.first = c.first > rows ? c.first - rows : 0;
        }
    }

    size_t capacity_;
    std::vector<float> meter_;
    std::vector<Column> columns_;
};

inline const char* KindName(Kind k)
{
    switch (k)
    {
    case Kind::Multiplier:
        return "a multiplier";
    case Kind::Divisor:
        return "a divisor";
    case Kind::Stops:
        return "a stop value";
    case Kind::Luminance:
        return "the scene's luminance";
    case Kind::Steady:
        return "steady";
    case Kind::NotFloat:
        return "not a float";
    case Kind::Timer:
        return "a timer";
    case Kind::Noisy:
        return "noise";
    case Kind::Unrelated:
        return "unrelated to the light";
    default:
        return "not sure";
    }
}
} // namespace DlssNr::ExposureMatch

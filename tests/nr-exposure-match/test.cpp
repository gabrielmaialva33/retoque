// The exposure match's pure rule (retoque/dlssnr/DlssNr_ExposureMatch.h, docs/dlssnr/design/exposure-scan-match.md)
// on synthetic traces and on janblade's recordings (data/, trimmed). The synthetic scenes and most of the recording
// checks are ported from janblade's tests/nr_exposure_match_smoke.cpp (GPL-3.0); the recorder, the meter, the offered
// value and the easing are this fork's. No GPU and no game.
//
// test <folder with the recordings>

#include <dlssnr/DlssNr_ExposureMatch.h>
#include <dlssnr/DlssNr_Pulse.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace DlssNr::ExposureMatch;

namespace
{
int fails = 0;
int checks = 0;

#define CHECK(c)                                                                                                       \
    do                                                                                                                 \
    {                                                                                                                  \
        ++checks;                                                                                                      \
        if (!(c))                                                                                                      \
        {                                                                                                              \
            std::printf("FAIL line %d: %s\n", __LINE__, #c);                                                           \
            ++fails;                                                                                                   \
        }                                                                                                              \
    } while (0)

uint32_t rng = 12345;
float Rand01()
{
    rng = rng * 1664525u + 1013904223u;
    return (float) (rng >> 8) / 16777216.0f;
}

// The true exposure walks through `ev` stops and back twice, smoothly, the way a player walks between a dark interior
// and daylight; both halves of the recording see the full range.
std::vector<float> TrueExposure(size_t rows, float ev, float baseEv = -2.0f)
{
    std::vector<float> e(rows);
    for (size_t i = 0; i < rows; ++i)
    {
        const float t = (float) i / (float) rows;
        e[i] = std::exp2(baseEv + ev * 0.5f * (1.0f - std::cos(t * 4.0f * 3.14159265f)));
    }
    return e;
}

// The meter reads the true exposure with a little noise, unadapted.
std::vector<float> Metered(const std::vector<float>& e, float noiseEv = 0.15f)
{
    std::vector<float> m(e.size());
    for (size_t i = 0; i < e.size(); ++i)
        m[i] = e[i] * std::exp2((Rand01() - 0.5f) * 2.0f * noiseEv);
    return m;
}

// Eye adaptation: the game's value trails the light, eased in stops.
std::vector<float> Eased(const std::vector<float>& e, float tauRows)
{
    std::vector<float> out(e.size());
    float le = std::log2(e[0]);
    const float blend = 1.0f - std::exp(-1.0f / tauRows);
    for (size_t i = 0; i < e.size(); ++i)
    {
        le += (std::log2(e[i]) - le) * blend;
        out[i] = std::exp2(le);
    }
    return out;
}

template <class F> std::vector<float> Map(const std::vector<float>& e, F f)
{
    std::vector<float> out(e.size());
    for (size_t i = 0; i < e.size(); ++i)
        out[i] = f(e[i], i);
    return out;
}

// --- The traces the brief names: a true exposure with lag, a timer, a frozen value, a clamp, a divisor form ---------

void Traces()
{
    const size_t rows = 1200;
    const auto e = TrueExposure(rows, 6.0f);
    const auto meter = Metered(e);

    // The true exposure, adapted by the game: it trails the meter, and the lag search finds by how much.
    {
        const Fit f = Classify(meter, Eased(e, 12.0f));
        CHECK(f.kind == Kind::Multiplier);
        CHECK(f.lag >= 6 && f.lag <= 24);
        CHECK(f.corr > 0.95f);
    }
    // ... in divisor form (a white-point-style value the game adapted): slope ~ -1, still an exposure.
    {
        const Fit f = Classify(meter, Map(Eased(e, 12.0f), [](float x, size_t) { return 3.0f / x; }));
        CHECK(f.kind == Kind::Divisor);
        CHECK(std::fabs(f.slope + 1.0f) < 0.15f);
        CHECK(f.lag > 1);
    }
    // ... in a stop value, crossing zero: no log of it can be taken.
    {
        const Fit f = Classify(meter, Map(Eased(e, 8.0f), [](float x, size_t) { return std::log2(x) + 0.5f; }));
        CHECK(f.kind == Kind::Stops);
        CHECK(std::fabs(f.scale - 1.0f) < 1e-6f);
    }
    // A timer: rises on every row, whatever the light. Widest travel would take it; the match never does.
    CHECK(Classify(meter, Map(e, [](float, size_t i) { return 0.01f + 0.1f * (float) i; })).kind == Kind::Timer);
    // A frozen value: one number for the whole run.
    CHECK(Classify(meter, Map(e, [](float, size_t) { return 1.0f; })).kind == Kind::Steady);
    // A value that follows the light but freezes for a stretch (a pause: the game holds its value while the meter reads
    // a menu's near-black frames): the frozen rows are left out and the rest still fit.
    {
        auto paused = meter;
        auto value = Map(Eased(e, 6.0f), [](float x, size_t) { return 0.7f * x; });
        for (size_t i = 500; i < 800; ++i)
        {
            paused[i] = 1e5f;
            value[i] = value[499];
        }
        const Fit f = Classify(paused, value);
        CHECK(f.kind == Kind::Multiplier);
        CHECK(f.corr > 0.95f);
    }
    // A partial clamp: the game adapts part of the way and holds a floor in bright scenes (20-40% of rows). Fitted off
    // its floor, a multiplier at its own slope.
    {
        const auto partial = Map(e, [](float x, size_t) { return std::max(6.0f, 8.0f * std::pow(x, 0.4f)); });
        size_t clamped = 0;
        for (float a : partial)
            clamped += a == 6.0f ? 1 : 0;
        CHECK(clamped * 5 >= rows && clamped * 5 <= rows * 2);
        const Fit f = Classify(meter, partial);
        CHECK(f.kind == Kind::Multiplier);
        CHECK(f.slope > 0.3f && f.slope < 0.5f);
    }
    // A value clamped most of the time is a stepped one: fitting only its few moves is not allowed, and it is no match.
    {
        const auto mostly = Map(e, [](float x, size_t) { return std::clamp(x, 0.5f, 1.0f); });
        size_t held = 0;
        for (float a : mostly)
            held += a == 0.5f || a == 1.0f ? 1 : 0;
        CHECK(held * 5 > rows * 2); // more than 40% at a plateau
        const std::vector<char> skip = detail::Plateaus(mostly, 20);
        CHECK(std::count(skip.begin(), skip.end(), (char) 1) == 0);
    }
    // The scene's luminance itself (a divisor at lag 0, the meter's own input): not an exposure.
    {
        const Fit f = Classify(meter, Map(e, [](float x, size_t) { return 3.0f / x; }));
        CHECK(f.kind == Kind::Luminance);
    }
}

// --- janblade's synthetic scenes (ported) ---------------------------------------------------------------------------

void Ported()
{
    const size_t rows = 1200;
    const auto e = TrueExposure(rows, 6.0f);
    const auto meter = Metered(e);

    {
        const Fit f = Classify(meter, Map(e, [](float x, size_t) { return 0.7f * x; }));
        CHECK(f.kind == Kind::Multiplier);
        CHECK(std::fabs(f.slope - 1.0f) < 0.1f);
    }
    for (float offset : { 6.0f, 10.0f, 15.0f }) // EV100 style: a stop value with a large offset
    {
        const Fit f = Classify(meter, Map(e, [offset](float x, size_t) { return std::log2(x) + offset; }));
        CHECK(f.kind == Kind::Stops);
        CHECK(std::fabs(f.scale - 1.0f) < 1e-6f);
    }
    {
        const Fit f = Classify(meter, Map(Eased(e, 12.0f), [](float x, size_t) { return -std::log(x); }));
        CHECK(f.kind == Kind::Stops);
        CHECK(std::fabs(f.scale + std::log(2.0f)) < 1e-6f);
    }
    CHECK(Classify(meter, Map(e, [](float x, size_t) { return -std::log(x); })).kind == Kind::Luminance);

    // A best lag at the end of the searched range is no fit; inside it is.
    {
        auto leading = [&](size_t rowsAhead)
        { return Map(e, [&](float, size_t i) { return e[std::min(i + rowsAhead, e.size() - 1)]; }); };
        CHECK(Classify(meter, leading(8)).kind == Kind::Unrelated);
        const Fit inside = Classify(meter, leading(3));
        CHECK(inside.kind == Kind::Multiplier && inside.lag == -3);
    }

    CHECK(Classify(meter, Map(e, [](float, size_t) { return Rand01(); })).kind == Kind::Noisy);
    CHECK(Classify(meter, Map(e, [](float, size_t i) { return i < 600 ? 1.0f : 1.3195f; })).kind == Kind::Steady);
    CHECK(Classify(meter, Map(e, [](float, size_t i) { return 1e-38f * (float) (1 + i % 7); })).kind == Kind::NotFloat);
    {
        const auto other = TrueExposure(rows, 3.0f, 0.0f);
        std::vector<float> shifted(rows);
        for (size_t i = 0; i < rows; ++i)
            shifted[i] = other[(i + rows / 3) % rows] * (1.0f + 0.3f * std::sin((float) i * 0.013f));
        CHECK(Classify(meter, shifted).kind == Kind::Unrelated);
    }

    // The exposure among junk and a copy of it: the first of the best.
    {
        std::vector<std::vector<float>> c;
        c.push_back(Map(e, [](float, size_t) { return 1.0f; }));
        c.push_back(Map(e, [](float, size_t i) { return 0.1f * (float) i; }));
        c.push_back(Map(e, [](float, size_t) { return Rand01(); }));
        c.push_back(Map(e, [](float x, size_t) { return 0.5f * x; }));
        c.push_back(Map(e, [](float x, size_t) { return 0.5f * x; }));
        const Verdict v = Match(meter, c);
        CHECK(v.what == What::Found);
        CHECK(v.index == 3);
        CHECK(v.meterTravelEv > 5.0f);
    }
    {
        std::vector<std::vector<float>> c { Map(e, [](float, size_t) { return 1.0f; }),
                                            Map(e, [](float, size_t) { return Rand01(); }) };
        CHECK(Match(meter, c).what == What::None);
    }
    {
        const auto small = TrueExposure(rows, 1.0f);
        CHECK(Match(Metered(small, 0.05f), { small }).what == What::NotSure);
    }
    {
        std::vector<float> m(meter.begin(), meter.begin() + 100);
        CHECK(Match(m, { std::vector<float>(e.begin(), e.begin() + 100) }).what == What::NotSure);
    }
    {
        const auto flat = Metered(std::vector<float>(rows, 0.5f), 0.1f);
        std::vector<std::vector<float>> c { Map(e, [](float, size_t) { return 1.0f; }),
                                            Map(e, [](float, size_t) { return Rand01(); }), e };
        const Verdict v = Match(flat, c);
        CHECK(v.what == What::Baked);
        CHECK(v.index == 2);
    }
    {
        std::vector<std::vector<float>> c { Map(e, [](float x, size_t i) { return i < 600 ? x : 1.0f / x; }) };
        CHECK(Match(meter, c).what != What::Found);
    }
    {
        std::vector<float> twoLevels(rows);
        for (size_t i = 0; i < rows; ++i)
            twoLevels[i] = i < rows / 2 ? 0.25f : 4.0f;
        std::vector<std::vector<float>> c { Map(twoLevels,
                                                [](float, size_t i) { return i < rows / 2 ? 3.0f : 7.0f; }) };
        CHECK(Match(Metered(twoLevels, 0.05f), c).what == What::NotSure);
    }
    {
        std::vector<float> m = meter;
        for (size_t i = 0; i < 200; ++i)
            m[i] = 0.0f;
        const Verdict v = Match(m, { Map(e, [](float x, size_t) { return 2.0f * x; }) });
        CHECK(v.what == What::Found);
        CHECK(v.rows == rows - 200);
    }
    {
        const std::vector<float> early = Map(e, [](float, size_t i) { return 1.0f + 0.001f * (float) (i % 7); });
        std::vector<float> late = Map(e, [](float x, size_t) { return 0.7f * x; });
        for (size_t i = 0; i < 400; ++i)
            late[i] = 0.0f;
        const Verdict v = MatchEach(meter, { early, late }, { 0, 400 });
        CHECK(v.what == What::Found);
        CHECK(v.index == 1);
        CHECK(MatchEach(meter, { late }, { 1100 }).what == What::NotSure);
    }
}

// --- This fork's parts: the meter, the offered value, the easing, the recorder --------------------------------------

void Ours()
{
    // The meter: middle grey over the scene luminance, pre-exposure divided out; nothing for a black frame.
    CHECK(std::fabs(Meter(0.18, 1.0f) - 1.0f) < 1e-6f);
    CHECK(std::fabs(Meter(0.36, 2.0f) - 1.0f) < 1e-6f); // pre-exposed twice as bright: the same scene
    CHECK(Meter(0.09, 1.0f) > Meter(0.18, 1.0f));       // darker scene, bigger exposure
    CHECK(Meter(0.0, 1.0f) == 0.0f);
    CHECK(Meter(NAN, 1.0f) == 0.0f);
    CHECK(std::fabs(Meter(0.18, 0.0f) - 1.0f) < 1e-6f); // no pre-exposure reads as 1

    // Off the pulse grid: the game's frame (luminance in over pixels), never the pass's output, empty tiles left out.
    {
        std::vector<float> grid(DlssNr::Pulse::kGridFloats, 0.0f);
        for (unsigned i = 0; i < DlssNr::Pulse::kGrid * DlssNr::Pulse::kGrid; ++i)
        {
            grid[i * 4 + 0] = 1000.0f;  // the output: must not count
            grid[i * 4 + 1] = 0.5f * 4; // four pixels of 0.5
            grid[i * 4 + 2] = 4.0f;
        }
        grid[2] = 0.0f; // a tile that read nothing
        grid[1] = 1e6f;
        grid[5] = NAN; // a tile that is not finite
        CHECK(std::fabs(DlssNr::Pulse::MeanInputLuma(grid.data()) - 0.5) < 1e-9);
        CHECK(std::isnan(DlssNr::Pulse::MeanInputLuma(nullptr)));
        std::vector<float> black(DlssNr::Pulse::kGridFloats, 0.0f);
        CHECK(std::isnan(DlssNr::Pulse::MeanInputLuma(black.data())));
        CHECK(Meter(DlssNr::Pulse::MeanInputLuma(black.data()), 1.0f) == 0.0f);
    }

    // As an exposure, and as offered: a divisor keeps its own units, a stop value becomes the multiplier it stands for.
    {
        Fit stops { Kind::Stops };
        stops.scale = 1.0f;
        CHECK(std::fabs(AsExposure(stops, 3.0f) - 8.0f) < 1e-5f);
        CHECK(AsExposure(stops, 1000.0f) == 0.0f);
        CHECK(std::fabs(AsExposure({ Kind::Divisor }, 4.0f) - 0.25f) < 1e-6f);
        CHECK(AsExposure({ Kind::Unrelated }, 4.0f) == 0.0f);
        CHECK(std::fabs(Offered({ Kind::Divisor }, AsExposure({ Kind::Divisor }, 4.0f)) - 4.0f) < 1e-6f);
        CHECK(std::fabs(Offered({ Kind::Multiplier }, 2.0f) - 2.0f) < 1e-6f);
        CHECK(std::fabs(Offered(stops, AsExposure(stops, 3.0f)) - 8.0f) < 1e-5f);
        CHECK(Offered({ Kind::Multiplier }, 0.0f) == 0.0f);
    }

    // Easing: only a match that does not trail the meter; faster toward a brighter scene; starts at the target.
    {
        Fit trailing { Kind::Multiplier };
        trailing.lag = 6;
        Fit target { Kind::Multiplier };
        target.lag = 0;
        Fit leading { Kind::Multiplier };
        leading.lag = -3;
        CHECK(!NeedsEasing(trailing));
        CHECK(NeedsEasing(target));
        CHECK(NeedsEasing(leading));
        CHECK(Ease(0.0f, 2.0f, 30.0f, 90.0f) == 2.0f);
        const float brighter = Ease(4.0f, 1.0f, 30.0f, 90.0f); // two stops down in exposure
        const float darker = Ease(1.0f, 4.0f, 30.0f, 90.0f);
        CHECK(brighter < 4.0f && brighter > 1.0f);
        CHECK(darker > 1.0f && darker < 4.0f);
        CHECK(std::fabs(std::log2(4.0f / brighter)) > std::fabs(std::log2(darker / 1.0f)));
        // A one-frame flash of 1 EV, eased, moves the offered value by a small fraction of it.
        float v = 1.0f;
        v = Ease(v, 2.0f, 30.0f, 90.0f);
        v = Ease(v, 1.0f, 30.0f, 90.0f);
        CHECK(std::fabs(std::log2(v)) < 0.05f);
        CHECK(Ease(1.0f, NAN, 30.0f, 90.0f) == 0.0f);
    }

    // The recorder: candidates adopted at different rows, a let-go one forgotten, the oldest rows dropped in a block.
    {
        const size_t rows = 1600;
        const auto e = TrueExposure(rows, 6.0f);
        const auto meter = Metered(e);
        const auto adapted = Eased(e, 10.0f);
        History h(1200);
        for (size_t i = 0; i < rows; ++i)
        {
            std::vector<std::pair<unsigned, float>> values;
            values.push_back({ 7, 0.01f + 0.05f * (float) i }); // a timer, from the start
            values.push_back({ 9, 1.0f });                      // frozen
            if (i >= 300)
                values.push_back({ 12, 2.0f / adapted[i] }); // the exposure, in divisor form, adopted late
            if (i < 500)
                values.push_back({ 15, 0.5f }); // let go of at row 500
            h.AddRow(meter[i], values);
            if (i == 500)
                h.Keep({ 7, 9, 12 });
        }
        CHECK(h.Rows() <= 1200 && h.Rows() > 900);
        CHECK(h.Columns() == 3);
        const History::Judgement j = h.Judge();
        CHECK(j.verdict.what == What::Found);
        CHECK(j.id == 12);
        CHECK(j.verdict.fit.kind == Kind::Divisor);
        CHECK(j.verdict.fit.lag > 1);
        std::printf("  recorder: id %u, %s, corr %.3f, lag %d, %zu rows, %.1f EV\n", j.id, KindName(j.verdict.fit.kind),
                    j.verdict.fit.corr, j.verdict.fit.lag, j.verdict.rows, j.verdict.meterTravelEv);

        History empty;
        CHECK(empty.Judge().verdict.what == What::NotSure);
        CHECK(empty.Judge().id == 0);
    }
}

// --- Recordings (data/, trimmed copies of janblade's: the column names keep his candidate numbers) ------------------

struct Recording
{
    std::vector<std::string> columns; // after "meter"
    std::vector<float> meter;
    std::vector<std::vector<float>> candidates;
    bool ok = false;
};

Recording Load(const std::string& path)
{
    Recording r;
    std::ifstream in(path);
    if (!in)
        return r;

    std::string line;
    bool header = false;
    while (std::getline(in, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line.empty() || line[0] == '#')
            continue;

        std::vector<std::string> cells;
        std::stringstream ss(line);
        std::string cell;
        while (std::getline(ss, cell, ','))
            cells.push_back(cell);

        if (!header)
        {
            header = true;
            if (cells.empty() || cells[0] != "meter")
                return r;
            r.columns.assign(cells.begin() + 1, cells.end());
            r.candidates.resize(r.columns.size());
            continue;
        }

        r.meter.push_back(std::stof(cells[0]));
        for (size_t i = 0; i < r.columns.size(); ++i)
            r.candidates[i].push_back(i + 1 < cells.size() ? std::stof(cells[i + 1]) : 0.0f);
    }

    r.ok = !r.meter.empty() && !r.columns.empty();
    return r;
}

const std::vector<float>& Column(const Recording& r, const char* name)
{
    static const std::vector<float> none;
    for (size_t i = 0; i < r.columns.size(); ++i)
        if (r.columns[i] == name)
            return r.candidates[i];
    std::printf("FAIL: no column %s\n", name);
    ++fails;
    return none;
}

std::vector<size_t> Firsts(const Recording& r)
{
    std::vector<size_t> first(r.candidates.size(), 0);
    for (size_t c = 0; c < r.candidates.size(); ++c)
        while (first[c] < r.candidates[c].size() && r.candidates[c][first[c]] == 0.0f)
            ++first[c];
    return first;
}

void Recordings(const std::string& folder)
{
    // NBA 2K27: its handed exposure (c14) holds one value per scene while the meter swings ~5 EV; nothing follows the
    // light, and c2/c4 are integer bits.
    {
        const Recording r = Load(folder + "/nba2k27_handed.csv");
        CHECK(r.ok);
        if (r.ok)
        {
            CHECK(Classify(r.meter, Column(r, "c14")).kind == Kind::Steady);
            CHECK(Classify(r.meter, Column(r, "c15")).kind == Kind::Steady);
            CHECK(Classify(r.meter, Column(r, "c2")).kind == Kind::NotFloat);
            CHECK(Classify(r.meter, Column(r, "c4")).kind == Kind::NotFloat);
            CHECK(Match(r.meter, r.candidates).what != What::Found);
        }
    }

    // Cyberpunk 2077, dark interior to daylight: c61 (1x1 R16_FLOAT) is the game's exposure multiplier, ~9.6 EV of
    // travel at corr 0.986. The other movers are not: c9/c11 smooth but unrelated, c24 integer bits, c13/c30 steady.
    {
        const Recording r = Load(folder + "/cyberpunk_interior_daylight.csv");
        CHECK(r.ok);
        if (r.ok)
        {
            const Verdict v = Match(r.meter, r.candidates);
            CHECK(v.what == What::Found);
            CHECK(v.index >= 0 && r.columns[(size_t) v.index] == "c61");
            CHECK(v.fit.kind == Kind::Multiplier);
            CHECK(v.fit.corr > 0.95f);
            CHECK(v.meterTravelEv > 6.0f);

            const Verdict each = MatchEach(r.meter, r.candidates, Firsts(r));
            CHECK(each.what == What::Found);
            CHECK(each.index >= 0 && r.columns[(size_t) each.index] == "c61");

            CHECK(Classify(r.meter, Column(r, "c61")).kind == Kind::Multiplier);
            CHECK(Classify(r.meter, Column(r, "c4")).kind == Kind::Steady);
            CHECK(Classify(r.meter, Column(r, "c9")).kind == Kind::Unrelated);
            CHECK(Classify(r.meter, Column(r, "c11")).kind == Kind::Unrelated);
            CHECK(Classify(r.meter, Column(r, "c13")).kind == Kind::Steady);
            CHECK(Classify(r.meter, Column(r, "c24")).kind == Kind::NotFloat);
            CHECK(Classify(r.meter, Column(r, "c30")).kind == Kind::Steady);

            // Widest travel on the same recording, for the record: which candidate the default rule would offer.
            size_t widest = 0;
            float widestRatio = 0.0f;
            for (size_t c = 0; c < r.candidates.size(); ++c)
            {
                float lo = 0.0f, hi = 0.0f;
                for (float a : r.candidates[c])
                    if (a > 1e-6f && a < 1e4f)
                    {
                        lo = lo == 0.0f ? a : std::min(lo, a);
                        hi = std::max(hi, a);
                    }
                if (lo > 0.0f && hi / lo > widestRatio)
                {
                    widestRatio = hi / lo;
                    widest = c;
                }
            }
            std::printf(
                "  cyberpunk: match %s (%s, slope %.3f, corr %.3f, lag %d, %.1f EV); widest travel %s (x%.0f)\n",
                r.columns[(size_t) v.index].c_str(), KindName(v.fit.kind), v.fit.slope, v.fit.corr, v.fit.lag,
                v.meterTravelEv, r.columns[widest].c_str(), widestRatio);

            // Through the recorder, as the scan feeds it: each candidate from its first reading.
            History h(4000);
            const std::vector<size_t> first = Firsts(r);
            for (size_t i = 0; i < r.meter.size(); ++i)
            {
                std::vector<std::pair<unsigned, float>> values;
                for (size_t c = 0; c < r.candidates.size(); ++c)
                    if (i >= first[c])
                        values.push_back({ (unsigned) (100 + c), r.candidates[c][i] });
                h.AddRow(r.meter[i], values);
            }
            const History::Judgement j = h.Judge();
            CHECK(j.verdict.what == What::Found);
            CHECK(j.id >= 100 && r.columns[j.id - 100] == "c61");
        }
    }

    // Doom Eternal (~4 min with menus and pauses): its handed exposure floors and eases too loosely to match; c8
    // follows the light as a scene luminance does and must never be the answer, on any prefix the scan would judge.
    {
        const Recording r = Load(folder + "/doom_eternal_long.csv");
        CHECK(r.ok);
        if (r.ok)
        {
            const std::vector<float>& game = Column(r, "game");
            const Fit c8 = Classify(r.meter, Column(r, "c8"));
            CHECK(c8.corr < -0.85f);
            CHECK(c8.slope < -0.8f && c8.slope > -1.2f);
            CHECK(c8.kind == Kind::Luminance || c8.kind == Kind::Unrelated);
            CHECK(Classify(r.meter, game).kind != Kind::Divisor);
            CHECK(Match(r.meter, { game }).what != What::Found);
            std::printf("  doom: c8 %s slope %.3f corr %.3f; game corr %.3f\n", KindName(c8.kind), c8.slope, c8.corr,
                        Classify(r.meter, game).corr);

            const std::vector<size_t> first = Firsts(r);
            for (size_t n = 200; n <= r.meter.size(); n += 50)
            {
                const std::vector<float> m(r.meter.begin(), r.meter.begin() + (std::ptrdiff_t) n);
                std::vector<std::vector<float>> c(r.candidates.size());
                for (size_t k = 0; k < c.size(); ++k)
                    c[k].assign(r.candidates[k].begin(), r.candidates[k].begin() + (std::ptrdiff_t) n);
                const Verdict v = MatchEach(m, c, first);
                if (v.what == What::Found)
                    CHECK(r.columns[(size_t) v.index] != "c8");
            }
        }
    }
}
} // namespace

int main(int argc, char** argv)
{
    const std::string folder = argc > 1 ? argv[1] : "data";
    Traces();
    Ported();
    Ours();
    Recordings(folder);

    if (fails != 0)
    {
        std::printf("FAIL: %d of %d checks\n", fails, checks);
        return 1;
    }
    std::printf("PASS: %d checks\n", checks);
    return 0;
}

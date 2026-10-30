// Scene-cut detection's rule (retoque/dlssnr/DlssNr_SceneCut.h, docs/dlssnr/design/scene-cut-detection.md), driven
// over synthetic 16x16 grids laid out as the pulse pass writes them: (luminance out, luminance in, pixels read, 0)
// per tile. A synthetic world is a smooth field of light; a frame is the grid of its tile means seen from a camera,
// with grain. Each sequence states how many cuts it holds, and the detector is held to exactly that.

#include <dlssnr/DlssNr_SceneCut.h>
#include <dlssnr/DlssNr_Pulse.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <limits>
#include <string>
#include <vector>

using namespace DlssNr::SceneCut;

// The grid the rule reads is the one the pulse pass already writes: same size, four floats a tile.
static_assert(kGrid == DlssNr::Pulse::kGrid);
static_assert(kPulseGridFloats == DlssNr::Pulse::kGridFloats);

static int failures = 0;
#define CHECK(cond)                                                                                                    \
    do                                                                                                                 \
    {                                                                                                                  \
        if (!(cond))                                                                                                   \
        {                                                                                                              \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond);                              \
            ++failures;                                                                                                \
        }                                                                                                              \
    } while (0)

namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr float kPixels = 1024.0f; // pixels each tile read; the pulse pass reads a quarter of its tile

// A deterministic generator, so a failure reproduces.
struct Lcg
{
    uint64_t state;
    explicit Lcg(uint64_t seed) : state(seed * 6364136223846793005ull + 1442695040888963407ull) {}
    double Uniform()
    {
        state = state * 6364136223846793005ull + 1442695040888963407ull;
        return static_cast<double>(state >> 11) / static_cast<double>(1ull << 53);
    }
    double Signed() { return Uniform() * 2.0 - 1.0; }
};

// A world: light as a smooth function of position, in tiles. Blobs of light and shade over a sky that is brighter
// at the top, which every world shares -- two scenes alike in their broad layout are the hard case for a cut.
struct World
{
    struct Blob
    {
        double x, y, sigma, stops;
    };
    std::vector<Blob> blobs;
    double phaseX = 0.0, phaseY = 0.0, base = 1.0;

    explicit World(uint64_t seed)
    {
        Lcg r(seed);
        for (int i = 0; i < 48; ++i)
            blobs.push_back(
                { -24.0 + 64.0 * r.Uniform(), -24.0 + 64.0 * r.Uniform(), 1.0 + 3.0 * r.Uniform(), 3.0 * r.Signed() });
        phaseX = 2.0 * kPi * r.Uniform();
        phaseY = 2.0 * kPi * r.Uniform();
        base = 0.05 + r.Uniform();
    }

    double Light(double x, double y) const
    {
        double stops = 0.6 * std::sin(0.9 * x + phaseX) * std::cos(0.7 * y + phaseY) - 0.12 * y;
        for (const Blob& b : blobs)
            stops += b.stops * std::exp(-((x - b.x) * (x - b.x) + (y - b.y) * (y - b.y)) / (2.0 * b.sigma * b.sigma));
        return base * std::exp2(stops);
    }
};

struct Camera
{
    double x = 0.0, y = 0.0; // the grid's top-left corner in the world, in tiles
    double zoom = 1.0;       // > 1: walked forward, the centre magnified
};

// Per-tile changes on top of the world: gain, an added flat light, a local burst, grain.
struct Look
{
    double gain = 1.0;
    double add = 0.0;                                     // a flat light added, in the world's base units
    double burstX = 0.0, burstY = 0.0, burstRadius = 0.0; // in grid tiles
    double burstStops = 0.0;                              // how much brighter its centre is
    double grain = 0.01;                                  // multiplicative noise per tile
    double floorNoise = 0.0;                              // additive noise, absolute
    uint64_t noiseSeed = 1;
};

std::vector<float> Render(const World& w, const Camera& c, const Look& look)
{
    std::vector<float> grid(kPulseGridFloats, 0.0f);
    Lcg noise(look.noiseSeed);
    for (unsigned ty = 0; ty < kGrid; ++ty)
    {
        for (unsigned tx = 0; tx < kGrid; ++tx)
        {
            double sum = 0.0;
            for (int sy = 0; sy < 4; ++sy)
            {
                for (int sx = 0; sx < 4; ++sx)
                {
                    const double gx = tx + (sx + 0.5) / 4.0 - kGrid / 2.0;
                    const double gy = ty + (sy + 0.5) / 4.0 - kGrid / 2.0;
                    sum += w.Light(c.x + kGrid / 2.0 + gx / c.zoom, c.y + kGrid / 2.0 + gy / c.zoom);
                }
            }
            double mean = sum / 16.0 * look.gain + look.add;
            if (look.burstRadius > 0.0)
            {
                const double dx = tx + 0.5 - look.burstX, dy = ty + 0.5 - look.burstY;
                const double r2 = (dx * dx + dy * dy) / (look.burstRadius * look.burstRadius);
                mean *= std::exp2(look.burstStops * std::exp(-r2 * 2.0));
            }
            mean *= 1.0 + look.grain * noise.Signed();
            mean += look.floorNoise * noise.Uniform();
            float* tile = &grid[(ty * kGrid + tx) * 4];
            tile[0] = static_cast<float>(mean * kPixels * 0.5); // our output: never read by the rule
            tile[1] = static_cast<float>(mean * kPixels);
            tile[2] = kPixels;
            tile[3] = 0.0f;
        }
    }
    return grid;
}

struct Run
{
    std::vector<Verdict> verdicts;
    unsigned cuts = 0;
    std::vector<uint64_t> cutAt;
    double maxSameIncoherence = 0.0; // the largest incoherence of a frame pair the rule called the same scene
};

// Frames 1..n, consecutive, 16 ms apart, no game resets.
Run Feed(Detector& d, unsigned n, const std::function<std::vector<float>(unsigned)>& frame)
{
    Run run;
    for (unsigned i = 1; i <= n; ++i)
    {
        const std::vector<float> grid = frame(i);
        const Verdict v = d.Add(i, 16 * static_cast<int64_t>(i), FromPulseGrid(grid.data()), false);
        run.verdicts.push_back(v);
        if (v.outcome == Outcome::Cut)
        {
            ++run.cuts;
            run.cutAt.push_back(v.seq);
        }
        if (v.outcome == Outcome::Same)
            run.maxSameIncoherence = std::fmax(run.maxSameIncoherence, v.scores.incoherence);
    }
    return run;
}

// The margins: the largest of each score over the sequence's compared pairs, and the largest incoherence and breadth
// of a pair whose light changed both ways (the polarity term alone did not rule it out).
void Report(const char* name, const Run& run)
{
    double inc = 0, br = 0, dist = 0, incBoth = 0, brBoth = 0;
    for (const Verdict& v : run.verdicts)
    {
        if (!v.scores.compared)
            continue;
        inc = std::fmax(inc, v.scores.incoherence);
        br = std::fmax(br, v.scores.breadth);
        dist = std::fmax(dist, v.scores.distribution);
        if (v.scores.polarity >= kCutPolarity && v.scores.polarity <= 1.0 - kCutPolarity)
        {
            incBoth = std::fmax(incBoth, v.scores.incoherence);
            brBoth = std::fmax(brBoth, v.scores.breadth);
        }
    }
    std::printf("  %-34s cuts %u  max: incoherence %.3f, breadth %.3f, distribution %.2f; both ways %.3f / %.3f\n",
                name, run.cuts, inc, br, dist, incBoth, brBoth);
}

Look Grain(unsigned i)
{
    Look look;
    look.noiseSeed = 1000 + i;
    return look;
}

// --- the frame ---------------------------------------------------------------------------------------------------

void TestFrame()
{
    const World w(7);
    const std::vector<float> grid = Render(w, {}, Grain(1));
    const Frame f = FromPulseGrid(grid.data());
    CHECK(f.valid);
    CHECK(!f.blank);
    CHECK(f.tiles == kTiles);
    CHECK(f.spread > 0.2);

    // Ranks run 0..1, each once, and follow the light.
    double lo = 1.0, hi = 0.0;
    unsigned brightest = 0, darkest = 0;
    for (unsigned i = 0; i < kTiles; ++i)
    {
        CHECK(f.rank[i] >= 0.0f && f.rank[i] <= 1.0f);
        lo = std::fmin(lo, f.rank[i]);
        hi = std::fmax(hi, f.rank[i]);
        if (grid[i * 4 + 1] > grid[brightest * 4 + 1])
            brightest = i;
        if (grid[i * 4 + 1] < grid[darkest * 4 + 1])
            darkest = i;
    }
    CHECK(lo == 0.0);
    CHECK(hi == 1.0);
    CHECK(f.rank[brightest] == 1.0f);
    CHECK(f.rank[darkest] == 0.0f);

    // Only the game's frame is read: our output, channel 0, can be anything.
    std::vector<float> scribbled = grid;
    for (unsigned i = 0; i < kTiles; ++i)
        scribbled[i * 4] = (i % 3 == 0) ? std::numeric_limits<float>::quiet_NaN() : 1e30f * static_cast<float>(i);
    const Frame g = FromPulseGrid(scribbled.data());
    for (unsigned i = 0; i < kTiles; ++i)
        CHECK(g.rank[i] == f.rank[i] && g.level[i] == f.level[i]);

    // FromMeans takes per-tile means directly: the same frame.
    std::vector<float> means(kTiles);
    for (unsigned i = 0; i < kTiles; ++i)
        means[i] = grid[i * 4 + 1] / grid[i * 4 + 2];
    const Frame m = FromMeans(means.data());
    CHECK(m.valid && m.tiles == kTiles);
    for (unsigned i = 0; i < kTiles; ++i)
        CHECK(m.rank[i] == f.rank[i]);

    // Gain does not change the frame: the levels are relative to the median, the ranks are order.
    std::vector<float> brighter = means;
    for (float& v : brighter)
        v *= 37.0f;
    const Frame b = FromMeans(brighter.data());
    for (unsigned i = 0; i < kTiles; ++i)
    {
        CHECK(b.rank[i] == f.rank[i]);
        CHECK(std::fabs(b.level[i] - m.level[i]) < 1e-4f);
    }

    // Ties share their rank: a letterbox of black tiles all sit at one average rank.
    std::vector<float> letterbox = means;
    for (unsigned i = 0; i < kGrid * 2; ++i)
        letterbox[i] = 0.0f;
    const Frame l = FromMeans(letterbox.data());
    CHECK(l.valid && !l.blank);
    for (unsigned i = 1; i < kGrid * 2; ++i)
        CHECK(l.rank[i] == l.rank[0]);
    CHECK(std::fabs(l.rank[0] - (kGrid * 2 - 1) / 2.0 / (kTiles - 1)) < 1e-6);
    CHECK(std::isfinite(l.level[0]));

    // A tile that measured nothing, or something that is not light, is left out; too many, and the frame is not
    // measured at all.
    std::vector<float> holes = grid;
    holes[0 * 4 + 2] = 0.0f;                                    // no pixels
    holes[1 * 4 + 1] = std::numeric_limits<float>::quiet_NaN(); // NaN
    holes[2 * 4 + 1] = std::numeric_limits<float>::infinity();  // inf
    holes[3 * 4 + 1] = -5.0f;                                   // negative
    const Frame h = FromPulseGrid(holes.data());
    CHECK(h.valid && h.tiles == kTiles - 4);
    for (unsigned i = 0; i < 4; ++i)
        CHECK(std::isnan(h.rank[i]) && std::isnan(h.level[i]));

    std::vector<float> mostlyHoles = grid;
    for (unsigned i = 0; i < kTiles - kMinTiles + 1; ++i)
        mostlyHoles[i * 4 + 2] = 0.0f;
    CHECK(!FromPulseGrid(mostlyHoles.data()).valid);
    CHECK(!FromPulseGrid(nullptr).valid);
    CHECK(!FromMeans(nullptr).valid);

    // Black, and flat: a frame, but no picture.
    std::vector<float> black(kTiles, 0.0f);
    const Frame k = FromMeans(black.data());
    CHECK(k.valid && k.blank);
    std::vector<float> flat(kTiles, 0.25f);
    const Frame fl = FromMeans(flat.data());
    CHECK(fl.valid && fl.blank);
}

// --- the comparison ----------------------------------------------------------------------------------------------

void TestCompare()
{
    const World a(41), b(42);
    const Frame fa = FromPulseGrid(Render(a, {}, Grain(1)).data());
    const Frame fa2 = FromPulseGrid(Render(a, {}, Grain(2)).data());
    const Frame fb = FromPulseGrid(Render(b, {}, Grain(3)).data());

    const Scores same = Compare(fa, fa2);
    CHECK(same.compared);
    CHECK(same.shiftX == 0 && same.shiftY == 0);
    CHECK(same.incoherence < 0.05);
    CHECK(!same.Cut());

    const Scores cut = Compare(fa, fb);
    CHECK(cut.compared);
    CHECK(cut.Cut());

    // A pan by whole tiles, one right and one up, is found as exactly that shift (in half tiles).
    Camera moved;
    moved.x = 1.0;
    moved.y = -1.0;
    const Scores pan = Compare(fa, FromPulseGrid(Render(a, moved, Grain(4)).data()));
    CHECK(pan.compared && !pan.Cut());
    CHECK(pan.shiftX == 2 && pan.shiftY == -2);
    CHECK(pan.overlap >= kMinOverlap && pan.overlap < kTiles);

    // By half a tile, within a step: a half-tile position is the mean of the tiles around it, close but not exact.
    moved.x = 1.0;
    moved.y = 0.5;
    const Scores half = Compare(fa, FromPulseGrid(Render(a, moved, Grain(5)).data()));
    CHECK(half.compared && !half.Cut());
    CHECK(half.shiftX == 2 && std::abs(half.shiftY - 1) <= 1);

    // Not compared: a frame not measured, or blank, on either side.
    std::vector<float> black(kTiles, 0.0f);
    const Frame fk = FromMeans(black.data());
    CHECK(!Compare(fa, fk).compared);
    CHECK(!Compare(fk, fa).compared);
    CHECK(!Compare(fa, Frame {}).compared);
}

// --- sequences -----------------------------------------------------------------------------------------------------

void TestStatic()
{
    const World w(21);
    Detector d;
    const Run run = Feed(d, 120, [&](unsigned i) { return Render(w, {}, Grain(i)); });
    Report("static, grain", run);
    CHECK(run.cuts == 0);
    CHECK(d.Counts().frames == 120);
    CHECK(d.Counts().compared == 119);
    CHECK(d.Counts().cuts == 0);

    // Heavier grain on a darker scene: still one scene.
    Detector d2;
    const Run noisy = Feed(d2, 120,
                           [&](unsigned i)
                           {
                               Look look = Grain(i);
                               look.grain = 0.08;
                               return Render(w, {}, look);
                           });
    Report("static, 8% grain", noisy);
    CHECK(noisy.cuts == 0);
}

void TestPans()
{
    const World w(31);
    struct Case
    {
        const char* name;
        double dx, dy, zoom;
    };
    const Case cases[] = {
        { "slow pan, 0.1 tile/frame", 0.1, 0.0, 1.0 },  { "pan, 0.5 tile/frame", 0.5, 0.0, 1.0 },
        { "fast pan, 1.5 tiles/frame", 1.5, 0.0, 1.0 }, { "diagonal pan, 0.7 x 0.4", 0.7, -0.4, 1.0 },
        { "tilt, 0.3 tile/frame up", 0.0, -0.3, 1.0 },  { "walk forward, 2% zoom/frame", 0.0, 0.0, 1.02 },
        { "walk forward and pan", 0.4, 0.0, 1.015 },
    };
    for (const Case& c : cases)
    {
        Detector d;
        const Run run = Feed(d, 90,
                             [&](unsigned i)
                             {
                                 Camera cam;
                                 cam.x = c.dx * i;
                                 cam.y = c.dy * i;
                                 cam.zoom = std::pow(c.zoom, i);
                                 return Render(w, cam, Grain(i));
                             });
        Report(c.name, run);
        CHECK(run.cuts == 0);
    }
}

void TestHardCut()
{
    const World a(41), b(42);
    Detector d;
    const Run run = Feed(d, 60, [&](unsigned i) { return Render(i <= 30 ? a : b, {}, Grain(i)); });
    Report("hard cut at 31", run);
    CHECK(run.cuts == 1);
    CHECK(!run.cutAt.empty() && run.cutAt[0] == 31);
    CHECK(d.Counts().cuts == 1);

    // A hundred pairs of worlds, panning on both sides of the cut. Worlds share their sky, and some cut to a picture
    // that is one big bright or dark area, which reads as light switched on: not every cut is found. None is found
    // twice, and none anywhere else.
    unsigned found = 0, total = 0, extra = 0;
    for (uint64_t s = 100; s < 200; ++s)
    {
        const World x(s), y(s + 1000);
        Detector e;
        const Run r = Feed(e, 40,
                           [&](unsigned i)
                           {
                               Camera cam;
                               cam.x = 0.3 * i;
                               return Render(i <= 20 ? x : y, cam, Grain(i));
                           });
        ++total;
        found += (r.cuts >= 1 && r.cutAt[0] == 21) ? 1 : 0;
        extra += r.cuts > 1 ? r.cuts - 1 : 0;
    }
    std::printf("  %-34s %u of %u found, %u extra\n", "cuts between 100 world pairs", found, total, extra);
    CHECK(found * 100 >= total * 90);
    CHECK(extra == 0);
}

void TestFlashes()
{
    const World w(51);

    // A white flash: the whole frame six times brighter for three frames, then back.
    {
        Detector d;
        const Run run = Feed(d, 60,
                             [&](unsigned i)
                             {
                                 Look look = Grain(i);
                                 if (i >= 20 && i < 23)
                                     look.gain = 6.0;
                                 return Render(w, {}, look);
                             });
        Report("flash: gain x6 for 3 frames", run);
        CHECK(run.cuts == 0);
    }

    // A flash that adds light: the dark parts lifted far more than the bright ones, in stops.
    {
        Detector d;
        const Run run = Feed(d, 60,
                             [&](unsigned i)
                             {
                                 Look look = Grain(i);
                                 if (i >= 20 && i < 24)
                                     look.add = w.base * 4.0;
                                 return Render(w, {}, look);
                             });
        Report("flash: added light for 4 frames", run);
        CHECK(run.cuts == 0);
    }

    // An explosion: a burst five stops bright over a fifth of the frame, in two frames, decaying, while the camera
    // pans slowly and shakes.
    {
        Detector d;
        const Run run = Feed(d, 60,
                             [&](unsigned i)
                             {
                                 Look look = Grain(i);
                                 Camera cam;
                                 cam.x = 0.15 * i + ((i >= 20 && i < 30) ? 0.3 * ((i % 2) ? 1 : -1) : 0.0);
                                 if (i >= 20)
                                 {
                                     const double age = i - 20.0;
                                     look.burstX = 9.0;
                                     look.burstY = 8.0;
                                     look.burstRadius = 4.0 + 0.3 * age;
                                     look.burstStops = (age < 2 ? 2.5 * (age + 1) : 5.0 * std::exp(-0.15 * age));
                                 }
                                 return Render(w, cam, look);
                             });
        Report("explosion, a fifth of the frame", run);
        CHECK(run.cuts == 0);
    }

    // A muzzle flash: one frame, a quarter of the frame, four stops.
    {
        Detector d;
        const Run run = Feed(d, 60,
                             [&](unsigned i)
                             {
                                 Look look = Grain(i);
                                 if (i % 10 == 0)
                                 {
                                     look.burstX = 8.0;
                                     look.burstY = 12.0;
                                     look.burstRadius = 5.0;
                                     look.burstStops = 4.0;
                                 }
                                 return Render(w, {}, look);
                             });
        Report("muzzle flashes, every 10th frame", run);
        CHECK(run.cuts == 0);
    }
}

void TestFades()
{
    const World a(61), b(62);

    // To black over 30 frames, black for 10, the other scene in over 30: a cut through black, but every step small.
    {
        Detector d;
        const Run run = Feed(d, 100,
                             [&](unsigned i)
                             {
                                 Look look = Grain(i);
                                 look.floorNoise = 1e-4;
                                 if (i <= 30)
                                 {
                                     look.gain = std::pow(1.0 - i / 30.0, 2.0);
                                     return Render(a, {}, look);
                                 }
                                 if (i <= 40)
                                 {
                                     look.gain = 0.0;
                                     return Render(b, {}, look);
                                 }
                                 look.gain = std::pow((i - 40) / 30.0, 2.0);
                                 if (i > 70)
                                     look.gain = 1.0;
                                 return Render(b, {}, look);
                             });
        Report("fade out, black, fade in", run);
        CHECK(run.cuts == 0);
        CHECK(d.Counts().blank > 0);
    }

    // A cross-dissolve over 24 frames.
    {
        Detector d;
        const Run run = Feed(d, 80,
                             [&](unsigned i)
                             {
                                 const double t = std::fmin(std::fmax((i - 30.0) / 24.0, 0.0), 1.0);
                                 Look la = Grain(i), lb = Grain(i + 500);
                                 la.gain = 1.0 - t;
                                 lb.gain = t;
                                 std::vector<float> ga = Render(a, {}, la), gb = Render(b, {}, lb);
                                 for (size_t k = 0; k < ga.size(); ++k)
                                     ga[k] = (k % 4 == 2) ? kPixels : ga[k] + gb[k];
                                 return ga;
                             });
        Report("cross-dissolve over 24 frames", run);
        CHECK(run.cuts == 0);
    }

    // Eye adaptation: the exposure walks four stops over 40 frames.
    {
        Detector d;
        const Run run = Feed(d, 60,
                             [&](unsigned i)
                             {
                                 Look look = Grain(i);
                                 look.gain = std::exp2(-4.0 * std::fmin(i / 40.0, 1.0));
                                 return Render(a, {}, look);
                             });
        Report("eye adaptation, 4 stops in 40", run);
        CHECK(run.cuts == 0);
    }
}

// --- hysteresis ----------------------------------------------------------------------------------------------------

std::vector<float> Scene(uint64_t world, unsigned i) { return Render(World(world), {}, Grain(i)); }

// The hysteresis tests cut between these worlds; each consecutive pair must be a cut by the scores, or the test
// would be testing nothing.
bool CutsBetween(uint64_t from, uint64_t to)
{
    return Compare(FromPulseGrid(Scene(from, 1).data()), FromPulseGrid(Scene(to, 2).data())).Cut();
}

void TestWarmup()
{
    CHECK(CutsBetween(72, 73) && CutsBetween(73, 74) && CutsBetween(74, 75) && CutsBetween(76, 77));
    // A cut on the second frame the detector sees is not believed: no history worth keeping yet, and the first frames
    // after a feature starts are where a game is least settled.
    Detector d;
    CHECK(d.Add(1, 16, FromPulseGrid(Scene(72, 1).data()), false).outcome == Outcome::Unpaired);
    const Verdict v = d.Add(2, 32, FromPulseGrid(Scene(73, 2).data()), false);
    CHECK(v.outcome == Outcome::Warmup);
    CHECK(v.scores.Cut());
    CHECK(d.Counts().warmup == 1 && d.Counts().cuts == 0);

    // The same after Reset.
    for (unsigned i = 3; i <= 10; ++i)
        d.Add(i, 16 * i, FromPulseGrid(Scene(73, i).data()), false);
    CHECK(d.Add(11, 176, FromPulseGrid(Scene(74, 11).data()), false).outcome == Outcome::Cut);
    d.Reset();
    CHECK(d.Counts().frames == 0 && d.Counts().cuts == 0);
    CHECK(d.Add(12, 192, FromPulseGrid(Scene(74, 12).data()), false).outcome == Outcome::Unpaired);
    CHECK(d.Add(13, 208, FromPulseGrid(Scene(75, 13).data()), false).outcome == Outcome::Warmup);

    // The warm-up is kWarmupPairs comparisons; the next is believed.
    Detector e;
    unsigned seq = 1;
    e.Add(seq, 16 * seq, FromPulseGrid(Scene(76, seq).data()), false);
    for (uint64_t k = 0; k < kWarmupPairs; ++k)
    {
        ++seq;
        CHECK(e.Add(seq, 16 * seq, FromPulseGrid(Scene(76, seq).data()), false).outcome == Outcome::Same);
    }
    ++seq;
    CHECK(e.Add(seq, 16 * seq, FromPulseGrid(Scene(77, seq).data()), false).outcome == Outcome::Cut);
}

void TestCooldown()
{
    CHECK(CutsBetween(81, 82) && CutsBetween(82, 83) && CutsBetween(83, 84) && CutsBetween(85, 86) &&
          CutsBetween(86, 87));
    // Cuts 5 frames apart (a montage): the first resets, the second is held back, one after the cooldown resets.
    Detector d;
    std::vector<Outcome> outcomes;
    auto worldAt = [](unsigned i) -> uint64_t
    {
        if (i <= 20)
            return 81;
        if (i <= 25)
            return 82;
        if (i <= 20 + kCooldownFrames + 5)
            return 83;
        return 84;
    };
    for (unsigned i = 1; i <= 80; ++i)
        outcomes.push_back(d.Add(i, 16 * i, FromPulseGrid(Scene(worldAt(i), i).data()), false).outcome);
    CHECK(outcomes[21 - 1] == Outcome::Cut);
    CHECK(outcomes[26 - 1] == Outcome::Cooldown);
    CHECK(outcomes[20 + kCooldownFrames + 6 - 1] == Outcome::Cut);
    CHECK(d.Counts().cuts == 2 && d.Counts().cooldown == 1);

    // The game's own reset starts the cooldown too: its cut is its own, and ours a frame later is the same cut.
    Detector g;
    for (unsigned i = 1; i <= 20; ++i)
        g.Add(i, 16 * i, FromPulseGrid(Scene(85, i).data()), false);
    const Verdict own = g.Add(21, 336, FromPulseGrid(Scene(86, 21).data()), true);
    CHECK(own.outcome == Outcome::GameReset);
    CHECK(g.Counts().gameResets == 1 && g.Counts().cuts == 0);
    CHECK(g.Add(22, 352, FromPulseGrid(Scene(87, 22).data()), false).outcome == Outcome::Cooldown);
}

void TestPairing()
{
    CHECK(CutsBetween(96, 97));
    Detector d;
    for (unsigned i = 1; i <= 10; ++i)
        d.Add(i, 16 * i, FromPulseGrid(Scene(91, i).data()), false);

    // A hole in the sequence: the pass ran at 11 and could not measure it. 12 has nothing to be compared with.
    CHECK(d.Add(12, 192, FromPulseGrid(Scene(92, 12).data()), false).outcome == Outcome::Unpaired);
    // Late: older than one already fed. Neither compared nor a reference.
    CHECK(d.Add(11, 176, FromPulseGrid(Scene(93, 11).data()), false).outcome == Outcome::Ignored);
    CHECK(d.Add(12, 192, FromPulseGrid(Scene(93, 12).data()), false).outcome == Outcome::Ignored);
    CHECK(d.Add(13, 208, FromPulseGrid(Scene(92, 13).data()), false).outcome == Outcome::Same);

    // A pause: consecutive, but longer than kGapMs apart.
    CHECK(d.Add(14, 208 + kGapMs + 1, FromPulseGrid(Scene(94, 14).data()), false).outcome == Outcome::Unpaired);

    // A frame not measured breaks the chain.
    CHECK(d.Add(15, 700, Frame {}, false).outcome == Outcome::Unmeasured);
    CHECK(d.Add(16, 716, FromPulseGrid(Scene(95, 16).data()), false).outcome == Outcome::Unpaired);

    // A blank frame is not compared, either way.
    std::vector<float> black(kTiles, 0.0f);
    CHECK(d.Add(17, 732, FromMeans(black.data()), false).outcome == Outcome::Blank);
    CHECK(d.Add(18, 748, FromPulseGrid(Scene(96, 18).data()), false).outcome == Outcome::Blank);
    CHECK(d.Add(19, 764, FromPulseGrid(Scene(97, 19).data()), false).outcome == Outcome::Cut);
    CHECK(d.Counts().blank == 2);
}

// --- the request ---------------------------------------------------------------------------------------------------

void TestPending()
{
    Pending p;
    CHECK(!p.Armed());
    CHECK(p.Take(5, true, false) == Request::None);

    // Detected at 31: frame 31 was already evaluated, so the earliest frame that can drop history is 32.
    p.Arm(31);
    CHECK(p.Armed());
    CHECK(p.Take(31, true, false) == Request::None);
    CHECK(p.Armed());
    CHECK(p.Take(33, true, false) == Request::Reset);
    CHECK(!p.Armed());
    CHECK(p.Take(34, true, false) == Request::None);

    // Off: said, never done.
    p.Arm(40);
    CHECK(p.Take(42, false, false) == Request::Shadow);
    CHECK(!p.Armed());

    // Too late: the model has rebuilt its history on the new scene; dropping it now costs and fixes nothing.
    p.Arm(50);
    CHECK(p.Take(50 + kMaxLateFrames, true, false) == Request::Reset);
    p.Arm(60);
    CHECK(p.Take(60 + kMaxLateFrames + 1, true, false) == Request::Late);
    CHECK(!p.Armed());

    // The game reset on the frame it is taken at: its reset, not ours.
    p.Arm(70);
    CHECK(p.Take(71, true, true) == Request::GameAlready);
    CHECK(!p.Armed());

    // Cleared on a feature or shape change.
    p.Arm(80);
    p.Clear();
    CHECK(!p.Armed() && p.Take(81, true, false) == Request::None);
}

void TestLog()
{
    Verdict v;
    v.outcome = Outcome::Cut;
    v.seq = 31;
    v.scores.compared = true;
    v.scores.incoherence = 0.31;
    v.scores.breadth = 0.72;
    v.scores.distribution = 0.44;
    const std::string reset = LogLine(v, Request::Reset);
    const std::string shadow = LogLine(v, Request::Shadow);
    CHECK(reset.find("scene cut") != std::string::npos);
    CHECK(reset.find("31") != std::string::npos);
    CHECK(reset.find("0.31") != std::string::npos);
    CHECK(reset.find("dropped") != std::string::npos);
    CHECK(shadow.find("SceneCutReset") != std::string::npos);
    CHECK(shadow.find("dropped") == std::string::npos);
    CHECK(LogLine(v, Request::Late).find("late") != std::string::npos);
    CHECK(!LogLine(v, Request::GameAlready).empty());
}
} // namespace

int main()
{
    TestFrame();
    TestCompare();
    std::printf("sequences:\n");
    TestStatic();
    TestPans();
    TestHardCut();
    TestFlashes();
    TestFades();
    TestWarmup();
    TestCooldown();
    TestPairing();
    TestPending();
    TestLog();

    if (failures != 0)
    {
        std::fprintf(stderr, "FAIL: %d check(s)\n", failures);
        return 1;
    }
    std::printf("PASS: nr-scene-cut\n");
    return 0;
}

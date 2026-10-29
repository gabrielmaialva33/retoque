// Zero-motion detection's CPU half (retoque/dlssnr/DlssNr_MotionProbe.h, docs/dlssnr/design/zero-motion-detection.md),
// driven over synthetic grids shaped as the probe shader writes them: (depth, depth slope, non-zero motion samples,
// motion samples) per cell of a 32x32 grid.

#include <dlssnr/DlssNr_MotionProbe.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <vector>

using namespace DlssNr::MotionProbe;

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
constexpr unsigned kTaps = kMotionTaps * kMotionTaps;

// A depth field over the grid: a sloped ground plane seen from a camera at height h, shifted by (sx, sy) cells of
// screen offset. Its slope per pixel is what the shader would measure for a region of `pixels` pixels per cell.
struct Scene
{
    double offsetX = 0.0; // in pixels: camera pan or jitter
    double offsetY = 0.0;
    double forward = 0.0; // camera moved along the view axis, in depth units
    double pixelsPerCell = 40.0;
    unsigned nonZero = 0; // non-zero motion samples per cell
    bool sky = false;     // the top quarter is a constant far plane
};

double DepthAt(const Scene& s, double px, double py)
{
    // A plane receding upward: depth grows with the row, plus a gentle lateral term, so every interior sample has a
    // finite, non-zero slope.
    return 10.0 + 0.02 * py + 0.005 * px + s.forward;
}

std::vector<float> Grid(const Scene& s)
{
    std::vector<float> grid(kGridFloats);
    for (unsigned y = 0; y < kGrid; ++y)
    {
        for (unsigned x = 0; x < kGrid; ++x)
        {
            float* t = &grid[(y * kGrid + x) * 4];
            const double px = (x + 0.5) * s.pixelsPerCell + s.offsetX;
            const double py = (y + 0.5) * s.pixelsPerCell + s.offsetY;
            if (s.sky && y < kGrid / 4)
            {
                t[0] = 0.0f;
                t[1] = 0.0f;
            }
            else
            {
                const double d = DepthAt(s, px, py);
                t[0] = static_cast<float>(d);
                t[1] = static_cast<float>(
                    std::fmax(std::fabs(DepthAt(s, px + 1, py) - d), std::fabs(DepthAt(s, px, py + 1) - d)));
            }
            t[2] = static_cast<float>(s.nonZero);
            t[3] = static_cast<float>(kTaps);
        }
    }
    return grid;
}

Probe ProbeOf(const Scene& s, uint64_t seq)
{
    static Probe p; // 8 KB: off the stack, as the renderer keeps it
    const auto grid = Grid(s);
    FromGrid(grid.data(), seq, p);
    return p;
}

const Shape kShape { 1920, 1080, 0, 0, 1920, 1080, 0, 0, true };

void TestCoordinates()
{
    // Centre and TapAt land inside every extent, the same integer maths as the shader.
    for (uint32_t extent : { 1u, 2u, 7u, 31u, 32u, 33u, 127u, 128u, 1279u, 3840u, 16384u })
    {
        for (uint32_t cell = 0; cell < kGrid; ++cell)
        {
            CHECK(Centre(cell, extent) < extent);
            for (uint32_t tap = 0; tap < kMotionTaps; ++tap)
                CHECK(TapAt(cell, tap, extent) < extent);
        }
        if (extent >= kGrid * kMotionTaps)
        {
            // Large enough: taps are strictly increasing, so no motion sample is read twice.
            uint32_t last = 0;
            bool first = true;
            for (uint32_t cell = 0; cell < kGrid; ++cell)
                for (uint32_t tap = 0; tap < kMotionTaps; ++tap)
                {
                    const uint32_t at = TapAt(cell, tap, extent);
                    CHECK(first || at > last);
                    last = at;
                    first = false;
                }
        }
    }
    CHECK(Centre(0, 1920) == 30);
    CHECK(Centre(31, 1920) == 1890);
    CHECK(Due(0) && !Due(1) && !Due(7) && Due(8) && Due(16));
}

void TestFromGrid()
{
    Scene s;
    s.nonZero = 3;
    Probe p = ProbeOf(s, 5);
    CHECK(p.valid && p.seq == 5);
    CHECK(p.nonZeroMotion == 3 * kCells);
    CHECK(p.motionSamples == kTaps * kCells);

    auto grid = Grid(s);
    grid[3] = 7.0f; // a cell that did not read the full lattice is not a grid this shader wrote
    FromGrid(grid.data(), 1, p);
    CHECK(!p.valid);
    grid = Grid(s);
    grid[2] = std::numeric_limits<float>::quiet_NaN();
    FromGrid(grid.data(), 1, p);
    CHECK(!p.valid);
    FromGrid(nullptr, 1, p);
    CHECK(!p.valid);

    // An invalid probe changes nothing.
    Detector d;
    CHECK(d.Add(kShape, p) == Event::None && !d.Dead());
}

void TestJitterIsNotMotion()
{
    // A still camera with TAA jitter: sub-pixel offsets every frame, zero vectors, correctly. Never dead.
    Detector d;
    const double jitter[][2] = { { 0.25, -0.33 }, { -0.5, 0.17 }, { 0.375, 0.44 }, { -0.125, -0.5 }, { 0.5, 0.0 } };
    for (unsigned i = 0; i < 40; ++i)
    {
        Scene s;
        s.offsetX = jitter[i % 5][0];
        s.offsetY = jitter[i % 5][1];
        CHECK(d.Add(kShape, ProbeOf(s, i * kInterval)) == Event::None);
        CHECK(!d.Dead());
        CHECK(!d.LastReading().depth.Moving());
    }
    CHECK(d.Streak() == 0);
}

void TestPanIsDeadAfterTwoReadings()
{
    Detector d;
    Scene s;
    s.sky = true;
    CHECK(d.Add(kShape, ProbeOf(s, 0)) == Event::None); // the first probe has nothing to compare with
    s.offsetX += 12.0;                                  // a pan of 1.5 px a frame over eight frames
    CHECK(d.Add(kShape, ProbeOf(s, kInterval)) == Event::None);
    CHECK(d.LastReading().Counts() && d.Streak() == 1 && !d.Dead()); // one reading is not a verdict
    // The sky cannot tell: left out of the count, not counted as still.
    CHECK(d.LastReading().depth.telling == kCells - kCells / 4);
    CHECK(d.LastReading().depth.moved == d.LastReading().depth.telling);
    s.offsetX += 12.0;
    CHECK(d.Add(kShape, ProbeOf(s, 2 * kInterval)) == Event::Dead);
    CHECK(d.Dead());
    const std::string on = EventLine(Event::Dead, d.LastReading(), true);
    const std::string off = EventLine(Event::Dead, d.LastReading(), false);
    CHECK(on.find("all zero in 2 readings") != std::string::npos);
    CHECK(on.find("as ZeroGuideReset does on zero guides") != std::string::npos);
    CHECK(off.find("ZeroGuideReset=true drops it") != std::string::npos);
    CHECK(on.find("768 of 768") != std::string::npos);

    // Held: more readings do not log again.
    s.offsetX += 12.0;
    CHECK(d.Add(kShape, ProbeOf(s, 3 * kInterval)) == Event::None && d.Dead());
    // A still all-zero reading keeps it: an honest zero now says nothing about the buffer.
    CHECK(d.Add(kShape, ProbeOf(s, 4 * kInterval)) == Event::None && d.Dead());

    // One non-zero sample anywhere ends it, once.
    auto grid = Grid(s);
    grid[(17 * kGrid + 3) * 4 + 2] = 1.0f;
    static Probe p;
    FromGrid(grid.data(), 5 * kInterval, p);
    CHECK(d.Add(kShape, p) == Event::Alive && !d.Dead() && d.Streak() == 0);
    CHECK(EventLine(Event::Alive, d.LastReading(), true).find("carry motion again") != std::string::npos);
    CHECK(d.Add(kShape, ProbeOf(s, 6 * kInterval)) == Event::None);
}

void TestForwardMotion()
{
    // Walking forward: every sample's depth falls by the distance walked. Dead after two readings.
    Detector d;
    Scene s;
    d.Add(kShape, ProbeOf(s, 0));
    s.forward -= 0.5;
    d.Add(kShape, ProbeOf(s, kInterval));
    s.forward -= 0.5;
    CHECK(d.Add(kShape, ProbeOf(s, 2 * kInterval)) == Event::Dead);
}

void TestRealMotionNeverDead()
{
    Detector d;
    Scene s;
    s.nonZero = kTaps;
    for (unsigned i = 0; i < 20; ++i)
    {
        s.offsetX += 12.0;
        CHECK(d.Add(kShape, ProbeOf(s, i * kInterval)) == Event::None && !d.Dead());
    }
    // Real motion in some cells only (a camera standing still, one character moving): not dead either.
    s.nonZero = 0;
    auto grid = Grid(s);
    grid[(5 * kGrid + 5) * 4 + 2] = 2.0f;
    static Probe p;
    for (unsigned i = 20; i < 30; ++i)
    {
        s.offsetX += 12.0;
        grid = Grid(s);
        grid[(5 * kGrid + 5) * 4 + 2] = 2.0f;
        FromGrid(grid.data(), i * kInterval, p);
        CHECK(d.Add(kShape, p) == Event::None && !d.Dead());
    }
}

void TestNanMotionIsNotZero()
{
    // The shader counts a NaN vector as non-zero (!(x == 0)); a field of them is a different fault, never "dead".
    Detector d;
    Scene s;
    s.nonZero = kTaps;
    for (unsigned i = 0; i < 6; ++i)
    {
        s.offsetX += 12.0;
        d.Add(kShape, ProbeOf(s, i * kInterval));
    }
    CHECK(!d.Dead());
}

void TestPartialMoveIsStill()
{
    // A still camera over animated water filling a third of the frame, no vectors written for it: under half moved.
    Detector d;
    Scene s;
    static Probe a, b;
    auto grid = Grid(s);
    FromGrid(grid.data(), 0, a);
    for (unsigned i = 1; i < 10; ++i)
    {
        grid = Grid(s);
        for (unsigned c = 0; c < kCells / 3; ++c)
            grid[c * 4] += (i % 2 ? 0.3f : -0.3f);
        FromGrid(grid.data(), i * kInterval, b);
        d.Add(kShape, i == 1 ? a : b);
        CHECK(!d.Dead());
    }
    // Exactly half of what can tell is a moving scene; one short of it is not.
    DepthChange half { 200, 100 };
    DepthChange under { 200, 99 };
    DepthChange few { kMinTelling - 1, kMinTelling - 1 };
    CHECK(half.Moving() && !under.Moving() && !few.Moving());
}

void TestNonFiniteDepthCannotTell()
{
    Scene s;
    auto ga = Grid(s);
    s.offsetX += 12.0;
    auto gb = Grid(s);
    for (unsigned c = 0; c < kCells; ++c)
        gb[c * 4] = (c % 2) ? std::numeric_limits<float>::infinity() : std::numeric_limits<float>::quiet_NaN();
    static Probe a, b;
    FromGrid(ga.data(), 0, a);
    FromGrid(gb.data(), kInterval, b);
    const DepthChange change = CompareDepth(a, b);
    CHECK(change.telling == 0 && change.moved == 0 && !change.Moving());
}

void TestShapeAndGapStartOver()
{
    Detector d;
    Scene s;
    d.Add(kShape, ProbeOf(s, 0));
    s.offsetX += 12.0;
    d.Add(kShape, ProbeOf(s, kInterval));
    CHECK(d.Streak() == 1);
    // Another shape: another buffer. Readings start over, the first probe of it compares with nothing.
    Shape other = kShape;
    other.depthWidth = 1280;
    other.depthHeight = 720;
    s.offsetX += 12.0;
    CHECK(d.Add(other, ProbeOf(s, 2 * kInterval)) == Event::None);
    CHECK(d.Streak() == 0 && !d.LastReading().compared);

    // Too old a previous probe: not compared, the streak neither grows nor breaks.
    s.offsetX += 12.0;
    d.Add(other, ProbeOf(s, 3 * kInterval));
    CHECK(d.Streak() == 1);
    s.offsetX += 12.0;
    d.Add(other, ProbeOf(s, 3 * kInterval + kInterval * kMaxGapIntervals + 1));
    CHECK(!d.LastReading().compared && d.Streak() == 1 && !d.Dead());
    // A probe that arrives out of order (an older seq) is not compared either.
    s.offsetX += 12.0;
    d.Add(other, ProbeOf(s, 2 * kInterval));
    CHECK(!d.LastReading().compared);

    // Dead, then the shape changes: judged afresh, and said once.
    Detector e;
    s = Scene {};
    e.Add(kShape, ProbeOf(s, 0));
    s.offsetX += 12.0;
    e.Add(kShape, ProbeOf(s, kInterval));
    s.offsetX += 12.0;
    CHECK(e.Add(kShape, ProbeOf(s, 2 * kInterval)) == Event::Dead);
    CHECK(e.Add(other, ProbeOf(s, 3 * kInterval)) == Event::Reshaped && !e.Dead());
    CHECK(EventLine(Event::Reshaped, e.LastReading(), false).find("afresh") != std::string::npos);
    CHECK(EventLine(Event::None, e.LastReading(), false).empty());

    // Low- or display-resolution vectors read differently: another shape too.
    Shape display = kShape;
    display.motionLowResolution = false;
    CHECK(!(display == kShape));
    e.Reset();
    CHECK(!e.Dead() && e.Streak() == 0);
}

void TestPolicyReset()
{
    // Only while dead, only on the game's guides, only with ZeroGuideReset on, never over the game's own reset.
    for (int mask = 0; mask < 16; ++mask)
    {
        const bool dead = mask & 1, game = mask & 2, key = mask & 4, gameReset = mask & 8;
        CHECK(PolicyReset(dead, game, key, gameReset) == (dead && game && key && !gameReset));
    }
}
} // namespace

int main()
{
    TestCoordinates();
    TestFromGrid();
    TestJitterIsNotMotion();
    TestPanIsDeadAfterTwoReadings();
    TestForwardMotion();
    TestRealMotionNeverDead();
    TestNanMotionIsNotZero();
    TestPartialMoveIsStill();
    TestNonFiniteDepthCannotTell();
    TestShapeAndGapStartOver();
    TestPolicyReset();

    if (failures != 0)
    {
        std::fprintf(stderr, "FAIL: %d check(s)\n", failures);
        return 1;
    }
    std::printf("PASS: grid coordinates, grid read-out, jitter is not motion, a pan and a walk are dead after two "
                "readings and held until one non-zero sample, real and partial motion never dead, NaN is not zero, "
                "non-finite depth cannot tell, shape and gap start over, the reset rule\n");
    return 0;
}

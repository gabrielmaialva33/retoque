// The light limiter at any cadence (docs/dlssnr/design/light-limiter.md): the production gate (DlssNr_LightLimit.h), the
// production per-pixel rules the shader runs (dlssnr_cadence_rule.h: CadenceLimitLight, CadenceLimitRegion,
// CadenceApplyGain, CadenceLight), and a CPU model of the three regional dispatches in the order the host records them.
// The model is the shader's decisions over arrays, not the shader on a GPU.
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

#include <dlssnr/DlssNr_Cadence.h>
#include <dlssnr/DlssNr_LightLimit.h>

using std::abs;
using std::cos;
using std::exp2;
using std::floor;
using std::isfinite;
using std::max;
using std::min;
using std::pow;
using std::sin;

struct float2
{
    float x = 0.0f, y = 0.0f;
    float2() = default;
    float2(float a, float b) : x(a), y(b) {}
};
struct float3
{
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float3() = default;
    float3(float a, float b, float c) : x(a), y(b), z(c) {}
};
inline float2 operator+(float2 a, float2 b) { return { a.x + b.x, a.y + b.y }; }
inline float2 operator-(float2 a, float2 b) { return { a.x - b.x, a.y - b.y }; }
inline float2 operator*(float2 a, float s) { return { a.x * s, a.y * s }; }
inline float3 operator+(float3 a, float3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline float3 operator-(float3 a, float3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline float3 operator*(float3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
inline float3 operator*(float s, float3 a) { return a * s; }
inline float3 operator/(float3 a, float s) { return { a.x / s, a.y / s, a.z / s }; }
inline float dot(float3 a, float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline int clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float saturate(float v) { return clamp(v, 0.0f, 1.0f); }
inline float frac(float v) { return v - std::floor(v); }
inline float smoothstep(float a, float b, float v)
{
    const float t = saturate((v - a) / (b - a));
    return t * t * (3.0f - 2.0f * t);
}

#define CR_FN inline
#define CR_UNROLL
#define CR_LOOP
#define CR_DEPTH_NOW(ax_, ay_) 0.0f
#define CR_DEPTH_THEN(ax_, ay_) 0.0f
#define CR_PROXY_NOW(ax_, ay_) float3()
#define CR_PROXY_THEN(ax_, ay_) float3()
#define CR_MOTION(ax_, ay_) float2()
#define CR_CHAIN_PREV(ax_, ay_) float2()
#define CR_RESIDUAL(ax_, ay_) float3()
#define CR_RESIDUAL_LOW(ax_, ay_) float3()
#define CR_UI(ax_, ay_) 0.0f
#define CR_FADE(ax_, ay_) float3()
#include <shaders/dlssnr/precompile/dlssnr_cadence_rule.h>

namespace LL = DlssNr::LightLimit;
namespace Cad = DlssNr::Cadence;

namespace
{
bool Near(float a, float b, float tolerance = 1e-4f) { return std::fabs(a - b) <= tolerance; }

// The shipped numbers, as the host passes them.
constexpr float kDt = 1.0f / 60.0f;

CadenceLimit Region(float raw, float input, float prevShown, float prevInput, int valid, float seconds = kDt,
                    float motion = 0.0f)
{
    return CadenceLimitRegion(raw, input, prevShown, prevInput, valid, LL::RegionStep(seconds),
                              Cad::kLightInputStops, LL::kRegionReleaseLow, LL::kRegionReleaseHigh, motion,
                              LL::kRegionMotionPixels, Cad::kLightMaxGainStops);
}

// ---- The regional pipeline over a row of pixels, in the order the host records the dispatches. ----------------------
constexpr int W = 256;     // eight tiles of 32
constexpr int Tile = (int) LL::kRegionTile;
constexpr int Tiles = W / Tile;

struct Row
{
    std::vector<float> proxy = std::vector<float>(W, 0.4f); // the model's input, encoded 0..1
    std::vector<float> edit = std::vector<float>(W, 0.0f);  // the model's light edit, in stops
    std::vector<float> motion = std::vector<float>(W, 0.0f);
    float Shown(int x) const { return CadenceLinearToSrgb(CadenceSrgbToLinear(proxy[x]) * exp2(edit[x])); }
};

struct Region_
{
    float shown[Tiles] = {}, input[Tiles] = {}, valid[Tiles] = {}, gain[Tiles] = {};
};

// Mode 10, 11, 12 over a row; returns the output row. limitValid is the gate's "not a reset".
std::vector<float> Limit(const Row& row, Region_& state, bool limitValid, float seconds)
{
    float sums[Tiles][4] = {};
    for (int t = 0; t < Tiles; ++t)
        for (int x = t * Tile; x < (t + 1) * Tile; x += 2)
        {
            sums[t][0] += CadenceLight(float3(row.Shown(x), row.Shown(x), row.Shown(x)), 0);
            sums[t][1] += CadenceLight(float3(row.proxy[x], row.proxy[x], row.proxy[x]), 0);
            sums[t][2] += row.motion[x];
            sums[t][3] += 1.0f;
        }
    for (int t = 0; t < Tiles; ++t)
    {
        const bool measurable = sums[t][0] > 0.0f && sums[t][1] > 0.0f && sums[t][3] > 0.0f;
        const float raw = measurable ? std::log2(sums[t][0] / sums[t][1]) : 0.0f;
        const float input = measurable ? std::log2(sums[t][1] / sums[t][3]) : 0.0f;
        const float motion = measurable ? sums[t][2] / sums[t][3] : 0.0f;
        const int valid = measurable && limitValid && state.valid[t] > 0.5f ? 1 : 0;
        const CadenceLimit r = Region(raw, input, state.shown[t], state.input[t], valid, seconds, motion);
        state.shown[t] = r.shown;
        state.input[t] = input;
        state.valid[t] = measurable ? 1.0f : 0.0f;
        state.gain[t] = r.gainStops;
    }
    std::vector<float> out(W);
    for (int x = 0; x < W; ++x)
    {
        const float centre = ((float) x + 0.5f) / (float) Tile - 0.5f;
        const float base = std::floor(centre);
        const float w = centre - base;
        const int a = clamp((int) base, 0, Tiles - 1), b = clamp((int) base + 1, 0, Tiles - 1);
        const float gain = state.gain[a] * (1.0f - w) + state.gain[b] * w;
        const float v = row.Shown(x);
        out[x] = CadenceApplyGain(float3(v, v, v), gain, 0).x;
    }
    return out;
}

float TileLight(const std::vector<float>& image, int t)
{
    float sum = 0.0f;
    for (int x = t * Tile; x < (t + 1) * Tile; ++x)
        sum += CadenceLight(float3(image[x], image[x], image[x]), 0);
    return std::log2(sum / (float) Tile);
}
} // namespace

int main()
{
    // ---- Requested: anything but 1 and 2 is off.
    assert(LL::Requested(0) == LL::Mode::Off && LL::Requested(1) == LL::Mode::Global &&
           LL::Requested(2) == LL::Mode::Regional);
    for (uint32_t typo : { 3u, 7u, 100u, 0xFFFFFFFFu })
        assert(LL::Requested(typo) == LL::Mode::Off);
    static_assert(LL::TilesFor(1) == 1 && LL::TilesFor(32) == 1 && LL::TilesFor(33) == 2 && LL::TilesFor(1920) == 60);

    // ---- The regional step: 0.75 stops a second, clamped as the global one is; half the global rate.
    assert(Near(LL::RegionStep(1.0f / 60.0f), 0.75f / 60.0f));
    assert(Near(LL::RegionStep(10.0f), 0.75f / 15.0f) && Near(LL::RegionStep(0.0f), 0.75f / 500.0f));
    assert(Near(LL::RegionStep(NAN), 0.75f / 500.0f) && Near(LL::RegionStep(-1.0f), 0.75f / 500.0f));
    assert(Near(LL::kRegionRate * 2.0f, Cad::kLightRate));
    assert(Near(LL::RegionMotionPixels(0.5f), 3.0f) && Near(LL::RegionMotionPixels(0.0f), 6.0f));

    // ---- The gate.
    {
        LL::Gate g;
        const uint64_t sig = 7;
        auto d = g.Decide(LL::Mode::Off, false, 1, 0, sig, false);
        assert(!d.run);
        d = g.Decide(LL::Mode::Global, false, 1, 0, sig, false);
        assert(d.run && d.reset && d.mode == LL::Mode::Global);              // the first frame has nothing to hold to
        g.Ran(true);
        d = g.Decide(LL::Mode::Global, false, 2, 16, sig, false);
        assert(d.run && !d.reset);                                           // the next one holds to it
        g.Ran(true);
        d = g.Decide(LL::Mode::Global, false, 2, 17, sig, false);
        assert(!d.run && !g.Limited());                                      // a second call in one present is another view
        d = g.Decide(LL::Mode::Global, false, 3, 33, sig, false);
        assert(d.run && d.reset);                                            // ... and what followed it starts over
        g.Ran(true);
        d = g.Decide(LL::Mode::Global, false, 4, 49, sig, true);
        assert(d.run && d.reset);                                            // the model's history reset: a new decision
        g.Ran(true);
        d = g.Decide(LL::Mode::Global, false, 5, 65, sig + 1, false);
        assert(d.run && d.reset);                                            // size, format, route changed
        g.Ran(true);
        d = g.Decide(LL::Mode::Regional, false, 6, 81, sig + 1, false);
        assert(d.run && d.reset && d.mode == LL::Mode::Regional);            // another mode is another state
        g.Ran(true);
        d = g.Decide(LL::Mode::Regional, false, 7, 81 + Cad::kGapMs + 1, sig + 1, false);
        assert(d.run && d.reset);                                            // a gap longer than the cadence's
        g.Ran(true);
        d = g.Decide(LL::Mode::Regional, false, 8, 81 + Cad::kGapMs + 2, sig + 1, false);
        assert(d.run && !d.reset);
        g.Ran(true);
        d = g.Decide(LL::Mode::Regional, true, 9, 81 + Cad::kGapMs + 3, sig + 1, false);
        assert(!d.run && !g.Limited());                                      // the cadence's own limiter held this frame
        d = g.Decide(LL::Mode::Regional, false, 10, 81 + Cad::kGapMs + 4, sig + 1, false);
        assert(d.run && d.reset);                                            // ... so this one starts over
        g.Ran(false);
        d = g.Decide(LL::Mode::Regional, false, 11, 81 + Cad::kGapMs + 5, sig + 1, false);
        assert(d.run && d.reset);                                            // a frame that could not be limited is not held to
        g.Ran(true);
        d = g.Decide(LL::Mode::Off, false, 12, 81 + Cad::kGapMs + 6, sig + 1, false);
        assert(!d.run);
        d = g.Decide(LL::Mode::Regional, false, 13, 81 + Cad::kGapMs + 7, sig + 1, false);
        assert(d.run && d.reset);                                            // off in between: nothing is kept
        // A route without a present count (0) never counts as a second call.
        LL::Gate z;
        assert(z.Decide(LL::Mode::Global, false, 0, 0, 1, false).run);
        z.Ran(true);
        assert(z.Decide(LL::Mode::Global, false, 0, 16, 1, false).run);
        z.Reset();
        assert(!z.Limited());
    }

    // ---- The regional rule: the cases that matter.
    {
        const float step = LL::RegionStep(kDt);
        // Nothing before it, or not finite: the raw light passes.
        auto r = Region(0.3f, 1.0f, 0.0f, 1.0f, 0);
        assert(r.gainStops == 0.0f && r.shown == 0.3f);
        r = Region(NAN, 1.0f, 0.0f, 1.0f, 1);
        assert(r.gainStops == 0.0f);
        r = Region(0.3f, 1.0f, NAN, 1.0f, 1);
        assert(r.gainStops == 0.0f);
        // The tile's own light held still: the edit's light moves by at most one step a frame, either way.
        r = Region(0.20f, 1.0f, 0.0f, 1.0f, 1);
        assert(Near(r.shown, step) && Near(r.gainStops, step - 0.20f));
        r = Region(-0.20f, 1.0f, 0.0f, 1.0f, 1);
        assert(Near(r.shown, -step));
        r = Region(0.005f, 1.0f, 0.0f, 1.0f, 1);
        assert(r.gainStops == 0.0f && r.shown == 0.005f);                    // inside the step: untouched, not eased
        // The tile's own light moved by 0.15 stop or more: a real change, let go. Just under it: held.
        r = Region(0.20f, 1.0f, 0.0f, 1.0f + Cad::kLightInputStops + 0.001f, 1);
        assert(r.gainStops == 0.0f);
        r = Region(0.20f, 1.0f, 0.0f, 1.0f + Cad::kLightInputStops - 0.01f, 1);
        assert(r.gainStops != 0.0f);
        // Motion at the release speed lets go; under it, held; not finite reads as still.
        r = Region(0.20f, 1.0f, 0.0f, 1.0f, 1, kDt, LL::kRegionMotionPixels);
        assert(r.gainStops == 0.0f);
        r = Region(0.20f, 1.0f, 0.0f, 1.0f, 1, kDt, LL::kRegionMotionPixels - 0.1f);
        assert(r.gainStops != 0.0f);
        r = Region(0.20f, 1.0f, 0.0f, 1.0f, 1, kDt, NAN);
        assert(r.gainStops != 0.0f);
        // The model's answer really changed (drift past the release band): let go; inside the low edge, held whole.
        r = Region(LL::kRegionReleaseHigh + 0.01f, 1.0f, 0.0f, 1.0f, 1);
        assert(r.gainStops == 0.0f && r.shown == LL::kRegionReleaseHigh + 0.01f);
        r = Region(LL::kRegionReleaseLow - 0.01f, 1.0f, 0.0f, 1.0f, 1);
        assert(Near(r.shown, step));
        // The hold weakens monotonically across the release band: what is shown never moves less for a larger drift, and
        // reaches the answer at the top of the band.
        float previousShown = -1e9f;
        for (float drift = LL::kRegionReleaseLow - 0.05f; drift <= LL::kRegionReleaseHigh + 0.05f; drift += 0.005f)
        {
            const float shown = Region(drift, 1.0f, 0.0f, 1.0f, 1).shown;
            assert(shown >= previousShown - 1e-6f && shown <= drift + 1e-6f);
            previousShown = shown;
        }
        // The gain never exceeds the clamp whatever the numbers say.
        r = CadenceLimitRegion(5.0f, 1.0f, 0.0f, 1.0f, 1, 100.0f, 0.15f, 100.0f, 200.0f, 0.0f, 6.0f, 1.0f);
        assert(r.gainStops == 0.0f);
        r = CadenceLimitRegion(5.0f, 1.0f, 0.0f, 1.0f, 1, 0.0f, 0.15f, 100.0f, 200.0f, 0.0f, 6.0f, 1.0f);
        assert(Near(r.gainStops, -1.0f) && Near(r.shown, 4.0f));
    }

    // ---- Lamp breathing in one region of a still picture: the model's edit in tile 3 swings +-0.2 stops with the
    // frame's own light still. Unlimited, that region's light moves by about 0.05 stop a frame; limited, by the
    // step (0.0125 stop a frame at 60 fps), and the tiles that did not breathe are untouched.
    {
        Row row;
        for (int x = 0; x < W; ++x)
            row.proxy[x] = 0.35f + 0.25f * (float) (x % 7) / 6.0f; // texture: the detail
        Region_ state;
        float worstRaw = 0.0f, worstLimited = 0.0f, otherTiles = 0.0f;
        float previousRaw = 0.0f, previousLimited = 0.0f;
        for (int frame = 0; frame < 240; ++frame)
        {
            const float phase = std::sin((float) frame * 0.25f);
            for (int x = 3 * Tile; x < 4 * Tile; ++x)
                row.edit[x] = 0.20f * phase;
            const auto out = Limit(row, state, frame > 0, kDt);
            const float raw = TileLight(
                [&]
                {
                    std::vector<float> shown(W);
                    for (int x = 0; x < W; ++x)
                        shown[x] = row.Shown(x);
                    return shown;
                }(),
                3);
            const float limited = TileLight(out, 3);
            if (frame > 0)
            {
                worstRaw = std::max(worstRaw, std::fabs(raw - previousRaw));
                if (frame > 30)
                    worstLimited = std::max(worstLimited, std::fabs(limited - previousLimited));
            }
            previousRaw = raw;
            previousLimited = limited;
            for (int t : { 0, 1, 6, 7 })
            {
                std::vector<float> shown(W);
                for (int x = 0; x < W; ++x)
                    shown[x] = row.Shown(x);
                otherTiles = std::max(otherTiles, std::fabs(TileLight(out, t) - TileLight(shown, t)));
            }
        }
        // The raw swing steps by more than the step. The limited one moves at the step in the middle of the tile and at
        // the raw swing's own rate at its very edge, where the bilinear gain blends in the neighbour's (which held
        // nothing): on average over the tile, a quarter of the raw swing and three quarters of the step. That is a tent a
        // tile wide, not a box, and this is the bound it gives.
        assert(worstRaw > 3.0f * LL::RegionStep(kDt));
        assert(worstLimited <= 0.25f * worstRaw + 0.75f * LL::RegionStep(kDt) + 1e-3f);
        assert(worstLimited < 0.5f * worstRaw);
        assert(otherTiles < 1e-3f);
    }

    // ---- The detail is not removed: the same gain multiplies a texture, so its contrast in stops is the answer's own.
    {
        Row row;
        for (int x = 0; x < W; ++x)
            row.proxy[x] = 0.30f + 0.30f * (float) (x % 5) / 4.0f;
        Region_ state;
        for (int x = 0; x < W; ++x)
            row.edit[x] = 0.0f;
        Limit(row, state, false, kDt);
        for (int x = 3 * Tile; x < 4 * Tile; ++x)
            row.edit[x] = 0.30f; // the lamp jumps by 0.3 stop in a still picture
        const auto out = Limit(row, state, true, kDt);
        // Pixel contrast inside the tile, in stops, before and after the gain.
        for (int x = 3 * Tile + 4; x < 4 * Tile - 4; ++x)
        {
            const float before = std::log2(CadenceSrgbToLinear(row.Shown(x + 1)) / CadenceSrgbToLinear(row.Shown(x)));
            const float after = std::log2(CadenceSrgbToLinear(out[x + 1]) / CadenceSrgbToLinear(out[x]));
            assert(Near(before, after, 0.01f));
        }
    }

    // ---- A real change goes through at once: the picture's own light steps by 0.5 stop in tile 2.
    {
        Row row;
        Region_ state;
        Limit(row, state, false, kDt);
        Limit(row, state, true, kDt);
        for (int x = 2 * Tile; x < 3 * Tile; ++x)
        {
            row.proxy[x] = 0.55f;
            row.edit[x] = 0.5f;
        }
        const auto out = Limit(row, state, true, kDt);
        for (int x = 2 * Tile + 8; x < 3 * Tile - 8; ++x)
            assert(Near(out[x], row.Shown(x), 1e-6f));
    }

    // ---- A reset starts over: a lamp that jumped and the gate that says "reset" show the answer as it is.
    {
        Row row;
        Region_ state;
        Limit(row, state, false, kDt);
        Limit(row, state, true, kDt);
        for (int x = 0; x < W; ++x)
            row.edit[x] = 0.25f;
        const auto held = Limit(row, state, true, kDt);
        assert(!Near(held[100], row.Shown(100), 1e-4f));
        Region_ fresh;
        const auto reset = Limit(row, fresh, false, kDt);
        assert(Near(reset[100], row.Shown(100), 1e-6f));
    }

    // ---- A black region (nothing measurable) is neither held nor poisoned, and its state is not valid afterwards.
    {
        Row row;
        for (int x = 0; x < W; ++x)
            row.proxy[x] = 0.0f;
        Region_ state;
        const auto out = Limit(row, state, true, kDt);
        for (int t = 0; t < Tiles; ++t)
            assert(state.valid[t] == 0.0f && state.gain[t] == 0.0f);
        for (int x = 0; x < W; ++x)
            assert(isfinite(out[x]));
    }

    // ---- The global rule is the cadence's, unchanged: the mean light held to 1.5 stops a second.
    {
        const float step = Cad::LimitStep(kDt);
        auto r = CadenceLimitLight(0.20f, 1.0f, 0.0f, 1.0f, 1, step, Cad::kLightInputStops, Cad::kLightMaxGainStops);
        assert(Near(r.shown, step) && Near(step, 1.5f / 60.0f));
        r = CadenceLimitLight(0.20f, 1.0f, 0.0f, 1.0f + 0.2f, 1, step, Cad::kLightInputStops, Cad::kLightMaxGainStops);
        assert(r.gainStops == 0.0f);
    }

    std::puts("light limiter: ok");
    return 0;
}

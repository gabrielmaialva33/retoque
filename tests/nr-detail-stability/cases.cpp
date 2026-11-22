// Detail stability (docs/dlssnr/design/detail-stability.md): the production per-pixel rule the shader runs
// (dlssnr_detail_stability_rule.h) over synthetic frame sequences, and the production gate (DlssNr_DetailStability.h). The
// shader's texture loads become array lookups with the same clamping; the dispatch becomes a loop. Not a GPU.
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <vector>

#include <dlssnr/DlssNr_DetailStability.h>

using std::abs;
using std::exp2;
using std::floor;
using std::isfinite;
using std::log2;
using std::max;
using std::min;

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
struct float4
{
    float x = 0.0f, y = 0.0f, z = 0.0f, w = 0.0f;
    float4() = default;
    float4(float a, float b, float c, float d) : x(a), y(b), z(c), w(d) {}
};
inline float3 operator+(float3 a, float3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
inline float3 operator-(float3 a, float3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
inline float3 operator*(float3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
inline int clamp(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float clamp(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
inline float saturate(float v) { return clamp(v, 0.0f, 1.0f); }
inline float smoothstep(float a, float b, float v)
{
    const float t = saturate((v - a) / (b - a));
    return t * t * (3.0f - 2.0f * t);
}

namespace
{
template <class T> struct Image
{
    int w = 0, h = 0;
    std::vector<T> px;
    void Resize(int width, int height, T value = T()) { w = width, h = height, px.assign(size_t(w) * h, value); }
    T At(int x, int y) const { return px[size_t(clamp(y, 0, h - 1)) * w + clamp(x, 0, w - 1)]; }
    T& Ref(int x, int y) { return px[size_t(y) * w + x]; }
};

Image<float3> g_proxy, g_answer;
Image<float2> g_motion;
Image<float4> g_history;
Image<float> g_ui;
float Round16(float v) { return float(_Float16(v)); }
} // namespace

#define DS_FN inline
#define DS_UNROLL
#define DS_PROXY(ax_, ay_) g_proxy.At(ax_, ay_)
#define DS_ANSWER(ax_, ay_) g_answer.At(ax_, ay_)
#define DS_MOTION(ax_, ay_) g_motion.At(ax_, ay_)
#define DS_HISTORY(ax_, ay_) g_history.At(ax_, ay_)
#define DS_UI(ax_, ay_) g_ui.At(ax_, ay_)
#define DS_ROUND16(v_) Round16(v_)
#include <shaders/dlssnr/precompile/dlssnr_detail_stability_rule.h>

namespace DS = DlssNr::DetailStability;

namespace
{
constexpr int W = 48;
constexpr int H = 32;

DsParams Shipped(bool valid, float alpha = DS::kDefaultBlend, bool hold = false)
{
    DsParams P {};
    P.width = W;
    P.height = H;
    P.alpha = alpha;
    P.holdRepeats = hold ? 1 : 0;
    P.historyValid = valid ? 1 : 0;
    P.haveUi = 0;
    P.bandLow = DS::kBandLow;
    P.bandHigh = DS::kBandHigh;
    return P;
}

// A frame of the pass: every pixel through the rule, the history written for the next one.
struct Frame
{
    Image<float3> answer;
    Image<float4> history;
    float weightSum = 0.0f;
};

Frame Run(const DsParams& P)
{
    Frame f;
    f.answer.Resize(W, H);
    f.history.Resize(W, H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
        {
            const DsResult r = DsFilter(x, y, P);
            f.answer.Ref(x, y) = r.answer;
            f.history.Ref(x, y) = r.history;
            f.weightSum += r.weight;
        }
    return f;
}

float Texture(int x, int y, int seed = 0)
{
    // Foliage-like: contrast at the pixel scale, deterministic.
    const unsigned h = (unsigned) (x * 73856093u) ^ (unsigned) (y * 19349663u) ^ (unsigned) (seed * 83492791u);
    return 0.25f + 0.5f * (float) ((h >> 7) & 255u) / 255.0f;
}

void Scene(int shiftX, int shiftY)
{
    g_proxy.Resize(W, H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
        {
            const float v = Texture(x + shiftX, y + shiftY);
            g_proxy.Ref(x, y) = float3(v, v * 0.9f, v * 0.8f);
        }
}

// The model's answer: the proxy times a per-frame, per-pixel re-decision of +-0.15 stop (what boils), plus a steady part.
void Answer(int frame, float amplitude, int shiftX = 0, int shiftY = 0)
{
    g_answer.Resize(W, H);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x)
        {
            const unsigned h = (unsigned) ((x + shiftX) * 2654435761u) ^ (unsigned) ((y + shiftY) * 40503u) ^
                               (unsigned) (frame * 69069u);
            const float noise = ((float) ((h >> 9) & 1023u) / 1023.0f - 0.5f) * 2.0f;
            const float p = g_proxy.At(x, y).x;
            const float k = exp2(0.10f + amplitude * noise);
            const float3 q = g_proxy.At(x, y);
            g_answer.Ref(x, y) = float3(std::min(q.x * k, 1.5f), std::min(q.y * k, 1.5f), std::min(q.z * k, 1.5f));
            (void) p;
        }
}

void Motion(float vx, float vy)
{
    g_motion.Resize(W, H, float2(vx, vy));
}

float Flicker(const Image<float3>& a, const Image<float3>& b)
{
    float sum = 0.0f;
    int n = 0;
    for (int y = 2; y < H - 2; ++y)
        for (int x = 2; x < W - 2; ++x)
        {
            sum += abs(a.At(x, y).y - b.At(x, y).y);
            ++n;
        }
    return sum / (float) n;
}
} // namespace

int main()
{
    // ---- The configured blend, the gate and the refusals.
    assert(DS::Blend(0.15f) == 0.15f && DS::Blend(0.0f) == DS::kMinBlend && DS::Blend(7.0f) == DS::kMaxBlend);
    assert(DS::Blend(NAN) == DS::kDefaultBlend && DS::Blend(INFINITY) == DS::kDefaultBlend);
    {
        DS::Inputs in;
        in.enabled = true;
        in.gameGuides = true;
        assert(DS::Refusal(in) == DS::Reason::None);
        DS::Inputs off = in;
        off.enabled = false;
        assert(DS::Refusal(off) == DS::Reason::Off);
        DS::Inputs proxy = in;
        proxy.driverProxy = true;
        assert(DS::Refusal(proxy) == DS::Reason::DriverProxy);
        DS::Inputs before = in;
        before.beforeUpscale = true;
        assert(DS::Refusal(before) == DS::Reason::BeforeUpscale);
        DS::Inputs guides = in;
        guides.gameGuides = false;
        assert(DS::Refusal(guides) == DS::Reason::NoGameGuides);
        DS::Inputs fg = in;
        fg.frameGeneration = true;
        assert(DS::Refusal(fg) == DS::Reason::FrameGeneration);
        DS::Inputs cadence = in;
        cadence.cadence = true;
        assert(DS::Refusal(cadence) == DS::Reason::Cadence);
        DS::Inputs surfaces = in;
        surfaces.surfaces = false;
        assert(DS::Refusal(surfaces) == DS::Reason::NoSurfaces);

        DS::Gate g;
        auto d = g.Decide(off, 1, 0, 7, false);
        assert(!d.run && !d.historyValid);
        d = g.Decide(in, 1, 0, 7, false);
        assert(d.run && !d.historyValid);            // the first frame has no history
        g.Ran(true);
        d = g.Decide(in, 2, 16, 7, false);
        assert(d.run && d.historyValid);
        g.Ran(true);
        d = g.Decide(in, 2, 17, 7, false);
        assert(!d.run && d.reason == DS::Reason::NoPresentCount); // a second view in one present
        d = g.Decide(in, 3, 33, 7, false);
        assert(d.run && !d.historyValid);            // ... and what followed it starts over
        g.Ran(true);
        d = g.Decide(in, 4, 49, 8, false);
        assert(d.run && !d.historyValid);            // a changed signature
        g.Ran(true);
        d = g.Decide(in, 5, 65, 8, true);
        assert(d.run && !d.historyValid);            // the model's history reset
        g.Ran(true);
        d = g.Decide(in, 6, 65 + DlssNr::Cadence::kGapMs + 1, 8, false);
        assert(d.run && !d.historyValid);            // a gap
        g.Ran(false);
        d = g.Decide(in, 7, 65 + DlssNr::Cadence::kGapMs + 2, 8, false);
        assert(d.run && !d.historyValid);            // a frame the pass did not record
        g.Ran(true);
        d = g.Decide(in, 8, 65 + DlssNr::Cadence::kGapMs + 3, 8, false);
        assert(d.run && d.historyValid);
        g.Ran(true);
        d = g.Decide(cadence, 9, 65 + DlssNr::Cadence::kGapMs + 4, 8, false);
        assert(!d.run);
        d = g.Decide(in, 10, 65 + DlssNr::Cadence::kGapMs + 5, 8, false);
        assert(d.run && !d.historyValid);            // a refusal drops the history
        g.Ran(true);
        DS::Gate z;
        assert(z.Decide(in, 0, 0, 1, false).run);
        z.Ran(true);
        assert(z.Decide(in, 0, 16, 1, false).historyValid); // no present count: never "a second call"
        z.Reset();
        assert(!z.Filtered());
    }

    // ---- The edit and its inverse; black does not blow up.
    {
        const float3 p(0.4f, 0.2f, 0.1f), a(0.5f, 0.1f, 0.1f);
        const float3 e = DsEdit(a, p);
        const float3 back = DsReconstruct(p, e);
        assert(abs(back.x - a.x) < 1e-5f && abs(back.y - a.y) < 1e-5f && abs(back.z - a.z) < 1e-5f);
        const float3 black = DsEdit(float3(0.05f, 0.05f, 0.05f), float3(0.0f, 0.0f, 0.0f));
        assert(isfinite(black.x) && black.x < 9.0f);
        assert(DsReconstruct(p, float3(-30.0f, -30.0f, -30.0f)).x == 0.0f);
    }

    // ---- No history, or alpha 1: the model's answer exactly, and the history is written either way.
    {
        Scene(0, 0);
        Answer(0, 0.15f);
        Motion(0.0f, 0.0f);
        g_history.Resize(W, H);
        for (bool valid : { false, true })
        {
            const Frame f = Run(Shipped(valid, valid ? 1.0f : 0.15f));
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x)
                {
                    assert(f.answer.At(x, y).x == g_answer.At(x, y).x && f.answer.At(x, y).z == g_answer.At(x, y).z);
                    assert(f.history.At(x, y).w >= 0.0f);
                }
        }
    }

    // ---- What is stored is what is shown from: the filtered edit is a half (the history's format) before it is stored and
    // before the answer is made of it, so a repeat is bit for bit.
    {
        Scene(0, 0);
        Motion(0.0f, 0.0f);
        Answer(1, 0.2f);
        g_history.Resize(W, H);
        Frame first = Run(Shipped(false));
        g_history = first.history;
        Answer(2, 0.2f);
        const Frame second = Run(Shipped(true));
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
            {
                const float4 h = second.history.At(x, y);
                assert(h.x == Round16(h.x) && h.y == Round16(h.y) && h.z == Round16(h.z) && h.w == Round16(h.w));
            }
    }

    // ---- A history the host says is no good (a reset, a gap) is not used even when it looks perfect.
    {
        Scene(0, 0);
        Motion(0.0f, 0.0f);
        Answer(3, 0.2f);
        g_history.Resize(W, H);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                g_history.Ref(x, y) = float4(0.5f, 0.5f, 0.5f, DsSignal(g_proxy.At(x, y)));
        const DsResult r = DsFilter(20, 15, Shipped(false));
        assert(r.weight == 1.0f && r.answer.x == g_answer.At(20, 15).x);
        assert(DsFilter(20, 15, Shipped(true)).weight < 1.0f);
    }

    // ---- A still scene whose edit boils every frame: the filtered answer flickers less than the model's, by a good
    // margin, and the mean light does not move.
    {
        Scene(0, 0);
        Motion(0.0f, 0.0f);
        g_history.Resize(W, H);
        Frame previousFiltered;
        float rawFlicker = 0.0f, filteredFlicker = 0.0f;
        Image<float3> previousRaw;
        double rawMean = 0.0, filteredMean = 0.0;
        for (int frame = 0; frame < 60; ++frame)
        {
            Answer(frame, 0.15f);
            const Frame f = Run(Shipped(frame > 0));
            g_history = f.history;
            if (frame > 20)
            {
                rawFlicker += Flicker(g_answer, previousRaw);
                filteredFlicker += Flicker(f.answer, previousFiltered.answer);
            }
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x)
                {
                    rawMean += g_answer.At(x, y).y;
                    filteredMean += f.answer.At(x, y).y;
                }
            previousRaw = g_answer;
            previousFiltered = f;
        }
        assert(filteredFlicker < 0.45f * rawFlicker);
        assert(std::fabs(filteredMean / rawMean - 1.0) < 0.03);
    }

    // ---- Panning: the content moves one pixel a frame with the game's vector; the model's boiling does not move with
    // it. Filtering with the vector beats filtering without it, and beats the model alone.
    {
        auto run = [&](bool useMotion)
        {
            g_history.Resize(W, H);
            Frame previous;
            Image<float3> previousRaw;
            float raw = 0.0f, filtered = 0.0f;
            for (int frame = 0; frame < 50; ++frame)
            {
                Scene(frame, 0);
                Answer(frame, 0.12f, frame, 0);
                Motion(useMotion ? 1.0f : 0.0f, 0.0f); // this frame's pixel was one pixel to the right last frame
                const Frame f = Run(Shipped(frame > 0));
                g_history = f.history;
                if (frame > 15)
                {
                    // Compare consecutive frames after undoing the pan (pixel x now is pixel x+1 before).
                    Image<float3> shifted, shiftedRaw;
                    shifted.Resize(W, H);
                    shiftedRaw.Resize(W, H);
                    for (int y = 0; y < H; ++y)
                        for (int x = 0; x < W; ++x)
                        {
                            shifted.Ref(x, y) = previous.answer.At(x + 1, y);
                            shiftedRaw.Ref(x, y) = previousRaw.At(x + 1, y);
                        }
                    raw += Flicker(g_answer, shiftedRaw);
                    filtered += Flicker(f.answer, shifted);
                }
                previous = f;
                previousRaw = g_answer;
            }
            return std::pair<float, float> { raw, filtered };
        };
        const auto with = run(true);
        const auto without = run(false);
        assert(with.second < 0.8f * with.first);
        assert(with.second < without.second);
    }

    // ---- The clip: a bright edit that the current frame no longer has anywhere in its 3x3 cannot persist. The history
    // holds +1.5 stops on a pixel; the current edit is +0.1 everywhere: the filtered edit is inside the current range, so
    // it is the current one, at once.
    {
        Scene(0, 0);
        Motion(0.0f, 0.0f);
        g_answer.Resize(W, H);
        g_history.Resize(W, H);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
            {
                const float3 q = g_proxy.At(x, y);
                g_answer.Ref(x, y) = float3(q.x * 1.0718f, q.y * 1.0718f, q.z * 1.0718f); // +0.1 stop
                const float s = DsSignal(q);
                g_history.Ref(x, y) = float4(1.5f, 1.5f, 1.5f, s);
            }
        const Frame f = Run(Shipped(true));
        for (int y = 2; y < H - 2; ++y)
            for (int x = 2; x < W - 2; ++x)
                assert(abs(f.answer.At(x, y).y - g_answer.At(x, y).y) < 0.01f * max(g_answer.At(x, y).y, 0.1f));
    }

    // ---- Rejection: where the input moved under the history, the model's answer goes through whole. A cut: every pixel's
    // input brightens by far more than the band.
    {
        Scene(0, 0);
        Motion(0.0f, 0.0f);
        g_history.Resize(W, H);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                g_history.Ref(x, y) = float4(0.0f, 0.0f, 0.0f, 0.0f); // the frame before was black
        Answer(0, 0.2f);
        const Frame f = Run(Shipped(true));
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
            {
                assert(f.answer.At(x, y).x == g_answer.At(x, y).x);
                assert(DsFilter(x, y, Shipped(true)).weight == 1.0f);
            }
    }
    // ... and an input change inside the band (under 0.02 in c / (1 + c)) is not one: the filter holds.
    {
        Scene(0, 0);
        Motion(0.0f, 0.0f);
        g_history.Resize(W, H);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                g_history.Ref(x, y) = float4(0.1f, 0.1f, 0.1f, DsSignal(g_proxy.At(x, y)) + 0.005f);
        Answer(0, 0.2f);
        for (int y = 3; y < H - 3; ++y)
            for (int x = 3; x < W - 3; ++x)
                assert(DsFilter(x, y, Shipped(true)).weight < 0.5f);
    }
    // ... and the band is near binary: nothing at its bottom, whole at its top, a ramp only between.
    {
        Scene(0, 0);
        Motion(0.0f, 0.0f);
        Answer(0, 0.2f);
        float previous = -1.0f;
        for (float shift = 0.0f; shift <= 0.2f; shift += 0.004f)
        {
            g_history.Resize(W, H);
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x)
                    g_history.Ref(x, y) = float4(0.0f, 0.0f, 0.0f, 1.0f); // far above any input: outside by 1 - range
            // Put the history input a distance `shift` above the 3x3 maximum at the probe pixel.
            const int px = 20, py = 15;
            float shi = 0.0f;
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                    shi = std::max(shi, DsSignal(g_proxy.At(px + dx, py + dy)));
            g_history.Ref(px, py) = float4(0.0f, 0.0f, 0.0f, shi + shift);
            const float w = DsFilter(px, py, Shipped(true)).weight;
            assert(w >= previous - 1e-6f);
            previous = w;
            if (shift < 0.015f)
                assert(w <= DS::kDefaultBlend + 1e-6f);
            if (shift > 0.105f)
                assert(w == 1.0f);
        }
    }
    // ... and it is the 3x3 RANGE that is judged, not the pixel: a history whose input is a neighbour's value (what a
    // sub-pixel pan of a high-contrast texture reprojects to) is inside the range and is not rejected, though it differs
    // from the pixel's own input by far more than the band.
    {
        Scene(0, 0);
        Motion(0.0f, 0.0f);
        Answer(0, 0.2f);
        int found = 0;
        for (int y = 3; y < H - 3 && found < 20; ++y)
            for (int x = 3; x < W - 4 && found < 20; ++x)
            {
                const float own = DsSignal(g_proxy.At(x, y)), right = DsSignal(g_proxy.At(x + 1, y));
                if (abs(own - right) < 0.12f)
                    continue;
                g_history.Resize(W, H);
                for (int yy = 0; yy < H; ++yy)
                    for (int xx = 0; xx < W; ++xx)
                        g_history.Ref(xx, yy) = float4(0.0f, 0.0f, 0.0f, DsSignal(g_proxy.At(xx, yy)));
                g_history.Ref(x, y).w = right;
                assert(DsFilter(x, y, Shipped(true)).weight < 1.0f);
                ++found;
            }
        assert(found > 0);
    }
    // ... off the picture, or a texel that stored nothing, or a vector that is not a number: rejected.
    {
        Scene(0, 0);
        Answer(0, 0.2f);
        g_history.Resize(W, H);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                g_history.Ref(x, y) = float4(0.0f, 0.0f, 0.0f, DsSignal(g_proxy.At(x, y)));
        Motion(-100.0f, 0.0f);
        assert(DsFilter(10, 10, Shipped(true)).weight == 1.0f);
        Motion(0.0f, 0.0f);
        g_history.Ref(10, 10).w = -1.0f;
        assert(DsFilter(10, 10, Shipped(true)).weight == 1.0f);
        Motion(NAN, 0.0f);
        assert(DsFilter(11, 11, Shipped(true)).weight == 1.0f);
        // A texel that stored nothing reached by a sliver of the bilinear weight still rejects: the blend of its
        // negative marker with the rest would otherwise pass for a value inside the band.
        g_history.Ref(10, 10).w = DsSignal(g_proxy.At(10, 10));
        g_history.Ref(21, 20).w = -1.0f;
        Motion(0.01f, 0.0f);
        assert(DsFilter(20, 20, Shipped(true)).weight == 1.0f);
        g_history.Ref(21, 20).w = DsSignal(g_proxy.At(21, 20));
        assert(DsFilter(20, 20, Shipped(true)).weight < 1.0f);
        // The same from the other side: the far texel is the one that stored nothing, reached by 1% of the weight.
        Motion(-0.01f, 0.0f);
        g_history.Ref(19, 20).w = -1.0f;
        assert(DsFilter(20, 20, Shipped(true)).weight == 1.0f);
        g_history.Ref(19, 20).w = DsSignal(g_proxy.At(19, 20));
        assert(DsFilter(20, 20, Shipped(true)).weight < 1.0f);
    }

    // ---- The dead end, side by side: an object moves off a texture and uncovers new content. A plain blend of the old
    // answer (the history, with no clip and no rejection) leaves the old edit in the uncovered region; this does not.
    {
        const int objX0 = 14, objX1 = 26;
        auto sceneWith = [&](int offset)
        {
            g_proxy.Resize(W, H);
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x)
                {
                    const bool inObject = x >= objX0 + offset && x < objX1 + offset;
                    const float v = inObject ? 0.85f : 0.30f;
                    g_proxy.Ref(x, y) = float3(v, v, v);
                }
        };
        auto answerWith = [&](int offset)
        {
            g_answer.Resize(W, H);
            for (int y = 0; y < H; ++y)
                for (int x = 0; x < W; ++x)
                {
                    const bool inObject = x >= objX0 + offset && x < objX1 + offset;
                    const float k = inObject ? exp2(0.7f) : exp2(0.0f);
                    const float3 q = g_proxy.At(x, y);
                    g_answer.Ref(x, y) = float3(q.x * k, q.y * k, q.z * k);
                }
        };
        sceneWith(0);
        answerWith(0);
        Motion(0.0f, 0.0f);
        g_history.Resize(W, H);
        Frame f = Run(Shipped(false));
        g_history = f.history;
        // The object jumps 14 pixels right, the game says so for the pixels it covers, and the background is still.
        sceneWith(14);
        answerWith(14);
        Motion(0.0f, 0.0f);
        const Frame next = Run(Shipped(true));
        // A pixel the object left (x = 16): the old edit there was +0.7 stop; the new answer is the plain background.
        const float truth = g_answer.At(16, 16).y;
        const float ours = next.answer.At(16, 16).y;
        // The plain blend of the old answer: 0.85 old + 0.15 new, no clip, no rejection.
        const float old = f.answer.At(16, 16).y;
        const float naive = 0.85f * old + 0.15f * truth;
        assert(abs(ours - truth) < 0.02f);
        assert(abs(naive - truth) > 0.1f);
    }

    // ---- Hold repeats: an input shown before exactly, with no motion, is shown exactly as before; without the option it
    // is not; where the input differs even slightly the filter decides.
    {
        Scene(0, 0);
        Motion(0.0f, 0.0f);
        Answer(1, 0.2f);
        g_history.Resize(W, H);
        Frame first = Run(Shipped(false));
        g_history = first.history;
        // Frame 2: a first filtered frame (alpha 0.15), so the history now holds a filtered edit.
        Answer(2, 0.2f);
        Frame second = Run(Shipped(true));
        g_history = second.history;
        // Frame 3: the SAME input, a new opinion of it.
        Answer(3, 0.2f);
        const Frame held = Run(Shipped(true, DS::kDefaultBlend, true));
        const Frame filtered = Run(Shipped(true, DS::kDefaultBlend, false));
        int exact = 0, differs = 0;
        for (int y = 3; y < H - 3; ++y)
            for (int x = 3; x < W - 3; ++x)
            {
                if (held.answer.At(x, y).y == second.answer.At(x, y).y)
                    ++exact;
                if (filtered.answer.At(x, y).y != second.answer.At(x, y).y)
                    ++differs;
            }
        assert(exact == (W - 6) * (H - 6));
        assert(differs > (W - 6) * (H - 6) / 2);
        // The input moves by one step of an 8-bit frame at one pixel: that pixel is not held.
        g_proxy.Ref(20, 15).x += 1.0f / 255.0f;
        g_proxy.Ref(20, 15).y += 1.0f / 255.0f;
        g_proxy.Ref(20, 15).z += 1.0f / 255.0f;
        const DsResult moved = DsFilter(20, 15, Shipped(true, DS::kDefaultBlend, true));
        assert(moved.weight > 0.0f);
        // A motion that is not exactly zero is not a repeat either.
        Motion(0.001f, 0.0f);
        Scene(0, 0);
        assert(DsFilter(20, 15, Shipped(true, DS::kDefaultBlend, true)).weight > 0.0f);
    }

    // ---- A vector that is not zero is not a repeat even where the input happens to be the same (a flat region panning):
    // the exact hold is for an input that did not move at all.
    {
        g_proxy.Resize(W, H, float3(0.4f, 0.4f, 0.4f));
        Answer(11, 0.2f);
        Motion(1.0f, 0.0f);
        g_history.Resize(W, H, float4(0.0f, 0.0f, 0.0f, DsSignal(float3(0.4f, 0.4f, 0.4f))));
        assert(DsFilter(20, 15, Shipped(true, DS::kDefaultBlend, true)).weight == DS::kDefaultBlend);
        Motion(0.0f, 0.0f);
        assert(DsFilter(20, 15, Shipped(true, DS::kDefaultBlend, true)).weight == 0.0f);
    }

    // ---- The interface keeps the model's answer and stores its own edit; non-finite pixels pass and store nothing.
    {
        Scene(0, 0);
        Motion(0.0f, 0.0f);
        Answer(5, 0.2f);
        g_history.Resize(W, H);
        for (int y = 0; y < H; ++y)
            for (int x = 0; x < W; ++x)
                g_history.Ref(x, y) = float4(1.0f, 1.0f, 1.0f, DsSignal(g_proxy.At(x, y)));
        g_ui.Resize(W, H, 1.0f);
        DsParams P = Shipped(true);
        P.haveUi = 1;
        const DsResult ui = DsFilter(10, 10, P);
        assert(ui.answer.x == g_answer.At(10, 10).x && ui.weight == 1.0f);
        g_ui.Resize(W, H, 0.0f);
        assert(DsFilter(10, 10, P).weight < 1.0f);
        // Half interface: halfway between the filtered answer and the model's, and the edit stored is halfway too.
        const DsResult none = DsFilter(10, 10, P);
        g_ui.Resize(W, H, 0.5f);
        P.haveUi = 1;
        const DsResult half = DsFilter(10, 10, P);
        const float3 eNone = float3(none.history.x, none.history.y, none.history.z);
        const float3 eRaw = DsEdit(g_answer.At(10, 10), g_proxy.At(10, 10));
        const float3 expected = DsReconstruct(g_proxy.At(10, 10), DsRound3((eNone + eRaw) * 0.5f));
        assert(abs(half.answer.y - expected.y) < 1e-4f);
        assert(abs(half.history.y - 0.5f * (none.history.y + DsEdit(g_answer.At(10, 10), g_proxy.At(10, 10)).y)) < 2e-3f);
        assert(half.weight < 1.0f && none.answer.y != g_answer.At(10, 10).y);
        g_ui.Resize(W, H, 0.0f);
        g_answer.Ref(12, 12).y = NAN;
        const DsResult bad = DsFilter(12, 12, Shipped(true));
        assert(bad.history.w < 0.0f && bad.weight == 1.0f);
        // A neighbour that is not finite does not poison the pixels around it.
        const DsResult around = DsFilter(13, 12, Shipped(true));
        assert(isfinite(around.answer.x) && isfinite(around.history.x));
    }

    // ---- Stationary fixed point: with the answer constant and the input constant, the filtered answer is the answer.
    {
        Scene(0, 0);
        Motion(0.0f, 0.0f);
        Answer(9, 0.0f);
        g_history.Resize(W, H);
        Frame f = Run(Shipped(false));
        for (int frame = 0; frame < 30; ++frame)
        {
            g_history = f.history;
            f = Run(Shipped(true));
        }
        for (int y = 2; y < H - 2; ++y)
            for (int x = 2; x < W - 2; ++x)
                assert(abs(f.answer.At(x, y).y - g_answer.At(x, y).y) < 0.004f);
    }

    std::puts("detail stability: ok");
    return 0;
}

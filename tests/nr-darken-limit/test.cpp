// Real HLSL writeback compiled with float32 intrinsics. The tests catch a missing,
// twice-applied or intermediate floor, black divisions and changes while off.
#include <algorithm>
#include <cassert>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <shaders/dlssnr/DlssNr_Common.h>

struct float3
{
    float x, y, z;
    float3(float a, float b, float c) : x(a), y(b), z(c) {}
};
float3 operator+(float3 a, float3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
float3 operator-(float3 a, float3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
float3 operator*(float3 a, float b) { return { a.x * b, a.y * b, a.z * b }; }
float3 max(float3 a, float b) { return { std::fmax(a.x, b), std::fmax(a.y, b), std::fmax(a.z, b) }; }
float dot(float3 a, float3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float min(float a, float b) { return std::fmin(a, b); }
bool isfinite(float a) { return std::isfinite(a); }
float saturate(float a) { return std::fmin(std::fmax(a, 0.0f), 1.0f); }
float3 lerp(float3 a, float3 b, float t) { return a + (b - a) * t; }
#include "composition.inc"

float3 Grey(float x) { return { x, x, x }; }
bool Bits(float3 a, float3 b) { return std::memcmp(&a, &b, sizeof(a)) == 0; }
void Near(float a, float b) { assert(std::fabs(a - b) <= std::max(1e-30f, std::fabs(b) * 2e-6f)); }
float Y(float3 a) { return a.x * 0.2126f + a.y * 0.7152f + a.z * 0.0722f; }
int main()
{
    const float3 zero = Grey(0);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    for (float invalid : { -1.0f, -0.0f, 0.0f, nan, inf, -inf })
        assert(DlssNr::MinimumRatio(invalid) == 0.0f);
    assert(DlssNr::MinimumRatio(.75f) == .75f);
    assert(DlssNr::MinimumRatio(2.0f) == 1.0f);
    // Literal oracle: a 0.1 result over original 1 with floor .75 must be .75,
    // never .5625 (.75 applied twice), and black must recover without a ratio division.
    for (float candidate : { 0.0f, 0.1f, 0.5f })
        Near(Finish(Grey(candidate), 1, zero, Grey(1), .75f).x, .75f);
    // Passthrough's normScale is 1; linear HDR's varies. Bound the raw reference,
    // after denormalization, not the model proxy. Include positive and signed colours.
    for (float scale : { 1e-20f, .1f, 1.0f, 80.0f, 203.0f, 10000.0f })
        for (float3 original : { Grey(1), float3(.8f, .2f, .1f), float3(-.1f, 1, .5f) })
        {
            original = original * scale;
            const float3 rest { std::fmin(original.x, 0.0f), std::fmin(original.y, 0.0f), std::fmin(original.z, 0.0f) };
            for (float floor : { .25f, .75f, 1.0f })
                Near(Y(Finish(zero, scale, rest, original, floor)), Y(original) * floor);
        }
    // The existing guard/residual limit may already be stricter; never lower it,
    // nor change a brighter result. Also no floor against a nonpositive source.
    for (float candidate : { .75f, .8f, 1.0f, 2.0f, 100.0f })
        assert(Bits(Finish(Grey(candidate), 1, zero, Grey(1), .75f), Grey(candidate)));
    assert(Bits(Finish(Grey(.2f), 1, zero, Grey(0), 1), Grey(.2f)));
    assert(Bits(Finish(Grey(.2f), 1, zero, Grey(-1), 1), Grey(.2f)));
    // Default/off returns exactly the previous writeback, even with signed values,
    // nonfinite model values and large HDR scales. No off-path round trip is allowed.
    for (float value : { -1.0f, -0.0f, 0.0f, 1e-20f, .1234567f, 1.0f, 65504.0f, nan, inf })
        for (float scale : { .1f, 1.0f, 203.0f })
        {
            const float3 input { value, .1234567f, -.2f };
            const float3 rest { -.1f, 0, -.2f };
            const float3 old = max(input * scale, 0.0f) + rest;
            for (float off : { 0.0f, -0.0f, -1.0f, nan, inf })
                assert(Bits(Finish(input, scale, rest, Grey(1), off), old));
        }
    puts("PASS: final floor, exact .75 semantics, black, HDR/signed/near-zero, passthrough domain and off-path bits");
}

#include <dlssnr/DlssNr_Chain.h>
#include <dlssnr/DlssNr_PassClamp.h>

#include <cassert>
#include <cmath>
#include <cstring>
#include <limits>

namespace
{
bool BitEqual(float a, float b)
{
    uint32_t ua = 0, ub = 0;
    std::memcpy(&ua, &a, sizeof(float));
    std::memcpy(&ub, &b, sizeof(float));
    return ua == ub;
}

bool Near(float a, float b, float eps = 1e-5f) { return std::abs(a - b) <= eps; }
} // namespace

int main()
{
    namespace PC = DlssNr::PassClamp;
    using PC::Reference::float3;
    using PC::Reference::PassClampStep;

    // 1. Default off / activation gate.
    assert(!PC::Active(1, false, 1.0f));
    assert(!PC::Active(2, false, 1.0f));
    assert(!PC::Active(10, false, 1.0f));
    assert(!PC::Active(1, true, 1.0f));
    assert(!PC::Active(1, false, 0.5f));
    assert(PC::Active(2, true, 1.0f));
    assert(PC::Active(2, false, 0.99f));
    assert(PC::Active(2, false, 0.0f));
    assert(!PC::Active(2, false, std::numeric_limits<float>::quiet_NaN()));
    assert(!PC::NeedsSecondScratch(2, true, 1.0f));
    assert(!PC::NeedsSecondScratch(2, false, 0.5f));
    assert(PC::NeedsSecondScratch(3, true, 1.0f));
    assert(PC::NeedsSecondScratch(3, false, 0.5f));
    assert(!PC::NeedsSecondScratch(3, false, 1.0f));

    // 2. Finite identity when clamp=0 and feedback=1.0 (no lerp rounding, no [0, 1] crush).
    for (float v : { -0.25f, 0.0f, 0.1234567f, 0.5f, 1.0f, 2.75f })
    {
        const float3 raw(v, v * 0.5f - 0.1f, v * 1.3f);
        const float3 proxy(0.2f, 0.4f, 0.6f);
        const float3 out = PassClampStep(raw, proxy, 0u, 1.0f);
        assert(BitEqual(out.x, raw.x));
        assert(BitEqual(out.y, raw.y));
        assert(BitEqual(out.z, raw.z));
    }

    // 3. NaN/Inf sanitization falls back to proxy (not mid-grey 0.5), and non-finite proxy falls back to 0.
    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();
    {
        const float3 darkProxy(0.02f, 0.03f, 0.04f);
        const float3 badRaw(nan, inf, -inf);
        const float3 outOff = PassClampStep(badRaw, darkProxy, 0u, 1.0f);
        assert(Near(outOff.x, 0.02f) && Near(outOff.y, 0.03f) && Near(outOff.z, 0.04f));
        const float3 outOn = PassClampStep(badRaw, darkProxy, 1u, 1.0f);
        assert(Near(outOn.x, 0.02f) && Near(outOn.y, 0.03f) && Near(outOn.z, 0.04f));

        const float3 badProxy(nan, 0.1f, -inf);
        const float3 badBoth(nan, nan, 0.25f);
        const float3 outBoth = PassClampStep(badBoth, badProxy, 1u, 1.0f);
        assert(Near(outBoth.x, 0.0f) && Near(outBoth.y, 0.1f) && Near(outBoth.z, 0.25f));
    }

    // 4. Hue-preserving CubeScaleResidual on [0, 1]^3: overshoot and undershoot scale all three residual
    // components by the same scalar alpha in (0, 1], keeping chromatic direction intact.
    {
        // High-energy orange highlight overshoot: proxy=(0.5, 0.4, 0.2), raw=(1.5, 0.8, 0.3), d=(1.0, 0.4, 0.1).
        // Per-channel saturate would give (1.0, 0.8, 0.3) -> residual (0.5, 0.4, 0.1), shifting hue yellow!
        // Hue-preserving alpha = (1.0 - 0.5) / 1.0 = 0.5 -> (1.0, 0.6, 0.25).
        const float3 proxy(0.5f, 0.4f, 0.2f);
        const float3 raw(1.5f, 0.8f, 0.3f);
        const float3 out = PassClampStep(raw, proxy, 1u, 1.0f);
        assert(Near(out.x, 1.0f));
        assert(Near(out.y, 0.6f));
        assert(Near(out.z, 0.25f));

        // Negative undershoot: proxy=(0.2, 0.5, 0.6), raw=(-0.2, 0.1, 0.4), d=(-0.4, -0.4, -0.2).
        // Tightest bound is x reaching 0 at alpha = 0.2 / 0.4 = 0.5 -> (0.0, 0.3, 0.5).
        const float3 proxyNeg(0.2f, 0.5f, 0.6f);
        const float3 rawNeg(-0.2f, 0.1f, 0.4f);
        const float3 outNeg = PassClampStep(rawNeg, proxyNeg, 1u, 1.0f);
        assert(Near(outNeg.x, 0.0f));
        assert(Near(outNeg.y, 0.3f));
        assert(Near(outNeg.z, 0.5f));

        // Inside-gamut change stays untouched.
        const float3 inBox(0.75f, 0.35f, 0.25f);
        const float3 outIn = PassClampStep(inBox, proxy, 1u, 1.0f);
        assert(Near(outIn.x, inBox.x) && Near(outIn.y, inBox.y) && Near(outIn.z, inBox.z));
    }

    // 5. Passthrough float proxy outside [0, 1] is never crushed to [0, 1].
    {
        const float3 superWhiteProxy(1.6f, 0.5f, 0.25f);
        const float3 out = PassClampStep(superWhiteProxy, superWhiteProxy, 1u, 1.0f);
        assert(Near(out.x, 1.6f) && Near(out.y, 0.5f) && Near(out.z, 0.25f));
    }

    // 6. PassFeedback under-relaxation: 0 -> proxy, 0.5 -> midpoint, 1.0 -> bit-identical restored.
    {
        const float3 proxy(0.2f, 0.4f, 0.6f);
        const float3 raw(1.2f, 0.8f, 0.7f); // clamped -> alpha=0.8 -> restored=(1.0, 0.72, 0.68)
        const float3 full = PassClampStep(raw, proxy, 1u, 1.0f);
        assert(Near(full.x, 1.0f) && Near(full.y, 0.72f) && Near(full.z, 0.68f));

        const float3 zero = PassClampStep(raw, proxy, 1u, 0.0f);
        assert(Near(zero.x, proxy.x) && Near(zero.y, proxy.y) && Near(zero.z, proxy.z));

        const float3 half = PassClampStep(raw, proxy, 1u, 0.5f);
        assert(Near(half.x, 0.6f) && Near(half.y, 0.56f) && Near(half.z, 0.64f));

        const float3 nanFeedback = PassClampStep(raw, proxy, 1u, nan);
        assert(BitEqual(nanFeedback.x, full.x) && BitEqual(nanFeedback.y, full.y) && BitEqual(nanFeedback.z, full.z));
    }

    // 7. Zero SRV/UAV aliasing and same-temporal-age proxy across 2..10 sequential passes.
    enum SurfaceId : int
    {
        kNone = 0,
        kModelInput = 1,
        kOutput = 2,
        kPassPing = 3,
        kClamp0 = 4,
        kClamp1 = 5,
    };

    for (uint32_t totalPasses = 2; totalPasses <= DlssNr::PassConfig::MaxPasses; ++totalPasses)
    {
        DlssNr::Chain::Routing<SurfaceId> chain(kModelInput, kOutput, kPassPing);
        // Pass 0 reads kModelInput, writes kOutput.
        assert(chain.Input() == kModelInput);
        assert(chain.Output() == kOutput);
        chain.Success();

        SurfaceId prevPassInput = kModelInput;
        const SurfaceId clamp1 = totalPasses > 2 ? kClamp1 : kNone;
        for (uint32_t i = 1; i < totalPasses; ++i)
        {
            const auto boundary = PC::PlanBoundary(i, chain.Input(), prevPassInput, kClamp0, clamp1);
            assert(boundary.target != kNone);
            // Boundary dispatch never aliases SRV and UAV.
            assert(boundary.raw != boundary.target);
            assert(boundary.proxy != boundary.target);
            assert(boundary.raw != boundary.proxy);
            // Same-temporal-age rule (janblade #55/#61): boundary i clamps against pass (i-1)'s own input,
            // never mixing pass >= 2 back against pass 0's raw kModelInput.
            assert(boundary.proxy == prevPassInput);
            if (i >= 2)
                assert(boundary.proxy != kModelInput);

            // Model pass i reads boundary.target and writes chain.Output(): distinct surfaces.
            const SurfaceId modelIn = boundary.target;
            const SurfaceId modelOut = chain.Output();
            assert(modelIn != modelOut);

            prevPassInput = modelIn;
            chain.Success();
        }
        assert(chain.completed == totalPasses);
        assert(chain.answer == (totalPasses % 2 == 1 ? kOutput : kPassPing));
    }

    // 8. Downstream passReset arming.
    {
        std::array<bool, DlssNr::PassConfig::MaxPasses> resets {};
        resets.fill(false);
        PC::ArmSkippedPassResets(resets, 3);
        assert(!resets[0] && !resets[1] && !resets[2]);
        for (uint32_t i = 3; i < DlssNr::PassConfig::MaxPasses; ++i)
            assert(resets[i]);
    }

    return 0;
}

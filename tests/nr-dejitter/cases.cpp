#include <dlssnr/DlssNr_Dejitter.h>

#include <cassert>
#include <cmath>
#include <cstring>
#include <limits>

namespace
{
bool Near(float a, float b, float eps = 1e-5f) { return std::abs(a - b) <= eps; }

DlssNr::Dejitter::Inputs ValidInputs()
{
    DlssNr::Dejitter::Inputs in {};
    in.enabled = true;
    in.beforeUpscale = false;
    in.held = false;
    in.gameGuides = true;
    in.mvJittered = true;
    in.mvLowResolution = true;
    in.haveJitter = true;
    in.reset = false;
    in.renderWidth = 1920;
    in.renderHeight = 1080;
    in.motionWidth = 1920;
    in.motionHeight = 1080;
    in.outputWidth = 3840;
    in.outputHeight = 2160;
    in.jitterX = -0.25f;
    in.jitterY = 0.125f;
    in.mvScaleX = -0.5f;
    in.mvScaleY = 0.5f;
    in.route = DlssNr::Dejitter::Route::AfterUpscale;
    in.pictureEpoch = 1;
    in.renderSequence = 10;
    return in;
}
} // namespace

int main()
{
    namespace DJ = DlssNr::Dejitter;
    using DJ::Reference::ApplyMotionDejitter;
    using DJ::Reference::float2;

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const float inf = std::numeric_limits<float>::infinity();

    // 1. Default-off and eligibility guards.
    {
        DJ::State state {};
        auto off = ValidInputs();
        off.enabled = false;
        const auto decOff = DJ::Step(state, off);
        assert(!decOff.active && !decOff.dispatch && !decOff.pulseReset);

        auto pre = ValidInputs();
        pre.beforeUpscale = true;
        assert(!DJ::Eligible(pre));

        auto noGuides = ValidInputs();
        noGuides.gameGuides = false;
        assert(!DJ::Eligible(noGuides));

        auto unjittered = ValidInputs();
        unjittered.mvJittered = false;
        assert(!DJ::Eligible(unjittered));

        auto hiRes = ValidInputs();
        hiRes.mvLowResolution = false;
        assert(!DJ::Eligible(hiRes));

        auto displayExtentX = ValidInputs();
        displayExtentX.motionWidth = 3840;
        assert(!DJ::Eligible(displayExtentX));

        auto displayExtentY = ValidInputs();
        displayExtentY.motionHeight = 2160;
        assert(!DJ::Eligible(displayExtentY));

        auto noJitter = ValidInputs();
        noJitter.haveJitter = false;
        assert(!DJ::Eligible(noJitter));

        auto nanJitter = ValidInputs();
        nanJitter.jitterX = nan;
        assert(!DJ::Eligible(nanJitter));

        auto tinyScale = ValidInputs();
        tinyScale.mvScaleX = 1e-7f;
        assert(!DJ::Eligible(tinyScale));

        auto infScale = ValidInputs();
        infScale.mvScaleY = inf;
        assert(!DJ::Eligible(infScale));

        auto zeroSeq = ValidInputs();
        zeroSeq.renderSequence = 0;
        assert(!DJ::Eligible(zeroSeq));
    }

    // 2. Priming, sign cancellation on static geometry, and zero-delta skip.
    {
        DJ::State state {};
        auto f1 = ValidInputs();
        const auto d1 = DJ::Step(state, f1);
        assert(d1.active && !d1.dispatch && d1.pulseReset);

        auto f2 = f1;
        f2.renderSequence = 11;
        f2.jitterX = 0.25f;
        f2.jitterY = -0.375f;
        const auto d2 = DJ::Step(state, f2);
        assert(d2.active && d2.dispatch && !d2.pulseReset);
        // delta = (jitterNow - jitterPrev) / mvScale = ((0.25 - -0.25) / -0.5, (-0.375 - 0.125) / 0.5) = (-1.0, -1.0).
        assert(Near(d2.deltaMvX, -1.0f));
        assert(Near(d2.deltaMvY, -1.0f));

        // A static pixel whose raw jittered MV equals -(jitterNow - jitterPrev) / mvScale cancels to (0, 0).
        const float2 staticRawMv(-(f2.jitterX - f1.jitterX) / f2.mvScaleX, -(f2.jitterY - f1.jitterY) / f2.mvScaleY);
        const float2 cancelled = ApplyMotionDejitter(staticRawMv, float2(d2.deltaMvX, d2.deltaMvY));
        assert(Near(cancelled.x, 0.0f) && Near(cancelled.y, 0.0f));

        // Identical jitter on consecutive frame needs no GPU dispatch and no reset.
        auto f3 = f2;
        f3.renderSequence = 12;
        const auto d3 = DJ::Step(state, f3);
        assert(d3.active && !d3.dispatch && !d3.pulseReset);
        assert(d3.deltaMvX == 0.0f && d3.deltaMvY == 0.0f);

        // Turning the option off pulses reset once so NR drops de-jittered recurrent history cleanly.
        auto f4 = f3;
        f4.enabled = false;
        f4.renderSequence = 13;
        const auto d4 = DJ::Step(state, f4);
        assert(!d4.active && !d4.dispatch && d4.pulseReset);
        f4.renderSequence = 14;
        const auto d5 = DJ::Step(state, f4);
        assert(!d5.active && !d5.dispatch && !d5.pulseReset);
    }

    // 3. Sequence gap, reset flag, held frame, and contract changes re-prime history with pulseReset.
    {
        DJ::State state {};
        auto in = ValidInputs();
        assert(DJ::Step(state, in).pulseReset);
        in.renderSequence = 11;
        in.jitterX = 0.1f;
        assert(DJ::Step(state, in).dispatch);

        // Skipped render frame (11 -> 13): re-prime, do not subtract stale jitter across the gap.
        in.renderSequence = 13;
        in.jitterX = -0.2f;
        const auto gap = DJ::Step(state, in);
        assert(gap.active && !gap.dispatch && gap.pulseReset);

        // Next consecutive frame (14) dispatches normally against frame 13's jitter.
        in.renderSequence = 14;
        in.jitterX = 0.3f;
        const auto resumed = DJ::Step(state, in);
        assert(resumed.active && resumed.dispatch && !resumed.pulseReset);
        assert(Near(resumed.deltaMvX, (0.3f - (-0.2f)) / in.mvScaleX));

        // Real reset flag on consecutive sequence re-primes.
        in.renderSequence = 15;
        in.reset = true;
        in.jitterX = -0.1f;
        const auto onReset = DJ::Step(state, in);
        assert(onReset.active && !onReset.dispatch && onReset.pulseReset);
        in.reset = false;

        // Render-size change re-primes.
        in.renderSequence = 16;
        in.jitterX = 0.2f;
        assert(DJ::Step(state, in).dispatch);
        in.renderSequence = 17;
        in.renderWidth = 1600;
        in.motionWidth = 1600;
        in.jitterX = -0.2f;
        const auto onResize = DJ::Step(state, in);
        assert(onResize.active && !onResize.dispatch && onResize.pulseReset);

        // Route change (AfterUpscale -> Present) re-primes.
        in.renderSequence = 18;
        in.jitterX = 0.15f;
        assert(DJ::Step(state, in).dispatch);
        in.renderSequence = 19;
        in.route = DJ::Route::Present;
        in.jitterX = -0.15f;
        const auto onRoute = DJ::Step(state, in);
        assert(onRoute.active && !onRoute.dispatch && onRoute.pulseReset);

        // Held frame clears state without pulsing reset while held, then re-primes when released.
        in.renderSequence = 20;
        in.jitterX = 0.25f;
        assert(DJ::Step(state, in).dispatch);
        in.held = true;
        const auto whileHeld = DJ::Step(state, in);
        assert(!whileHeld.active && !whileHeld.dispatch && !whileHeld.pulseReset);
        in.held = false;
        in.renderSequence = 21;
        const auto afterHold = DJ::Step(state, in);
        assert(afterHold.active && !afterHold.dispatch && afterHold.pulseReset);
    }

    // 4. Shared HLSL/C++ per-pixel rule guards non-finite inputs and overflow.
    {
        const float2 mv(3.25f, -7.5f);
        const float2 delta(-0.5f, 1.25f);
        const float2 out = ApplyMotionDejitter(mv, delta);
        assert(Near(out.x, 2.75f) && Near(out.y, -6.25f));

        const float2 badMv(nan, 4.0f);
        const float2 outBadMv = ApplyMotionDejitter(badMv, delta);
        assert(std::isnan(outBadMv.x) && outBadMv.y == 4.0f);

        const float2 badDelta(1.0f, inf);
        const float2 outBadDelta = ApplyMotionDejitter(mv, badDelta);
        assert(outBadDelta.x == mv.x && outBadDelta.y == mv.y);

        const float maxFloat = std::numeric_limits<float>::max();
        const float2 huge(maxFloat, 1.0f);
        const float2 overflowOut = ApplyMotionDejitter(huge, float2(maxFloat, 1.0f));
        assert(overflowOut.x == huge.x && overflowOut.y == huge.y);
    }

    return 0;
}

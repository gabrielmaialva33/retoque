#include <dlssnr/DlssNr_SlPresentPolicy.h>
#include <dlssnr/DlssNr_PresentGuideIdentity.h>
#include <cassert>
#include <cstdio>
using namespace DlssNr;
int main()
{
    SlPresent::Recording recording;
    assert(!recording.CanCopy());
    recording.Reset(true);
    assert(recording.CanCopy());
    recording.Begin(true);
    assert(!recording.CanCopy());
    recording.End();
    assert(recording.CanCopy());
    recording.Begin(false);
    recording.End();
    assert(!recording.CanCopy());
    recording.Reset(true);
    recording.Close();
    assert(!recording.CanCopy());
    recording.Reset(false);
    assert(!recording.CanCopy());
    recording.Reset(true);
    recording.End();
    assert(!recording.CanCopy());
    SlPresent::Marker marker;
    uint32_t token = 55;
    assert(!marker.Take(token));
    marker.Start(0);
    assert(marker.Take(token) && token == 0);
    assert(!marker.Take(token));
    marker.Start(1);
    marker.Start(2);
    assert(!marker.Take(token));
    marker.Start(2);
    marker.End();
    assert(!marker.Take(token));
    SlPresent::Frames frames;
    auto revision = frames.Constants(1, 0, true);
    assert(revision && frames.Tags(1, 0, true) == revision);
    assert(!frames.Capture(1, revision, 0));
    assert(frames.Capture(1, revision, 10));
    assert(!frames.Present(1, 11, false));
    assert(frames.Present(1, 11, true));
    assert(!frames.Present(1, 11, true));
    assert(!frames.Capture(1, revision, 10));
    assert(frames.Matches(1, revision));
    frames.Tags(1, 0, true);
    assert(!frames.Matches(1, revision));
    {
        SlPresent::Frames incomplete;
        const auto r = incomplete.Constants(10, 2, true);
        assert(!incomplete.Tags(10, 2, false));
        assert(!incomplete.Capture(10, r, 5));
    }
    {
        SlPresent::Frames noTags;
        const auto r = noTags.Constants(10, 2, true);
        assert(!noTags.Capture(10, r, 5));
        assert(noTags.Tags(10, 2, true));
        assert(noTags.Capture(10, r, 5));
        noTags.Tags(10, UINT32_MAX, true);
        assert(!noTags.Matches(10, r));
    }
    for (int scenario = 0; scenario < 10; ++scenario)
    {
        SlPresent::Frames f;
        auto r = f.Constants(10, 2, true);
        if (scenario == 0)
            f.Constants(10, 2, true);
        if (scenario == 1)
            f.Tags(10, 2, false);
        if (scenario == 2)
            f.Constants(11, 3, true);
        if (scenario == 3)
            f.Tokenless();
        if (scenario == 4)
            f.Constants(10 + SlPresent::Frames::Capacity, 2, true);
        if (scenario == 5)
            f.Invalidate(10);
        if (scenario == 6)
            r++;
        if (scenario == 7)
        {
            f = {};
            f.Tags(10, 2, true);
            f.Constants(10, 2, true);
        }
        f.Tags(10, 2, true);
        const bool captured = f.Capture(10, r, 5);
        assert(!captured || scenario >= 8);
        assert(!f.Present(10, scenario == 8 ? 7 : 6, scenario != 9) || scenario < 8);
    }
    assert(!SlPresent::HudlessAllowed(false, 0, 100));
    assert(!SlPresent::HudlessAllowed(true, 31, 100));
    assert(SlPresent::HudlessAllowed(true, 30, 100));
    assert(!SlPresent::HudlessAllowed(true, 0, 0));

    using namespace PresentGuideIdentity;
    PresentGuides::Ring ring;
    Tracker observed, legacy;
    int device, chain, write, read;
    Present p;
    p.device = &device;
    p.swapchain = &chain;
    p.width = 1920;
    p.height = 1080;
    p.nowMs = 100;
    // Real boundaries with NO capture, including warmup/refused NR work, still advance.
    for (int i = 0; i < 12; ++i)
        observed.ObservePresent(p);
    assert(observed.Epoch() == 12 && legacy.Epoch() == 0);
    Stamp stamp;
    stamp.device = &device;
    stamp.outputWidth = 1920;
    stamp.outputHeight = 1080;
    stamp.renderWidth = 1280;
    stamp.renderHeight = 720;
    observed.StampCapture(0, stamp, 100);
    legacy.StampCapture(0, stamp, 100);
    ring.CommitRecord(0, 1280, 720, false, 100, &write, &read);
    observed.ObservePresent(p);
    Present old = p;
    assert(observed.AtObservedPresent(0, ring, p).reason == Reason::Accepted);
    assert(legacy.AtPresent(0, ring, old).reason == Reason::Accepted);
    assert(observed.Epoch() == 13); // decision must not tick again
    const auto previous = p;
    observed.ObservePresent(p); // optional work refused on this real Present
    assert(observed.AtObservedPresent(0, ring, previous).reason == Reason::Lag);
    observed.ObservePresent(p);
    assert(observed.AtObservedPresent(0, ring, p).reason == Reason::Lag);
    p.enforceLag = false;
    assert(observed.AtObservedPresent(0, ring, p).reason == Reason::Accepted);
    p.lastConsumed = ring.GetSlot(0).sequence;
    assert(observed.AtObservedPresent(0, ring, p).reason == Reason::Consumed);
    // Mixed producers do not make an ordinary upscaler capture ambiguous.
    Tracker mixed;
    mixed.StampCapture(0, stamp, 100);
    stamp.streamline = true;
    mixed.StampCapture(1, stamp, 100);
    ring.CommitRecord(1, 1280, 720, false, 100, &write, &read);
    p.lastConsumed = 0;
    p.enforceLag = true;
    assert(mixed.AtPresent(0, ring, p).reason == Reason::Accepted);
    assert(mixed.AtObservedPresent(1, ring, p).reason == Reason::Ambiguous); // bridge/default cannot read SL
    p.allowStreamline = true;
    assert(mixed.AtObservedPresent(1, ring, p).reason == Reason::Accepted);
    puts("PASS: identity, no-capture clock/lag/consumption, recording, markers, pair refusal and HUD-less guard");
}

// The multi-view detector (retoque/dlssnr/DlssNr_MultiView.h) against synthetic sub-rectangles of one colour
// allocation. No D3D12, no NGX, no game: the layouts are the shapes Assetto Corsa EVO's multi-monitor logs and
// janblade's nr_views_smoke.cpp describe, written out as numbers.
#include "retoque/dlssnr/DlssNr_MultiView.h"

#include <cassert>
#include <cstdio>
#include <vector>

using namespace DlssNr::MultiView;

namespace
{
constexpr uint64_t kAllocW = 5760;
constexpr uint32_t kAllocH = 1080;
constexpr uint32_t kFormat = 10; // any one format; only equality matters

Seen View(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint64_t allocW = kAllocW, uint32_t allocH = kAllocH,
          uint32_t format = kFormat)
{
    Seen s;
    s.allocWidth = allocW;
    s.allocHeight = allocH;
    s.allocFormat = format;
    s.rect = { x, y, w, h };
    return s;
}

// Three screens side by side at render size (1280x720 each in a 3840x720 region), plus the combined picture.
const std::vector<Seen> kTriple = { View(0, 0, 1280, 720), View(1280, 0, 1280, 720), View(2560, 0, 1280, 720),
                                    View(0, 0, 3840, 720) };

// Every seam notes an evaluate: the pre-upscale scope and the after-upscale pass both see it.
Verdict NoteTwice(Detector& d, uint64_t epoch, const Seen& s)
{
    const Result first = d.Note(epoch, s);
    const Result second = d.Note(epoch, s);
    assert(first.viewsInEpoch == second.viewsInEpoch && "one view noted by two seams is one view");
    assert(first.verdict == second.verdict);
    return second.verdict;
}
} // namespace

int main()
{
    unsigned checks = 0;

    // One view per frame, with dynamic resolution moving every frame and both seams noting it: never a layout.
    {
        Detector d;
        for (uint64_t epoch = 1; epoch <= 1000; ++epoch)
        {
            const uint32_t w = 1000 + static_cast<uint32_t>(epoch % 280), h = 600 + static_cast<uint32_t>(epoch % 120);
            assert(NoteTwice(d, epoch, View(0, 0, w, h)) == Verdict::Run);
        }
        assert(!d.Active() && d.StandDowns() == 0);
        ++checks;
    }

    // The triple-screen layout: the first frame runs (nothing is known yet), the second frame's second view finds it,
    // and from then on every evaluate stands down, counted.
    {
        Detector d;
        for (const Seen& s : kTriple)
            assert(NoteTwice(d, 1, s) == Verdict::Run);
        assert(!d.Active());

        Result r = d.Note(2, kTriple[0]);
        assert(r.verdict == Verdict::Run && !r.latchedNow && r.viewsInEpoch == 1);
        r = d.Note(2, kTriple[1]);
        assert(r.verdict == Verdict::StandDown && r.latchedNow && r.viewsInEpoch == 2 && r.standDowns == 1);
        r = d.Note(2, kTriple[1]);
        assert(r.verdict == Verdict::StandDown && !r.latchedNow && r.standDowns == 2);
        r = d.Note(2, kTriple[2]);
        assert(r.verdict == Verdict::StandDown && r.viewsInEpoch == 3 && r.standDowns == 3);

        uint64_t expected = 3;
        for (uint64_t epoch = 3; epoch < 103; ++epoch)
            for (const Seen& s : kTriple)
            {
                r = d.Note(epoch, s);
                assert(r.verdict == Verdict::StandDown && !r.latchedNow && r.standDowns == ++expected);
            }
        assert(d.Active() && d.StandDowns() == expected);
        d.Reset();
        assert(!d.Active() && d.StandDowns() == 0 && d.Note(103, kTriple[0]).verdict == Verdict::Run);
        ++checks;
    }

    // Two views of one size at different offsets (no extent change for the settling gates to notice): still a layout.
    {
        Detector d;
        const Seen left = View(0, 0, 1920, 1080), right = View(1920, 0, 1920, 1080);
        for (uint64_t epoch = 1; epoch <= 3; ++epoch)
        {
            d.Note(epoch, left);
            d.Note(epoch, right);
        }
        assert(d.Active());
        assert(d.Note(4, left).verdict == Verdict::StandDown);
        ++checks;
    }

    // Two pictures in two different allocations within a frame (a mirror, a scope, a portrait): not this layout.
    {
        Detector d;
        for (uint64_t epoch = 1; epoch <= 50; ++epoch)
        {
            assert(d.Note(epoch, View(0, 0, 1920, 1080, 1920, 1080)).verdict == Verdict::Run);
            assert(d.Note(epoch, View(0, 0, 512, 256, 512, 256)).verdict == Verdict::Run);
            assert(d.Note(epoch, View(0, 0, 1920, 1080, 1920, 1080, kFormat + 1)).verdict == Verdict::Run);
        }
        assert(!d.Active() && d.StandDowns() == 0);
        ++checks;
    }

    // A present counter that does not advance puts everything in one "frame": a resolution change there is not a
    // layout, however many times it happens.
    {
        Detector d;
        for (uint32_t i = 0; i < 100; ++i)
            assert(d.Note(7, View(0, 0, 1000 + i, 600)).verdict == Verdict::Run);
        assert(!d.Active());
        ++checks;
    }

    // Before any observed present (epoch 0) nothing is judged or remembered.
    {
        Detector d;
        for (uint32_t i = 0; i < 10; ++i)
            for (const Seen& s : kTriple)
                assert(d.Note(0, s).verdict == Verdict::Run);
        assert(!d.Active() && d.StandDowns() == 0);
        assert(d.Note(1, kTriple[0]).viewsInEpoch == 1);
        // So the first observed frame is the first that counts toward the layout, and it alone is not enough.
        for (const Seen& s : kTriple)
            assert(d.Note(1, s).verdict == Verdict::Run);
        ++checks;
    }

    // A multi-view frame between single-view frames (a replay camera cut) is not enough.
    {
        Detector d;
        for (uint64_t epoch = 1; epoch <= 60; ++epoch)
        {
            d.Note(epoch, kTriple[3]);
            if (epoch % 2 == 0)
                d.Note(epoch, kTriple[0]);
        }
        assert(!d.Active() && d.StandDowns() == 0);
        ++checks;
    }

    // The layout goes away: it is kept through kQuietEpochs - 1 single-view frames (a menu, a loading screen) and
    // forgotten after the kQuietEpochs-th, said once.
    {
        Detector d;
        for (uint64_t epoch = 1; epoch <= 3; ++epoch)
            for (const Seen& s : kTriple)
                d.Note(epoch, s);
        assert(d.Active());
        const Seen single = View(0, 0, 1920, 1080, 1920, 1080);
        uint64_t epoch = 4;
        unsigned unlatched = 0;
        for (uint32_t i = 0; i < kQuietEpochs; ++i, ++epoch)
        {
            const Result r = d.Note(epoch, single);
            assert(r.verdict == Verdict::StandDown && !r.unlatchedNow);
        }
        // The epoch that ends the kQuietEpochs-th single-view frame releases it.
        const Result r = d.Note(epoch, single);
        unlatched += r.unlatchedNow;
        assert(r.verdict == Verdict::Run && unlatched == 1 && !d.Active());
        for (uint32_t i = 0; i < 10; ++i)
            assert(d.Note(++epoch, single).verdict == Verdict::Run);
        ++checks;
    }

    // Quiet frames interrupted by the layout start counting again.
    {
        Detector d;
        for (uint64_t epoch = 1; epoch <= 3; ++epoch)
            for (const Seen& s : kTriple)
                d.Note(epoch, s);
        uint64_t epoch = 4;
        for (uint32_t i = 0; i < kQuietEpochs - 1; ++i)
            d.Note(epoch++, kTriple[0]);
        d.Note(epoch, kTriple[0]);
        d.Note(epoch++, kTriple[1]); // back again
        for (uint32_t i = 0; i < kQuietEpochs - 1; ++i)
            assert(d.Note(epoch++, kTriple[0]).verdict == Verdict::StandDown);
        assert(d.Active());
        ++checks;
    }

    // More views than are remembered: still one layout, no overflow (ASan watches the arrays).
    {
        Detector d;
        for (uint64_t epoch = 1; epoch <= 3; ++epoch)
            for (uint32_t i = 0; i < 3 * kMaxViews; ++i)
                d.Note(epoch, View(i * 100, 0, 100, 100));
        assert(d.Active());
        const Result r = d.Note(4, View(0, 0, 100, 100));
        assert(r.verdict == Verdict::StandDown && r.viewsInEpoch == 1);
        ++checks;
    }

    assert(checks == 10 && "ZERO COVERAGE");
    std::printf("PASS: %u multi-view detector cases\n", checks);
}

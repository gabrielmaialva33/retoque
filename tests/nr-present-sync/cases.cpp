#include <dlssnr/DlssNr_PresentSync.h>

#include <cassert>
#include <cstdio>
#include <cstring>

namespace
{
using DlssNr::DecidePresentSync;
using DlssNr::PresentSyncInputs;
using DlssNr::PresentSyncWanted;

// FSR-FG at OptiScaler's own frame generation hook, as PCSX2 runs it with synthesized frame generation: FFX's
// swapchain or the fork's presenter (LSFG included), the pass on that swapchain's game queue, no DLSS-G anywhere.
PresentSyncInputs FsrFgHook()
{
    PresentSyncInputs in;
    in.fgHook = in.fgOutput = in.fsrFgOutput = in.onFgGameQueue = true;
    return in;
}
} // namespace

int main()
{
    // Auto, nothing that paces the queue: the flip is on the write-back's own queue, so no CPU wait.
    assert(!PresentSyncWanted(std::nullopt, PresentSyncInputs {}));

    // Auto with any sign of frame generation keeps the wait, each on its own.
    {
        PresentSyncInputs in;
        in.fgHook = true;
        assert(PresentSyncWanted(std::nullopt, in));
    }
    {
        PresentSyncInputs in;
        in.fgOutput = true;
        assert(PresentSyncWanted(std::nullopt, in));
    }
    {
        PresentSyncInputs in;
        in.dlssgSeen = true;
        assert(PresentSyncWanted(std::nullopt, in));
    }
    {
        // A game that ships DLSS-G keeps the wait even where OptiScaler never saw its options.
        PresentSyncInputs in;
        in.dlssgLoaded = true;
        assert(PresentSyncWanted(std::nullopt, in));
    }

    // At the game's own Present, ahead of its DLSS-G, auto does not wait even with every sign of frame generation:
    // the write-back is on the game's queue ahead of the fence DLSS-G waits on (nr-before-dlssg.md, step 0b).
    {
        PresentSyncInputs in;
        in.dlssgSeen = in.dlssgLoaded = in.boundary = true;
        assert(!PresentSyncWanted(std::nullopt, in));
        // An explicit true still waits there.
        assert(PresentSyncWanted(true, in));
    }

    // FSR-FG's own swapchain at the frame generation hook orders the game queue's work ahead of its own on the GPU,
    // so auto does not hold the present thread for it (nr-present-hook.md, 2026-11-18).
    assert(!PresentSyncWanted(std::nullopt, FsrFgHook()));

    // Every one of its conditions is needed. Without any one of them the swapchain is not known to order the pass,
    // and auto waits as it did.
    {
        // XeFG, DLSS-G as OptiScaler's output, the flip meter, reprojection: a hook and an output, not FSR-FG's.
        PresentSyncInputs in = FsrFgHook();
        in.fsrFgOutput = false;
        assert(PresentSyncWanted(std::nullopt, in));
    }
    {
        // A custom FSR-FG provider brings a swapchain of its own.
        PresentSyncInputs in = FsrFgHook();
        in.fsrFgCustomProvider = true;
        assert(PresentSyncWanted(std::nullopt, in));
    }
    {
        // Submitted on a queue that is not the swapchain's game queue: its fence says nothing about this list.
        PresentSyncInputs in = FsrFgHook();
        in.onFgGameQueue = false;
        assert(PresentSyncWanted(std::nullopt, in));
    }
    {
        // The wrapped entry, below FSR-FG's swapchain or any other interposer: not the hook, so not ordered by it.
        PresentSyncInputs in = FsrFgHook();
        in.fgHook = false;
        assert(PresentSyncWanted(std::nullopt, in));
    }

    // Multi-pass recordings admitted one at a time (chain-in-flight.md): the next frame's would be refused while
    // this one is still on the GPU, so there the wait stays, for the next frame and not for the flip.
    {
        PresentSyncInputs in = FsrFgHook();
        in.serialRecordings = true;
        assert(PresentSyncWanted(std::nullopt, in));
    }
    // It is asked only where the exception would otherwise drop the wait: no other answer moves with it.
    {
        PresentSyncInputs in;
        in.serialRecordings = true;
        assert(!PresentSyncWanted(std::nullopt, in)); // no frame generation: no wait, as since 2026-10-13
        in.boundary = in.dlssgSeen = true;
        assert(!PresentSyncWanted(std::nullopt, in)); // the game's own Present
        assert(!PresentSyncWanted(false, in));
    }

    // DLSS-G anywhere keeps the wait under FSR-FG too: seen through Streamline or NGX, or only loaded.
    {
        PresentSyncInputs in = FsrFgHook();
        in.dlssgSeen = true;
        assert(PresentSyncWanted(std::nullopt, in));
    }
    {
        PresentSyncInputs in = FsrFgHook();
        in.dlssgLoaded = true;
        assert(PresentSyncWanted(std::nullopt, in));
    }

    // An explicit setting wins either way.
    {
        PresentSyncInputs fg;
        fg.fgHook = fg.fgOutput = fg.dlssgSeen = fg.dlssgLoaded = true;
        assert(!PresentSyncWanted(false, fg));
        assert(PresentSyncWanted(true, PresentSyncInputs {}));
        // Under FSR-FG as well: true brings the wait back where auto dropped it, false drops it where auto keeps.
        assert(PresentSyncWanted(true, FsrFgHook()));
        PresentSyncInputs dlssg = FsrFgHook();
        dlssg.dlssgLoaded = true;
        assert(!PresentSyncWanted(false, dlssg));
    }

    // The reason goes with the answer, one literal per branch, so the log can say why a frame waited or did not.
    {
        const PresentSyncInputs none;
        PresentSyncInputs xefg;
        xefg.fgHook = xefg.fgOutput = true;
        PresentSyncInputs dlssg = FsrFgHook();
        dlssg.dlssgSeen = true;
        PresentSyncInputs boundary;
        boundary.boundary = boundary.dlssgSeen = true;
        PresentSyncInputs serial = FsrFgHook();
        serial.serialRecordings = true;

        const struct
        {
            std::optional<bool> configured;
            PresentSyncInputs in;
            bool wait;
            const char* why;
        } rows[] = {
            { true, FsrFgHook(), true, "PresentSync=true" },
            { false, dlssg, false, "PresentSync=false" },
            { std::nullopt, boundary, false, "the game's own Present" },
            { std::nullopt, FsrFgHook(), false, "FSR-FG's swapchain orders" },
            { std::nullopt, serial, true, "admitted one at a time" },
            { std::nullopt, dlssg, true, "DLSS-G" },
            { std::nullopt, xefg, true, "not known to order" },
            { std::nullopt, none, false, "no frame generation" },
        };
        for (const auto& row : rows)
        {
            const auto decision = DecidePresentSync(row.configured, row.in);
            assert(decision.wait == row.wait && decision.wait == PresentSyncWanted(row.configured, row.in));
            assert(decision.why != nullptr && std::strstr(decision.why, row.why) != nullptr);
            // The same inputs give the same literal: the log line is written when the pointer changes.
            assert(decision.why == DecidePresentSync(row.configured, row.in).why);
        }
    }

    std::printf("PASS: present sync -- auto waits only with frame generation (hook, output, DLSS-G seen or loaded), "
                "never at the game's own Present, not under FSR-FG's own swapchain at the hook (and still with "
                "DLSS-G, another output, a custom provider, another queue, the wrapped entry or multi-pass "
                "recordings admitted one at a time), an explicit setting wins\n");
    return 0;
}

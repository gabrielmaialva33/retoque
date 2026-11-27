#include <dlssnr/DlssNr_PresentReset.h>
#include <cassert>
#include <iostream>
#include <thread>
#include <vector>

struct Frame
{
    bool Reset = false;
    bool ResetIsPolicy = true;
    bool ResetIsSceneCut = true;
};

int main()
{
    using DlssNr::PresentReset::Pending;
    Pending pending;
    pending.Observe(false, true);
    pending.Observe(true, false);
    assert(pending.Snapshot() == 0);
    pending.Observe(true, true); // camera cut on a capture the ring refuses
    const auto skipped = pending.Snapshot();
    assert(skipped != 0);
    pending.Observe(true, false); // later capture succeeds, with no cut flag
    assert(pending.Snapshot() == skipped);
    Frame frame;
    Pending::Apply(skipped, frame);
    assert(frame.Reset && !frame.ResetIsPolicy && !frame.ResetIsSceneCut);
    assert(pending.Snapshot() == skipped); // looking/picking/recording does not consume
    pending.Confirm(skipped, false, true); // model declined
    pending.Confirm(skipped, true, false); // list abandoned / submit failed
    assert(pending.Snapshot() == skipped);
    pending.Observe(true, true); // another cut races with confirmation of this recording
    const auto newer = pending.Snapshot();
    pending.Confirm(skipped, true, true);
    assert(pending.Snapshot() == newer);
    pending.Confirm(newer, true, true);
    pending.Confirm(skipped, true, true); // late completion cannot resurrect old resets
    assert(pending.Snapshot() == 0);
    Frame policy;
    Pending::Apply(0, policy);
    assert(!policy.Reset && policy.ResetIsPolicy && policy.ResetIsSceneCut);
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t)
        threads.emplace_back(
            [&]
            {
                for (int i = 0; i < 1000; ++i)
                    pending.Observe(true, true);
            });
    for (auto& t : threads)
        t.join();
    const auto all = pending.Snapshot();
    assert(all == newer + 4000);
    pending.Confirm(all, true, true);
    assert(pending.Snapshot() == 0);
    std::cout << "present reset: disabled/false, skipped capture, skipped model, abandoned recording, race, cause, "
                 "concurrent producers passed\n";
}

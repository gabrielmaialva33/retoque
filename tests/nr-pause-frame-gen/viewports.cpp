// The production viewport owner of the PauseWithoutFrameGen signal (retoque/hooks/Streamline_DlssgRequest.h), under
// ASan/UBSan: the first viewport whose call Streamline accepted owns it, a second one stands the pause down for good.
#include <hooks/Streamline_DlssgRequest.h>

#include <cstdio>
#include <thread>
#include <vector>

static int failures = 0;

static void Check(bool ok, const char* what)
{
    if (!ok)
    {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

int main()
{
    namespace R = StreamlineDlssgRequest;

    {
        std::atomic<uint64_t> word { 0 };
        Check(!R::MultipleViewports(word), "nothing seen: one viewport at most");
        Check(R::Accept(word, 0), "viewport 0 is a viewport: the first owns the signal");
        Check(R::Accept(word, 0), "its repeats keep owning it");
        Check(!R::MultipleViewports(word), "one viewport");
        Check(!R::Accept(word, 1), "a second viewport does not set the signal");
        Check(R::MultipleViewports(word), "a second viewport is remembered");
        Check(!R::Accept(word, 0), "and the first one no longer sets it either");
        Check(R::MultipleViewports(word), "the second viewport stays remembered");
    }

    {
        // Handles at the ends of the range are not confused with the flags
        std::atomic<uint64_t> word { 0 };
        Check(R::Accept(word, 0xffffffffu), "the highest handle owns the signal");
        Check(R::Accept(word, 0xffffffffu), "and keeps it");
        Check(!R::MultipleViewports(word), "one viewport, highest handle");
        Check(!R::Accept(word, 0xfffffffeu), "a neighbouring handle is another viewport");
        Check(R::MultipleViewports(word), "two viewports, highest handles");
    }

    {
        // Nine threads drive slDLSSGSetOptions in some games: one viewport from all of them stays one viewport
        std::atomic<uint64_t> word { 0 };
        std::atomic<int> refused { 0 };
        std::vector<std::thread> threads;
        for (int t = 0; t < 9; ++t)
            threads.emplace_back(
                [&]
                {
                    for (int i = 0; i < 2000; ++i)
                        refused += !R::Accept(word, 7);
                });
        for (auto& thread : threads)
            thread.join();
        Check(refused == 0 && !R::MultipleViewports(word), "one viewport from nine threads");
    }

    {
        // Two viewports racing: exactly one wins the first call, and both end up refused
        std::atomic<uint64_t> word { 0 };
        std::thread a(
            [&]
            {
                for (int i = 0; i < 2000; ++i)
                    R::Accept(word, 1);
            });
        std::thread b(
            [&]
            {
                for (int i = 0; i < 2000; ++i)
                    R::Accept(word, 2);
            });
        a.join();
        b.join();
        Check(R::MultipleViewports(word), "two racing viewports are seen as two");
        Check(!R::Accept(word, 1) && !R::Accept(word, 2), "neither sets the signal afterwards");
    }

    std::printf(failures == 0 ? "PASS: PauseWithoutFrameGen viewport owner\n" : "FAIL: viewport owner (%d)\n",
                failures);
    return failures == 0 ? 0 : 1;
}

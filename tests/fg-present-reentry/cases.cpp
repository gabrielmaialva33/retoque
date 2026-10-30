
// ---- cases ----

static int failures = 0;
static void check(bool passed, const std::string& message)
{
    if (!passed)
    {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
    }
}

// A one-shot gate between two threads
struct Gate
{
    std::mutex m;
    std::condition_variable cv;
    bool open = false;
    void Open()
    {
        {
            std::lock_guard lock(m);
            open = true;
        }
        cv.notify_all();
    }
    bool Wait(std::chrono::milliseconds limit)
    {
        std::unique_lock lock(m);
        return cv.wait_for(lock, limit, [&] { return open; });
    }
};

enum class Outcome
{
    Returned,
    Deadlock, // libstdc++ reports a non-recursive re-lock on the owning thread as EDEADLK; Windows' SRW lock hangs
    Hung
};

// Runs work on its own thread with a deadline. A thread that never returns is abandoned; the process ends with _Exit.
static Outcome Run(const std::function<void()>& work, std::chrono::milliseconds limit = std::chrono::seconds(5))
{
    auto done = std::make_shared<Gate>();
    auto outcome = std::make_shared<std::atomic<Outcome>>(Outcome::Hung);
    std::thread(
        [=]
        {
            try
            {
                work();
                outcome->store(Outcome::Returned);
            }
            catch (const std::system_error&)
            {
                outcome->store(Outcome::Deadlock);
            }
            done->Open();
        })
        .detach();
    if (!done->Wait(limit))
    {
        // The thread is blocked on a mutex that lives in this frame: nothing after this can run safely
        std::fprintf(stderr, "FAIL: a call hung for %lld ms (deadlock)\n", (long long) limit.count());
        std::fflush(stderr);
        std::_Exit(1);
    }
    return outcome->load();
}

static const char* Name(Outcome outcome)
{
    return outcome == Outcome::Returned ? "returned" : outcome == Outcome::Deadlock ? "self-deadlock" : "hung";
}

struct Reentry
{
    const char* name;
    std::function<HRESULT(WrappedIDXGISwapChain4&)> call;
};

static IUnknown* const presentQueue = reinterpret_cast<IUnknown*>(0x1234);
static const Reentry reentries[] = {
    { "SetFullscreenState", [](WrappedIDXGISwapChain4& sc) { return sc.SetFullscreenState(1, nullptr); } },
    { "ResizeBuffers",
      [](WrappedIDXGISwapChain4& sc) { return sc.ResizeBuffers(2, 1280, 720, DXGI_FORMAT_UNKNOWN, 0); } },
    { "ResizeBuffers1", [](WrappedIDXGISwapChain4& sc)
      { return sc.ResizeBuffers1(2, 1280, 720, DXGI_FORMAT_UNKNOWN, 0, nullptr, &presentQueue); } },
};

struct PresentEntry
{
    const char* name;
    uint32_t owner;
    std::function<HRESULT(WrappedIDXGISwapChain4&)> call;
};
static const PresentEntry presents[] = {
    { "Present", 4, [](WrappedIDXGISwapChain4& sc) { return sc.Present(1, 0); } },
    { "Present1", 5, [](WrappedIDXGISwapChain4& sc) { return sc.Present1(1, 0, nullptr); } },
};

static void Reset()
{
    auto& state = State::Instance();
    state.currentFG = nullptr;
    state.activeFgOutput = FGOutput::NoFG;
    state.activeFgNvngx = FGNvngxReplacement::None;
    Config::Instance()->FGUseMutexForSwapchain.value = true;
    insidePresent = [] {};
    fullscreenWork = [] { return S_OK; };
    resizeWork = [] { return S_OK; };
}

// The game resizes from its window procedure while its own Present/Present1 is still inside DXGI (alt-tab, a mode
// change): same thread, the swapchain's local mutex already held as owner 4 or 5.
static void SameThreadReentry()
{
    for (const auto nvngx : { FGNvngxReplacement::None, FGNvngxReplacement::DLSSGMod })
    {
        for (const auto& present : presents)
        {
            for (const auto& reentry : reentries)
            {
                Reset();
                State::Instance().activeFgNvngx = nvngx;
                WrappedIDXGISwapChain4 sc;
                const std::string label = std::string(present.name) + " -> " + reentry.name +
                                          (nvngx == FGNvngxReplacement::None ? "" : " (nvngx DLSS-G)");
                bool reentered = false;
                uint32_t ownerInside = 0;
                insidePresent = [&]
                {
                    ownerInside = sc._localMutex.getOwner();
                    check(reentry.call(sc) == S_OK, label + ": the re-entrant call reached the device");
                    reentered = true;
                };
                const auto outcome = Run([&] { present.call(sc); });
                check(outcome == Outcome::Returned,
                      label + ": re-entry on the presenting thread must not wait for itself, got " + Name(outcome));
                check(ownerInside == present.owner, label + ": present holds the local mutex under its owner id");
                check(reentered, label + ": the window-procedure call completed inside present");
                check(sc._localMutex.getOwner() == 0, label + ": present released the local mutex");
            }
        }
    }
}

// OptiScaler's own FG presents (FGPresent) with the FG mutex held as owner 2 on the same thread; the resize hooks then
// want it as owner 3. Waiting for oneself never ends: the call goes on and leaves owner 2 in place.
static void SameThreadFgMutex()
{
    for (const auto& reentry : reentries)
    {
        Reset();
        FG fg;
        auto& state = State::Instance();
        state.currentFG = &fg;
        state.activeFgOutput = FGOutput::FSRFG;
        WrappedIDXGISwapChain4 sc;
        const std::string label = std::string("Present1 + FG present -> ") + reentry.name;
        uint32_t ownerDuringWork = 0;
        fullscreenWork = resizeWork = [&]
        {
            ownerDuringWork = fg.Mutex.getOwner();
            return S_OK;
        };
        insidePresent = [&]
        {
            fg.Mutex.lock(2);
            reentry.call(sc);
            check(fg.Mutex.getOwner() == 2, label + ": the FG present keeps its own FG mutex");
            fg.Mutex.unlockThis(2);
        };
        const auto outcome = Run([&] { sc.Present1(1, 0, nullptr); });
        check(outcome == Outcome::Returned,
              label + ": the FG mutex this thread holds must not be waited for, got " + Name(outcome));
        check(ownerDuringWork == 2, label + ": the device work ran under the present's FG mutex");
        check(fg.Mutex.getOwner() == 0, label + ": nothing left locked");
    }
}

// Another thread is still protected: while a present holds the local mutex, the methods wait for it to finish.
// expectWait=false is upstream's rule, kept: Present (owner 4) under the nvngx DLSS-G replacement lets them through.
static void OtherThread(const PresentEntry& present, FGNvngxReplacement nvngx, bool expectWait)
{
    for (const auto& reentry : reentries)
    {
        Reset();
        State::Instance().activeFgNvngx = nvngx;
        WrappedIDXGISwapChain4 sc;
        const std::string label = std::string("other thread during ") + present.name + " -> " + reentry.name +
                                  (nvngx == FGNvngxReplacement::None ? "" : " (nvngx DLSS-G)");
        Gate entered, release;
        std::atomic<bool> inside { false };
        std::atomic<bool> workDuringPresent { false };
        insidePresent = [&]
        {
            inside = true;
            entered.Open();
            release.Wait(std::chrono::seconds(5));
            inside = false;
        };
        fullscreenWork = resizeWork = [&]
        {
            workDuringPresent = inside.load();
            return S_OK;
        };
        std::thread presenter([&] { present.call(sc); });
        check(entered.Wait(std::chrono::seconds(5)), label + ": present started");
        std::atomic<bool> finished { false };
        std::thread caller(
            [&]
            {
                reentry.call(sc);
                finished = true;
            });
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        check(finished.load() == !expectWait, label + (expectWait ? ": must wait for present to leave the mutex"
                                                                  : ": upstream's owner-4 DLSS-G skip is unchanged"));
        release.Open();
        presenter.join();
        caller.join();
        check(workDuringPresent.load() == !expectWait,
              label + (expectWait ? ": device work ran after present" : ": device work ran during present"));
        check(sc._localMutex.getOwner() == 0, label + ": nothing left locked");
    }
}

// A resize on another thread still waits for the FG present (FG mutex owner 2) held elsewhere.
static void OtherThreadFgMutex()
{
    for (const auto& reentry : reentries)
    {
        Reset();
        FG fg;
        auto& state = State::Instance();
        state.currentFG = &fg;
        state.activeFgOutput = FGOutput::FSRFG;
        WrappedIDXGISwapChain4 sc;
        const std::string label = std::string("other thread during FG present -> ") + reentry.name;
        Gate held, release;
        std::thread presenter(
            [&]
            {
                fg.Mutex.lock(2);
                held.Open();
                release.Wait(std::chrono::seconds(5));
                fg.Mutex.unlockThis(2);
            });
        check(held.Wait(std::chrono::seconds(5)), label + ": FG present holds its mutex");
        std::atomic<bool> finished { false };
        uint32_t ownerDuringWork = 0;
        fullscreenWork = resizeWork = [&]
        {
            ownerDuringWork = fg.Mutex.getOwner();
            return S_OK;
        };
        std::thread caller(
            [&]
            {
                reentry.call(sc);
                finished = true;
            });
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        check(!finished.load(), label + ": must wait for the FG mutex held by another thread");
        release.Open();
        presenter.join();
        caller.join();
        check(ownerDuringWork == 3, label + ": device work ran under its own FG mutex (owner 3)");
        check(fg.Mutex.getOwner() == 0, label + ": FG mutex released");
    }
}

int main()
{
    SameThreadReentry();
    SameThreadFgMutex();
    OtherThread(presents[1], FGNvngxReplacement::None, true);
    OtherThread(presents[1], FGNvngxReplacement::DLSSGMod, true);
    OtherThread(presents[0], FGNvngxReplacement::None, true);
    OtherThread(presents[0], FGNvngxReplacement::DLSSGMod, false);
    OtherThreadFgMutex();
    if (failures == 0)
        std::printf("fg-present-reentry: all cases passed\n");
    std::fflush(stdout);
    std::fflush(stderr);
    // A hung case leaves a thread blocked on a mutex; never wait for it
    std::_Exit(failures == 0 ? 0 : 1);
}

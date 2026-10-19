#include <atomic>
#include <condition_variable>
#include <future>
#include <iostream>
#include <thread>

static void check(bool condition, const char* message)
{
    if (!condition)
    {
        std::cerr << "FAIL: " << message << '\n';
        std::exit(1);
    }
}

int main()
{
    bool aliveA = true;
    int freedA = 0, freedB = 0;
    DeferredRelease::RegisterOwner(7);
    DeferredRelease::RegisterOwner(8);
    DeferredRelease::Enqueue(
        [&]
        {
            check(aliveA, "owner A still alive during destruction");
            ++freedA;
        },
        7);
    DeferredRelease::Enqueue([&] { ++freedB; }, 8);
    DeferredRelease::RetireOwner(7, true);
    aliveA = false;
    check(freedA == 1 && freedB == 0, "retire only the drained device, before its destruction");
    DeferredRelease::Enqueue([&] { check(false, "late callback for destroyed device"); }, 7);
    Clock::tick = 3;
    DeferredRelease::Collect();
    check(freedA == 1 && freedB == 1, "another device's collect cannot use the retired device");

    DeferredRelease::RegisterOwner(7); // a successful create reused the numeric handle
    DeferredRelease::Enqueue([&] { ++freedA; }, 7);
    DeferredRelease::RetireOwner(7, false);
    Clock::tick = 6;
    DeferredRelease::Collect();
    check(freedA == 1, "lost or undrained device abandons callbacks");
    DeferredRelease::RegisterOwner(7);
    DeferredRelease::Enqueue([&] { ++freedA; }, 7);
    DeferredRelease::RetireOwner(7, true);
    check(freedA == 2, "new lifetime of reused handle accepts callbacks");

    int flushed = 0;
    DeferredRelease::RegisterOwner(10);
    DeferredRelease::Enqueue([&] { ++flushed; }, 10);
    DeferredRelease::FlushOwner(10);
    check(flushed == 1, "device-wide idle flushes queued objects without waiting for wall time");
    DeferredRelease::Enqueue([&] { ++flushed; }, 10);
    DeferredRelease::FlushOwner(10);
    check(flushed == 2, "flushing for menu recreation keeps the live device open to enqueues");

    // The collector extracts both callbacks before executing the first. Pause it there so retirement
    // must account for work no longer in the pending vector, not merely erase vector entries.
    std::mutex barrier;
    std::condition_variable changed;
    bool entered = false, resume = false;
    std::atomic<bool> deviceAlive { true };
    std::atomic<int> freed { 0 };
    DeferredRelease::RegisterOwner(9);
    DeferredRelease::Enqueue(
        [&]
        {
            std::unique_lock lock(barrier);
            entered = true;
            changed.notify_all();
            changed.wait(lock, [&] { return resume; });
        });
    DeferredRelease::Enqueue(
        [&]
        {
            check(deviceAlive.load(), "detached callback completes before device destruction");
            ++freed;
        },
        9);
    Clock::tick = 9;
    auto collector = std::async(std::launch::async, [] { DeferredRelease::Collect(); });
    {
        std::unique_lock lock(barrier);
        changed.wait(lock, [&] { return entered; });
    }
    std::promise<void> retiring;
    auto retirement = std::async(std::launch::async,
                                 [&]
                                 {
                                     retiring.set_value();
                                     DeferredRelease::RetireOwner(9, true);
                                     deviceAlive = false;
                                 });
    retiring.get_future().wait();
    check(retirement.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout,
          "retirement waits for a collector which has already detached its callbacks");
    {
        std::lock_guard lock(barrier);
        resume = true;
    }
    changed.notify_all();
    collector.get();
    retirement.get();
    check(freed == 1 && !deviceAlive, "one destruction before retirement completes");
    DeferredRelease::Collect();
    check(freed == 1, "no detached callback survives retirement");
    std::cout << "PASS: drained, lost, late enqueue, reused owner and detached-collector race\n";
}

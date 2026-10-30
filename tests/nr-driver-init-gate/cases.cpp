// The driver-init gate (retoque/dlssnr/DlssNr_DriverInitGate.h): the production header, real threads, real mutexes.
// No Vulkan, NGX or driver runs; the "driver" and "NGX" calls are stand-ins that hold the gate as the real ones do.

#include <dlssnr/DlssNr_DriverInitGate.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;
namespace G = DlssNr::DriverInitGate;

namespace
{
int g_failures = 0;

void Check(bool condition, const std::string& what)
{
    if (!condition)
    {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

// A one-shot signal between threads.
struct Signal
{
    std::mutex mutex;
    std::condition_variable cv;
    bool set = false;

    void Set()
    {
        {
            std::lock_guard lock(mutex);
            set = true;
        }
        cv.notify_all();
    }

    // False when it was not set within `bound` (the case then fails rather than hangs).
    bool Wait(std::chrono::milliseconds bound = 5s)
    {
        std::unique_lock lock(mutex);
        return cv.wait_for(lock, bound, [this] { return set; });
    }
};

// Two holders on two threads: the second never holds the gate while the first does, in every pairing of kinds.
void Exclusion()
{
    for (const auto first : { G::Hold::Ngx, G::Hold::DeviceCreate })
    {
        for (const auto second : { G::Hold::Ngx, G::Hold::DeviceCreate })
        {
            G::Gate gate(5s);
            Signal firstIn;
            std::atomic<bool> firstOut { false }, secondIn { false }, overlapped { false };
            G::Entry secondEntry;

            std::thread a(
                [&]
                {
                    G::Scope scope(first, true, "first", gate);
                    firstIn.Set();
                    std::this_thread::sleep_for(60ms);
                    overlapped = overlapped || secondIn.load();
                    firstOut = true;
                });
            std::thread b(
                [&]
                {
                    firstIn.Wait();
                    G::Scope scope(second, true, "second", gate);
                    secondEntry = scope.entry();
                    secondIn = true;
                    overlapped = overlapped || !firstOut.load();
                });
            a.join();
            b.join();

            const auto label = std::string(first == G::Hold::Ngx ? "ngx" : "device") + " then " +
                               (second == G::Hold::Ngx ? "ngx" : "device");
            Check(!overlapped, label + ": the second holder entered while the first held the gate");
            Check(secondEntry.held && !secondEntry.timedOut, label + ": the second holder took the gate");
            Check(secondEntry.waited >= 30ms, label + ": the second holder waited for the first");
            Check(secondEntry.blockedBy == first, label + ": the wait names what the other thread held");
            const auto stats = gate.Snapshot();
            Check(stats.contended == 1 && stats.timedOut == 0, label + ": one contended entry, none timed out");
            Check(!gate.HeldHere(), label + ": released");
        }
    }
    std::printf("  exclusion: four pairings of holders serialized\n");
}

// The same thread re-enters (an NGX call reaching a hooked vkCreateDevice, a hook below a hook) without waiting, and
// the gate is free once the outermost scope ends.
void Reentrant()
{
    G::Gate gate(500ms);
    {
        G::Scope outer(G::Hold::Ngx, true, "outer", gate);
        {
            G::Scope inner(G::Hold::DeviceCreate, true, "inner", gate);
            Check(inner.entry().held && inner.entry().waited < 1ms, "re-entry on the owning thread does not wait");
            G::Scope innermost(G::Hold::Ngx, true, "innermost", gate);
            Check(innermost.entry().held, "a third level is held");
        }
        Check(gate.HeldHere(), "still held by the outer scope");

        std::atomic<bool> otherHeld { true };
        std::thread other(
            [&]
            {
                G::Entry e = gate.Enter(G::Hold::DeviceCreate);
                otherHeld = e.held;
                if (e.held)
                    gate.Leave(G::Hold::DeviceCreate);
            });
        other.join();
        Check(!otherHeld, "another thread is kept out while the outer scope holds (its bounded wait ends unheld)");
    }
    Check(!gate.HeldHere(), "free after the outermost scope");

    std::atomic<bool> later { false };
    std::thread after(
        [&]
        {
            G::Scope scope(G::Hold::DeviceCreate, true, "after", gate);
            later = scope.entry().held && scope.entry().waited < 50ms;
        });
    after.join();
    Check(later, "another thread takes it at once afterwards");
    std::printf("  reentrant: three levels on one thread, others kept out, free afterwards\n");
}

// Scope(..., wanted=false) does nothing: NR off leaves vkCreateDevice exactly as it was.
void NotWanted()
{
    G::Gate gate(200ms);
    G::Scope holder(G::Hold::Ngx, true, "holder", gate);
    std::atomic<bool> fast { false }, untouched { false };
    std::thread t(
        [&]
        {
            const auto start = std::chrono::steady_clock::now();
            G::Scope scope(G::Hold::DeviceCreate, false, "not wanted", gate);
            fast = std::chrono::steady_clock::now() - start < 20ms;
            untouched = !scope.entry().wanted && !scope.entry().held && !scope.entry().timedOut;
        });
    t.join();
    Check(fast, "an unwanted scope never waits");
    Check(untouched, "an unwanted scope takes nothing");
    Check(gate.Snapshot().entries == 1, "an unwanted scope is not counted");
    Check(G::WaitLine(G::Entry {}, "x").empty(), "nothing to say for an unwanted scope");
    std::printf("  not wanted: no wait, no hold, no count\n");
}

// The present host's vkCreateDevice hook, called down from OptiScaler's: its own code runs outside the gate, the
// driver call inside it, and the outer hook's hold is back when it returns.
void SuspendAndResume()
{
    G::Gate gate(5s);
    Signal suspended, otherIn, otherMayLeave;
    std::atomic<bool> otherHeldWhileSuspended { false };
    G::Entry resumed;

    std::thread outerHook(
        [&]
        {
            G::Scope outer(G::Hold::DeviceCreate, true, "outer hook", gate);
            {
                G::OwnCode own(gate);
                Check(!gate.HeldHere(), "the inner hook's own code runs outside the gate");
                suspended.Set();
                otherIn.Wait();
                {
                    G::Scope driver(G::Hold::DeviceCreate, true, "inner hook's call down", gate);
                    Check(driver.entry().held && driver.entry().waited >= 20ms,
                          "the inner call down waits for the other holder");
                    Check(driver.entry().blockedBy == G::Hold::Ngx, "the wait names the NGX call");
                }
                Check(!gate.HeldHere(), "free again after the inner call down");
            }
            Check(gate.HeldHere(), "the outer hook's hold is back when the inner hook returns");
        });
    std::thread ngx(
        [&]
        {
            suspended.Wait();
            G::Scope scope(G::Hold::Ngx, true, "ngx", gate);
            otherHeldWhileSuspended = scope.entry().held && scope.entry().waited < 50ms;
            otherIn.Set();
            std::this_thread::sleep_for(40ms);
        });
    outerHook.join();
    ngx.join();
    Check(otherHeldWhileSuspended, "another thread takes the gate while the hook's own code runs");
    Check(!gate.HeldHere(), "free after the outer hook");
    const auto stats = gate.Snapshot();
    Check(stats.suspended == 1 && stats.orphaned == 0, "one suspension, nothing orphaned");
    std::printf("  suspend/resume: the inner hook's own code ran outside, the outer hold restored\n");
}

// Never suspended under an NGX call: a device create nested inside NGX keeps the gate, or the driver's two locks could
// meet again.
void NoSuspendUnderNgx()
{
    G::Gate gate(150ms);
    {
        G::Scope ngx(G::Hold::Ngx, true, "ngx", gate);
        G::Scope nested(G::Hold::DeviceCreate, true, "nested create", gate);
        G::OwnCode own(gate);
        Check(gate.HeldHere(), "still held under an NGX call");
        std::atomic<bool> otherHeld { true };
        std::thread t(
            [&]
            {
                G::Scope other(G::Hold::DeviceCreate, true, "other", gate);
                otherHeld = other.entry().held;
            });
        t.join();
        Check(!otherHeld, "another thread is kept out");
    }
    Check(gate.Snapshot().suspended == 0, "nothing suspended");
    std::printf("  no suspension under an NGX call\n");
}

// A wait has a bound: a holder that never lets go costs a waiter the bound, then the waiter goes ahead unheld, says
// so, and its scope's end touches nothing of the holder's.
void Bounded()
{
    G::Gate gate(80ms);
    Signal holding, done;
    std::thread holder(
        [&]
        {
            G::Scope scope(G::Hold::Ngx, true, "stuck ngx", gate);
            holding.Set();
            done.Wait();
        });
    holding.Wait();
    G::Entry entry;
    {
        G::Scope waiter(G::Hold::DeviceCreate, true, "vkCreateDevice", gate);
        entry = waiter.entry();
    }
    Check(!entry.held && entry.timedOut, "the waiter went ahead unheld after the bound");
    Check(entry.waited >= 70ms && entry.waited < 2s, "it waited about the bound");
    Check(!gate.HeldHere(), "the waiter holds nothing");
    std::atomic<bool> stillHeld { false };
    std::thread probe(
        [&]
        {
            G::Entry e = gate.Enter(G::Hold::DeviceCreate);
            stillHeld = !e.held;
            if (e.held)
                gate.Leave(G::Hold::DeviceCreate);
        });
    probe.join();
    Check(stillHeld, "the holder's hold survived the waiter's scope end");
    const std::string line = G::WaitLine(entry, "vkCreateDevice");
    Check(line.find("went ahead without the gate") != std::string::npos, "the line says it went ahead: " + line);
    Check(line.find("NGX") != std::string::npos, "the line names the NGX holder: " + line);
    done.Set();
    holder.join();
    Check(gate.Snapshot().timedOut >= 1, "counted");
    std::printf("  bounded: %s\n", line.c_str());
}

// The hook's hold cannot be taken back in time: the outer hook's scope end is then a no-op, never a release of another
// thread's hold; the thread's next entries count normally.
void ResumeTimesOut()
{
    G::Gate gate(80ms);
    Signal suspended, otherHolds, release;
    std::thread outerHook(
        [&]
        {
            {
                G::Scope outer(G::Hold::DeviceCreate, true, "outer hook", gate);
                {
                    G::OwnCode own(gate);
                    suspended.Set();
                    otherHolds.Wait();
                } // the resume times out: the other thread still holds
                Check(!gate.HeldHere(), "the outer hold is lost, not stolen");
            } // a no-op: must not release the other thread's hold
            std::atomic<bool> otherStill { false };
            std::thread probe(
                [&]
                {
                    G::Entry e = gate.Enter(G::Hold::DeviceCreate);
                    otherStill = !e.held;
                    if (e.held)
                        gate.Leave(G::Hold::DeviceCreate);
                });
            probe.join();
            Check(otherStill, "the outer scope's end left the other thread's hold alone");
            release.Set();
        });
    std::thread other(
        [&]
        {
            suspended.Wait();
            G::Scope scope(G::Hold::Ngx, true, "ngx", gate);
            otherHolds.Set();
            release.Wait();
        });
    outerHook.join();
    other.join();
    Check(gate.Snapshot().orphaned == 1, "one orphaned hold");
    {
        G::Scope again(G::Hold::DeviceCreate, true, "again", gate);
        Check(again.entry().held, "the thread takes it again normally");
    }
    Check(!gate.HeldHere(), "and releases it normally");
    std::printf("  resume timeout: the lost hold is accounted, nobody's hold released\n");
}

// The cycle the design note describes, with real mutexes standing for ours:
//   T1 vkCreateDevice: OptiScaler's hook (gate) -> the present host's hook: the driver call, then the transport lock;
//   T2 vkDestroyDevice: the transport lock -> g_vkMutex (AfterUpscaleDeviceGone, blocking);
//   T3 the after-upscale route: g_vkMutex -> the gate (the model's NGX feature creation).
// With the present host's own code outside the gate (OwnCode) every thread finishes and no wait reaches the bound.
// Without it, T1 holds the gate while it waits for the transport lock and the three wait on each other until the
// bound breaks the cycle (the mutation control).
void HookChain()
{
    G::Gate gate(400ms);
    std::mutex transport, vkMutex;
    Signal routeHoldsVk, inDriver, destroyHoldsTransport;
    std::atomic<int> finished { 0 };

    std::thread t3(
        [&]
        {
            std::lock_guard route(vkMutex);
            routeHoldsVk.Set();
            inDriver.Wait();
            G::Scope create(G::Hold::Ngx, true, "the route's feature creation", gate);
            std::this_thread::sleep_for(5ms);
            ++finished;
        });
    std::thread t1(
        [&]
        {
            routeHoldsVk.Wait();
            G::Scope optiscalerHook(G::Hold::DeviceCreate, true, "OptiScaler's vkCreateDevice", gate);
            {
                G::OwnCode presentHostHook(gate);
                {
                    G::Scope callDown(G::Hold::DeviceCreate, true, "the present host's vkCreateDevice", gate);
                    inDriver.Set();
                    destroyHoldsTransport.Wait();
                    std::this_thread::sleep_for(40ms); // T3 now waits for the gate, T2 for g_vkMutex
                }
                std::lock_guard registerDevice(transport);
            }
            ++finished;
        });
    std::thread t2(
        [&]
        {
            inDriver.Wait();
            std::lock_guard destroy(transport);
            destroyHoldsTransport.Set();
            std::lock_guard routeGone(vkMutex);
            ++finished;
        });
    t1.join();
    t2.join();
    t3.join();
    Check(finished == 3, "all three threads finished");
    const auto stats = gate.Snapshot();
    Check(stats.timedOut == 0 && stats.orphaned == 0,
          "no wait reached the bound (timed out " + std::to_string(stats.timedOut) + ", orphaned " +
              std::to_string(stats.orphaned) + "): the gate closed a cycle");
    std::printf(
        "  hook chain: vkCreateDevice / vkDestroyDevice / the route's creation all finished, no bound reached\n");
}

// The process gate is one object, and a reporter set once receives what a scope has to say, after it let go.
void ProcessAndReporter()
{
    Check(&G::Process() == &G::Process(), "one process gate");
    static std::vector<std::string> lines;
    static std::vector<bool> warnings;
    static G::Gate* reported = nullptr;
    G::SetReporter(
        [](const std::string& line, bool warning)
        {
            lines.push_back(line);
            warnings.push_back(warning);
            if (reported != nullptr)
                Check(!reported->HeldHere(), "reported after the gate was let go");
        });
    G::Gate gate(60ms);
    reported = &gate;
    Signal holding, done;
    std::thread holder(
        [&]
        {
            G::Scope scope(G::Hold::DeviceCreate, true, "a vkCreateDevice", gate);
            holding.Set();
            done.Wait(300ms);
        });
    holding.Wait();
    {
        G::Scope quick(G::Hold::Ngx, true, "the builder's feature creation", gate);
    }
    done.Set();
    holder.join();
    {
        G::Scope uncontended(G::Hold::Ngx, true, "uncontended", gate);
    }
    G::SetReporter(nullptr);
    reported = nullptr;
    Check(lines.size() == 1,
          "one line: the timed-out wait; nothing for an uncontended scope (" + std::to_string(lines.size()) + ")");
    Check(!warnings.empty() && warnings[0], "a timed-out wait is a warning");
    Check(!lines.empty() && lines[0].find("the builder's feature creation") != std::string::npos &&
              lines[0].find("vkCreateDevice") != std::string::npos,
          "the line names the waiter and the holder");
    std::printf("  reporter: %s\n", lines.empty() ? "(none)" : lines[0].c_str());
}
} // namespace

int main(int argc, char** argv)
{
    const std::map<std::string, std::function<void()>> cases {
        { "exclusion", Exclusion },
        { "reentrant", Reentrant },
        { "not-wanted", NotWanted },
        { "suspend-resume", SuspendAndResume },
        { "no-suspend-under-ngx", NoSuspendUnderNgx },
        { "bounded", Bounded },
        { "resume-times-out", ResumeTimesOut },
        { "hook-chain", HookChain },
        { "process-and-reporter", ProcessAndReporter },
    };
    if (argc != 2 || cases.find(argv[1]) == cases.end())
    {
        std::fprintf(stderr, "usage: cases <case>\n");
        return 2;
    }
    cases.at(argv[1])();
    return g_failures == 0 ? 0 : 1;
}

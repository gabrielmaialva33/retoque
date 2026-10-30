#pragma once

// The driver-init gate: a vkCreateDevice and the model's NGX initialisation or feature creation never run at the same
// moment in the process (docs/dlssnr/design/nr-vulkan-present-host.md, "The driver-init gate", 2026-10-30).
//
// Why. Assassin's Creed Odyssey on DXVK froze at startup in janblade's fork (OptiScaler-F5-DLSSNR-Multipass 12de6a03,
// GPL-3.0; the idea is taken, not the code): live stacks showed the game's thread inside a hooked vkCreateDevice,
// holding the NVIDIA driver's Vulkan lock and waiting for its CUDA lock, while the render thread was inside the model's
// NGX feature creation, holding the CUDA lock and waiting for the Vulkan lock. The driver takes the two in opposite
// orders, so two threads that are in each at once never come out.
//
// What holds it, and only across the one opaque call:
//   - Hold::DeviceCreate: the call down of a vkCreateDevice hook of ours (OptiScaler's bundle, the Vulkan present
//     host's), only with [DlssNr] Enabled -- NR off, vkCreateDevice is exactly what it was;
//   - Hold::Ngx: the model's NGX initialisation, the driver core's bring-up for it and its feature creation, on every
//     route (D3D12 and the D3D11 bridge through it, the Vulkan after-upscale route, the Vulkan present host's builder).
// It is re-entrant on its owning thread (a hook below a hook; a create reached from inside an NGX call).
//
// Its place in the lock order: innermost. It is taken holding at most g_nrMutex (D3D12: under g_preMutex when the
// before-upscale route calls in) or g_vkMutex (the Vulkan after-upscale route), never g_presentNgxMutex, the present
// host's locks, g_ownerMutex or the forwarder's load lock; and while it is held our code takes nothing -- only the
// opaque call runs. The present host's vkCreateDevice hook, which runs below OptiScaler's and takes the transport lock
// after its call down (and vkDestroyDevice takes g_vkMutex under that lock), runs its own code outside the gate:
// OwnCode lets the thread's device-create holds go for it and takes them back at its end. Holding the gate across that
// code would close transport -> g_vkMutex -> gate -> transport.
//
// Bounded. A wait that reaches the bound (10 s) goes ahead without the gate and says so: the overlap the gate exists to
// prevent is then possible again, but an unforeseen cycle through it cannot hang the game. A hold that cannot be taken
// back after OwnCode is accounted so the outer scope's end releases nothing of another thread's.
//
// Standard library only, host-tested in tests/nr-driver-init-gate.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

namespace DlssNr::DriverInitGate
{
enum class Hold : uint8_t
{
    DeviceCreate, // a vkCreateDevice hook's call down
    Ngx,          // the model's NGX initialisation, core bring-up or feature creation
};

inline constexpr std::chrono::milliseconds kWaitBound { 10000 };

// What one entry did.
struct Entry
{
    bool wanted = false;                 // asked for (a Scope with wanted=false asks for nothing)
    bool held = false;                   // the gate is held for it
    bool timedOut = false;               // the wait reached the bound and went ahead without it
    Hold blockedBy = Hold::DeviceCreate; // what the other thread held, when it waited
    std::chrono::nanoseconds waited {};
};

struct Stats
{
    uint64_t entries = 0;   // entries asked for
    uint64_t contended = 0; // of which found it held by another thread
    uint64_t timedOut = 0;  // of which reached the bound (entries and resumes)
    uint64_t suspended = 0; // OwnCode let device-create holds go
    uint64_t orphaned = 0;  // a resume that reached the bound: those holds are lost, accounted
    std::chrono::nanoseconds maxWait {};
};

class Gate
{
  public:
    explicit Gate(std::chrono::nanoseconds bound = kWaitBound) : _bound(bound) {}
    Gate(const Gate&) = delete;
    Gate& operator=(const Gate&) = delete;

    // Blocks until this thread holds the gate (at once when it already does), at most the bound.
    Entry Enter(Hold hold)
    {
        Entry entry;
        entry.wanted = true;
        std::unique_lock lock(_mutex);
        ++_stats.entries;
        if (!Wait(lock, entry))
            return entry;
        Take(hold, 1);
        entry.held = true;
        return entry;
    }

    // After an Enter that returned held; the holds are let go in the reverse order of their entries.
    void Leave(Hold hold)
    {
        std::unique_lock lock(_mutex);
        const auto me = std::this_thread::get_id();
        if (_depth != 0 && _owner == me)
        {
            --_depth;
            if (hold == Hold::Ngx && _ngx != 0)
                --_ngx;
            if (_depth == 0)
            {
                _owner = {};
                lock.unlock();
                _released.notify_all();
            }
            return;
        }

        // A hold lost to a resume that reached the bound: nothing to release.
        for (auto it = _orphans.begin(); it != _orphans.end(); ++it)
        {
            if (it->first == me)
            {
                if (--it->second == 0)
                    _orphans.erase(it);
                return;
            }
        }
    }

    // This thread's holds, when every one is a device create's (never under an NGX call): let go for a stretch of our
    // own code. How many were let go (0: none held here, or an NGX call holds it).
    unsigned SuspendDeviceCreates()
    {
        std::unique_lock lock(_mutex);
        if (_depth == 0 || _owner != std::this_thread::get_id() || _ngx != 0)
            return 0;
        const unsigned holds = _depth;
        _depth = 0;
        _owner = {};
        ++_stats.suspended;
        lock.unlock();
        _released.notify_all();
        return holds;
    }

    // Takes back what SuspendDeviceCreates let go, waiting at most the bound. Unheld: those holds are lost and their
    // scopes' ends release nothing.
    Entry Resume(unsigned holds)
    {
        Entry entry;
        if (holds == 0)
            return entry;
        entry.wanted = true;
        std::unique_lock lock(_mutex);
        if (!Wait(lock, entry))
        {
            ++_stats.orphaned;
            const auto me = std::this_thread::get_id();
            auto it = std::find_if(_orphans.begin(), _orphans.end(), [&](const auto& o) { return o.first == me; });
            if (it == _orphans.end())
                _orphans.emplace_back(me, holds);
            else
                it->second += holds;
            return entry;
        }
        Take(Hold::DeviceCreate, holds);
        entry.held = true;
        return entry;
    }

    bool HeldHere() const
    {
        std::lock_guard lock(_mutex);
        return _depth != 0 && _owner == std::this_thread::get_id();
    }

    Stats Snapshot() const
    {
        std::lock_guard lock(_mutex);
        return _stats;
    }

  private:
    // Under _mutex. False when another thread kept it past the bound.
    bool Wait(std::unique_lock<std::mutex>& lock, Entry& entry)
    {
        if (_depth == 0 || _owner == std::this_thread::get_id())
            return true;

        ++_stats.contended;
        entry.blockedBy = _ngx != 0 ? Hold::Ngx : Hold::DeviceCreate;
        const auto start = std::chrono::steady_clock::now();
        const bool free = _released.wait_for(lock, _bound, [this] { return _depth == 0; });
        entry.waited = std::chrono::steady_clock::now() - start;
        if (entry.waited > _stats.maxWait)
            _stats.maxWait = entry.waited;
        if (!free)
        {
            entry.timedOut = true;
            ++_stats.timedOut;
        }
        return free;
    }

    void Take(Hold hold, unsigned holds)
    {
        _owner = std::this_thread::get_id();
        _depth += holds;
        if (hold == Hold::Ngx)
            _ngx += holds;
    }

    const std::chrono::nanoseconds _bound;
    mutable std::mutex _mutex;
    std::condition_variable _released;
    std::thread::id _owner {};
    unsigned _depth = 0; // the owner's holds
    unsigned _ngx = 0;   // of which NGX calls
    std::vector<std::pair<std::thread::id, unsigned>> _orphans;
    Stats _stats;
};

// The process's gate. Never destroyed: a thread can still be in it while the process exits.
inline Gate& Process()
{
    static Gate* gate = new Gate();
    return *gate;
}

// What an entry has to say: nothing below a millisecond's wait; otherwise who waited for what, and whether it went
// ahead without the gate.
inline std::string WaitLine(const Entry& entry, const char* site)
{
    if (!entry.wanted)
        return {};
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(entry.waited).count();
    if (!entry.timedOut && ms < 1)
        return {};
    const char* holder =
        entry.blockedBy == Hold::Ngx ? "the model's NGX initialisation or feature creation" : "a vkCreateDevice";
    std::string line = std::string("DLSS-NR driver-init gate: ") + (site != nullptr ? site : "?") + " waited " +
                       std::to_string(ms) + " ms for " + holder + " on another thread";
    if (entry.timedOut)
        line += " and went ahead without the gate (the bound was reached; the driver's Vulkan and CUDA locks may meet)";
    return line;
}

// Where the lines go (the log, set once by the DLL); none: they are dropped. Called after the gate was let go.
using Reporter = void (*)(const std::string& line, bool warning);

inline std::atomic<Reporter>& ReporterSlot()
{
    static std::atomic<Reporter> slot { nullptr };
    return slot;
}

inline void SetReporter(Reporter reporter) { ReporterSlot().store(reporter, std::memory_order_release); }

inline void Report(const Entry& entry, const char* site)
{
    const Reporter reporter = ReporterSlot().load(std::memory_order_acquire);
    if (reporter == nullptr)
        return;
    const std::string line = WaitLine(entry, site);
    if (!line.empty())
        reporter(line, entry.timedOut);
}

// The gate around one opaque call: `{ Scope gate(Hold::Ngx, true, "..."); call(); }`.
class Scope
{
  public:
    Scope(Hold hold, bool wanted, const char* site, Gate& gate = Process()) : _gate(gate), _hold(hold), _site(site)
    {
        if (wanted)
            _entry = _gate.Enter(hold);
    }

    ~Scope()
    {
        if (_entry.held)
            _gate.Leave(_hold);
        Report(_entry, _site);
    }

    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;

    const Entry& entry() const { return _entry; }

  private:
    Gate& _gate;
    const Hold _hold;
    const char* const _site;
    Entry _entry;
};

// First in a vkCreateDevice hook that may run below another of ours: this hook's own code runs outside the gate, the
// caller's device-create holds are let go until it returns. Never lets go of a hold an NGX call made.
class OwnCode
{
  public:
    explicit OwnCode(Gate& gate = Process()) : _gate(gate), _holds(gate.SuspendDeviceCreates()) {}

    ~OwnCode()
    {
        if (_holds != 0)
            Report(_gate.Resume(_holds), "a vkCreateDevice hook, taking the gate back for the hook above it");
    }

    OwnCode(const OwnCode&) = delete;
    OwnCode& operator=(const OwnCode&) = delete;

  private:
    Gate& _gate;
    const unsigned _holds;
};
} // namespace DlssNr::DriverInitGate

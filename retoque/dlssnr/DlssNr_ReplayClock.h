// The clock the NR pass decides by (docs/dlssnr/design/offline-replay-seam.md).
//
// Outside an offline replay session this is the steady clock, read exactly as before. During one, it is the instant
// the session began plus the logical milliseconds of the frame being replayed, so cadence, adaptive calm, the settle
// gates and the gap rules see recorded time rather than however fast the harness happens to run. GPU fences,
// timeouts, GPU timing, log rate limits and CPU measurements stay on the real clock.
//
// No device, no Config, no state beyond three values: tests/nr-replay-seam compiles it as it is.

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace DlssNr::ReplayClock
{
namespace Detail
{
inline std::atomic<bool> active { false };
inline std::atomic<int64_t> logicalUs { 0 };
inline std::atomic<int64_t> baseTicks { 0 }; // steady_clock::time_point at Begin, as a tick count
} // namespace Detail

inline bool Active() { return Detail::active.load(std::memory_order_acquire); }

inline std::chrono::steady_clock::time_point Now()
{
    if (!Active())
        return std::chrono::steady_clock::now();
    const std::chrono::steady_clock::time_point base { std::chrono::steady_clock::duration(
        Detail::baseTicks.load(std::memory_order_relaxed)) };
    return base + std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                      std::chrono::microseconds(Detail::logicalUs.load(std::memory_order_relaxed)));
}

// The replay adapter only. Begin anchors logical time at the real instant it is called, so a session continues
// forward from every timestamp the pass took before it.
inline void Begin()
{
    Detail::baseTicks.store(std::chrono::steady_clock::now().time_since_epoch().count(), std::memory_order_relaxed);
    Detail::logicalUs.store(0, std::memory_order_relaxed);
    Detail::active.store(true, std::memory_order_release);
}

inline void Set(double ms) { Detail::logicalUs.store(static_cast<int64_t>(ms * 1000.0), std::memory_order_relaxed); }

inline void End() { Detail::active.store(false, std::memory_order_release); }
} // namespace DlssNr::ReplayClock

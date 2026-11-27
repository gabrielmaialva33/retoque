#pragma once

#include <atomic>
#include <cstdint>

namespace DlssNr::VkActivity
{
// Adapted from janblade/OptiScaler-F5-DLSSNR-Multipass (GPL-3.0), f853ec4e.
// A retained feature says nothing about a live switch to the D3D12 bridge. Publish only a
// successfully recorded native composition; no resource reads on the menu's thread.
inline bool Recent(uint64_t last, uint64_t now) { return last != 0 && now >= last && now - last < 1000; }
class State
{
    std::atomic<uint64_t> _last { 0 };

  public:
    void Recorded(uint64_t now) { _last.store(now, std::memory_order_relaxed); }
    void Clear() { _last.store(0, std::memory_order_relaxed); }
    bool Running(uint64_t now) const { return Recent(_last.load(std::memory_order_relaxed), now); }
};
} // namespace DlssNr::VkActivity

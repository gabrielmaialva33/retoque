#pragma once

#include <cstdint>
#include <mutex>

// Adapted from janblade/OptiScaler-F5-DLSSNR-Multipass (GPL-3.0), c4dd476e, 86686b81 and
// 76327eb6: device plus start/stop generation, not just an initialized bit. Metadata only;
// no GPU retirement and no opaque driver call under this leaf mutex.
namespace DlssNr::VkCore
{
struct Token
{
    uintptr_t device = 0;
    uint64_t generation = 0;
    explicit operator bool() const { return device != 0 && generation != 0; }
    bool operator==(const Token&) const = default;
};

class State
{
    mutable std::mutex _mutex;
    Token _current;
    uint64_t _generation = 0;

  public:
    Token Current() const
    {
        std::lock_guard lock(_mutex);
        return _current;
    }
    uint64_t Generation() const
    {
        std::lock_guard lock(_mutex);
        return _generation;
    }
    bool Matches(Token token) const
    {
        std::lock_guard lock(_mutex);
        return token && token == _current;
    }
    Token Start(uintptr_t device)
    {
        std::lock_guard lock(_mutex);
        _current = { device, ++_generation };
        return _current;
    }
    // A successful private init must not claim a start/stop observed while the driver call ran.
    Token StartIfUnchanged(uintptr_t device, uint64_t before)
    {
        std::lock_guard lock(_mutex);
        if (_generation != before)
            return {};
        _current = { device, ++_generation };
        return _current;
    }
    void Stop(uintptr_t device = 0)
    {
        std::lock_guard lock(_mutex);
        if (device && _current.device != device)
            return;
        _current = { 0, ++_generation };
    }
    bool StopIfCurrent(Token owned)
    {
        std::lock_guard lock(_mutex);
        if (!owned || owned != _current)
            return false;
        _current = { 0, ++_generation };
        return true;
    }
};
} // namespace DlssNr::VkCore

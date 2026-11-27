#pragma once

// Adapted from ShyVortex/OptiScaler-DLSSNR-PreSR-Multipass, 5e3a5a3d (GPL-3.0).
// One DX menu/input context owns one window; a secondary present cannot refresh its lease.
// The caller serializes Accept with the overlay's existing mutex. No GPU lifetime decisions here.
#include <cstdint>

namespace MenuWindowOwnerPolicy
{
inline constexpr uint64_t kOwnerStaleMs = 1000;

template <typename HandleType> class Owner
{
    HandleType _owner {};
    uint64_t _lastPresent = 0;

  public:
    HandleType Handle() const { return _owner; }

    bool Accept(HandleType candidate, bool ownerIsWindow, uint64_t now)
    {
        if (candidate == HandleType {})
            return false;
        if (_owner != HandleType {} && candidate != _owner && ownerIsWindow &&
            (now < _lastPresent || now - _lastPresent < kOwnerStaleMs))
            return false;
        _owner = candidate;
        _lastPresent = now;
        return true;
    }
};
} // namespace MenuWindowOwnerPolicy

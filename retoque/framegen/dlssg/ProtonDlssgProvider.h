#pragma once

#include <array>
#include <cstdint>
#include <string_view>

// Under Proton the NVIDIA Linux driver's Wine build ships its own nvngx_dlssg.dll in /usr/lib/nvidia/wine, which the
// game sees as Z:\run\host\usr\lib\nvidia\wine\ (pressure-vessel) or Z:\usr\lib\nvidia\wine\, and its NGX core loads
// that copy as the DLSS-G provider even when the game ships a newer one. The Ada MFG unlock (MfgUnlock.h) patches
// the provider in memory and knows the 310.9 builds the games ship; driver 620.73.04's Wine copy is 310.2.1, which it
// does not match, so NGX published one generated frame and Streamline logged "Multi-frame not supported, max generated
// frames 1 (SL Plugin supports 5, NGX feature supports 1)" while the game's 310.9.16, patched, sat unused
// (The Witcher 3, RTX 4090, 2026-11-22).
//
// With the unlock on, under Wine, a load of the driver's copy is answered with the game's own nvngx_dlssg.dll when
// that one is newer: the NVIDIA-signed file as shipped, already mapped and patched by the time NGX asks. Nothing on
// disk changes. An older or equal game copy, or none, leaves the driver's copy alone.
namespace ProtonDlssgProvider
{
using Version = std::array<uint16_t, 4>; // major, minor, patch, build

// `lowered` is the full path of the module being loaded, lexically normalised and lower-cased.
inline bool IsDriverWineCopy(std::wstring_view lowered)
{
    constexpr std::wstring_view kDir = L"\\nvidia\\wine\\";
    constexpr std::wstring_view kFile = L"\\nvngx_dlssg.dll";
    return lowered.size() > kFile.size() && lowered.substr(lowered.size() - kFile.size()) == kFile &&
           lowered.find(kDir) != std::wstring_view::npos;
}

inline bool UseGameCopy(bool unlockOn, bool preferGame, bool wine, bool driverCopy, bool gameCopyKnown,
                        const Version& game, const Version& driver)
{
    return unlockOn && preferGame && wine && driverCopy && gameCopyKnown && game > driver;
}
} // namespace ProtonDlssgProvider

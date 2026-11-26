#pragma once

#include "MfgPatchPlan.h"
#include <algorithm>
#include <span>
#include <unordered_map>

// Unique executable-section match and 1/3/5 ceiling admission, independently implemented from
// dashdogy/RTX40MFG-Unlock (MIT), bb2c902, PatchUniqueWrapperMaximumPattern. No control-route code copied.
namespace MfgWrapper
{
struct Section
{
    const uint8_t* data;
    size_t size;
    bool executable;
};
struct Match
{
    size_t matches = 0;
    uintptr_t countAt = 0;
    uint32_t maximum = 0;
    bool Accepted() const { return matches == 1 && (maximum == 1 || maximum == 3 || maximum == 5); }
};

inline bool LegacyInterposer(bool slInit, bool slGetFeatureFunction) { return slInit && !slGetFeatureFunction; }

inline Match Scan(std::span<const Section> sections)
{
    // Ignore the complete immediate during matching, then validate it as a DWORD. Unsupported values
    // still count toward uniqueness: a second generic min() must not disappear behind ceiling filtering.
    // r8d: Streamline 2.7.1. edx: 2.7.3, 2.7.30, 2.8.0, 2.9.0, 2.9.1 and 2.14.1;
    // each was unique in executable sections of eleven copies from ten games (2026-10-14).
    // 2.14.1 already carries five; earlier observed wrappers carry three.
    constexpr uint8_t edx[] = { 0xBA, 0, 0, 0, 0, 0x3B, 0xCA, 0x0F, 0x42, 0xD1 };
    constexpr uint8_t r8[] = { 0x41, 0xB8, 0, 0, 0, 0, 0x41, 0x3B, 0xC8, 0x44, 0x0F, 0x42, 0xC1 };
    Match result;
    for (const auto& section : sections)
    {
        if (!section.executable)
            continue;
        const auto scan = [&](const auto& pattern, size_t immediate)
        {
            if (section.size < sizeof(pattern))
                return;
            for (size_t off = 0; off <= section.size - sizeof(pattern); ++off)
            {
                bool matches = true;
                for (size_t i = 0; i < sizeof(pattern); ++i)
                    if ((i < immediate || i >= immediate + 4) && section.data[off + i] != pattern[i])
                        matches = false;
                if (!matches)
                    continue;
                ++result.matches;
                result.countAt = reinterpret_cast<uintptr_t>(section.data + off + immediate);
                std::memcpy(&result.maximum, section.data + off + immediate, sizeof(result.maximum));
            }
        };
        scan(edx, 1);
        scan(r8, 2);
    }
    return result;
}

// Load notifications carry the actual wrapper handle. Keep only changed, committed sites, so a
// repeated load of the same image is cheap and a failed write is not retried/logged on every call.
// A remap at the same address restores NVIDIA's original bytes (provider twin: 15282cd3).
// Fresh section bounds are checked before reading a previous site; no stale module pointer is read.
class Attempts
{
    std::unordered_map<uintptr_t, std::vector<MfgPatch::Site>> done;

  public:
    bool Contains(uintptr_t module) const { return done.contains(module); }
    bool NeedsAttempt(uintptr_t module, bool load, std::span<const Section> sections) const
    {
        const auto entry = done.find(module);
        if (entry == done.end())
            return true;
        if (!load)
            return false;
        for (const auto& site : entry->second)
        {
            if (site.NoOp())
                continue;
            for (const auto& section : sections)
            {
                const auto start = reinterpret_cast<uintptr_t>(section.data);
                if (section.executable && site.at >= start && site.at - start <= section.size &&
                    site.size <= section.size - (site.at - start) &&
                    std::memcmp(reinterpret_cast<const void*>(site.at), site.original.data(), site.size) == 0)
                    return true;
            }
        }
        return false;
    }
    void Remember(uintptr_t module, const std::vector<MfgPatch::Site>& committed) { done[module] = committed; }
};
} // namespace MfgWrapper

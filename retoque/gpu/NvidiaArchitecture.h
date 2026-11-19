#pragma once

#include <cstdint>
#include <string_view>

namespace Retoque::NvidiaArchitecture
{
// A deliberately bounded hardware hint, not a complete PCI database. 2504 is the GA106 RTX 3060
// observed reporting Ada under dlssg_for_sm86. Other GeForce RTX 20/30 cards use the adapter's name;
// professional names and unknown IDs stay unknown rather than guessing from broad PCI ranges.
constexpr uint32_t AdapterHint(uint32_t deviceId, std::string_view name)
{
    if (deviceId == 0x2504)
        return 0x170; // Ampere

    if (name.starts_with("NVIDIA "))
        name.remove_prefix(7);
    constexpr std::string_view prefix = "GeForce RTX ";
    if (!name.starts_with(prefix))
        return 0;
    name.remove_prefix(prefix.size());
    if (name.size() < 4 || (name.size() > 4 && name[4] != ' '))
        return 0;
    for (unsigned i = 0; i < 4; ++i)
        if (name[i] < '0' || name[i] > '9')
            return 0;
    if (name.starts_with("20"))
        return 0x160; // Turing
    if (name.starts_with("30"))
        return 0x170; // Ampere
    return 0;
}

// For Retoque decisions only. Never change IdentifyGpu's cache or the NVAPI result seen by the game
// and third-party loaders. A hardware hint can only demote: no promotion of an older or unknown
// (zero) NVAPI architecture. This also works before the loader module is visible to a module probe.
constexpr uint32_t ForAdapter(bool isNvidia, uint32_t deviceId, std::string_view name, uint32_t reported)
{
    if (!isNvidia)
        return reported;
    const uint32_t hint = AdapterHint(deviceId, name);
    return hint != 0 && hint < reported ? hint : reported;
}
} // namespace Retoque::NvidiaArchitecture

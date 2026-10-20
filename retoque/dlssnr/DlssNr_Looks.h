#pragma once

// One-click appearance bundles (docs/dlssnr/design/looks.md). Pure data and matching, no Config dependency, so the
// host test compiles it alone; the menu reads and writes the existing keys.

#include <array>
#include <cmath>
#include <cstdint>

namespace DlssNr::Looks
{
struct Values
{
    float Intensity = 1.0f;
    uint32_t Style = 0; // 0 A (default), 1 B (natural), 2 C (cinematic)
    float LocalStructure = 1.0f;
    float LocalTone = 1.0f;
    float SkinStructure = -1.0f; // -1 follows local structure
    bool AutoMask = true;
    float ColourStrength = 1.0f;
    float DetailStrength = 1.0f; // TransferStrength
};

struct Look
{
    const char* Name;
    const char* Help;
    Values Value;
};

inline constexpr std::array<Look, 6> kLooks { {
    { "Default", "The model's own defaults: style A, everything at 1.", Values {} },
    { "Natural", "NVIDIA's style B: softer contrast and saturation, kinder to skin.",
      Values { 1.0f, 1, 1.0f, 1.0f, -1.0f, true, 1.0f, 1.0f } },
    { "Cinematic", "NVIDIA's style C: a gentler, filmic saturation.",
      Values { 1.0f, 2, 1.0f, 1.0f, -1.0f, true, 1.0f, 1.0f } },
    { "Subtle", "What NVIDIA's RTX Remix ships: structure 0.7, tone 0.3, no skin mask -- closer to the game.",
      Values { 1.0f, 0, 0.7f, 0.3f, -1.0f, false, 1.0f, 1.0f } },
    { "Gentle", "A conservative community recipe: intensity 0.5, structure 0.7, tone 0.3, colour 0.5, detail 0.85.",
      Values { 0.5f, 0, 0.7f, 0.3f, -1.0f, true, 0.5f, 0.85f } },
    { "Game colour", "Keeps the game's own hue (colour strength 0); only the light changes.",
      Values { 1.0f, 0, 1.0f, 1.0f, -1.0f, true, 0.0f, 1.0f } },
} };

inline bool Near(float a, float b) { return std::fabs(a - b) <= 1e-4f; }

inline bool Matches(const Values& a, const Values& b)
{
    return Near(a.Intensity, b.Intensity) && a.Style == b.Style && Near(a.LocalStructure, b.LocalStructure) &&
           Near(a.LocalTone, b.LocalTone) && Near(a.SkinStructure, b.SkinStructure) && a.AutoMask == b.AutoMask &&
           Near(a.ColourStrength, b.ColourStrength) && Near(a.DetailStrength, b.DetailStrength);
}

// The look the current values are, or -1 (Custom).
inline int Find(const Values& current)
{
    for (size_t i = 0; i < kLooks.size(); ++i)
        if (Matches(current, kLooks[i].Value))
            return static_cast<int>(i);
    return -1;
}
} // namespace DlssNr::Looks

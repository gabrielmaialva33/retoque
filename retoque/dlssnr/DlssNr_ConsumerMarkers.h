#pragma once

// Which module names belong to another DLSS 5 neural-rendering producer. Kept apart from
// DlssNr_Consumers.cpp, which enumerates modules through Win32, so a host test can check the list
// (tests/nr-consumers).
//
// A marker matches case-insensitively anywhere in a module's base file name, and appears in that
// producer's name and in nothing else that legitimately shares the process:
//   - never a substring of "nvngx": the driver core (_nvngx.dll), our own module under whatever name
//     it was installed as, and our forwarder (nvngx.dll_dlssnr.dll) all carry it;
//   - never plain "renodx": RenoDX's per-game HDR add-ons (renodx-<game>.addon64) are ReShade
//     add-ons that do not run the neural model, and a user combining one with our NR is the normal
//     case, not a double pass. Only its DLSS builds (renodx-dlss.addon64, renodx-dlss5.addon64) run it.

#include <cstddef>
#include <string_view>

namespace DlssNr::Consumers
{
struct Competitor
{
    const wchar_t* marker;
    const char* project;
};

inline constexpr Competitor kCompetitors[] = {
    { L"deep-fried-chicken", "Deep Fried Chicken" },
    { L"renodx-dlss", "RenoDX DLSS (renodx-dlss / renodx-dlss5)" },
    { L"renodx_dlss", "RenoDX DLSS (renodx-dlss / renodx-dlss5)" },
    { L"dlss5-feed", "DLSS5-Feeder" },
    { L"dlss5_feed", "DLSS5-Feeder" },
};

// The producer a module base file name belongs to, or nullptr. ASCII case folding only: the
// markers are ASCII, and a non-ASCII character can never be part of a match.
inline const Competitor* MatchModuleName(std::wstring_view fileName)
{
    const auto fold = [](wchar_t c) { return c >= L'A' && c <= L'Z' ? static_cast<wchar_t>(c - L'A' + L'a') : c; };

    for (const auto& competitor : kCompetitors)
    {
        const std::wstring_view marker = competitor.marker;

        if (marker.size() > fileName.size())
            continue;

        for (std::size_t at = 0; at + marker.size() <= fileName.size(); ++at)
        {
            std::size_t i = 0;

            while (i < marker.size() && fold(fileName[at + i]) == marker[i])
                ++i;

            if (i == marker.size())
                return &competitor;
        }
    }

    return nullptr;
}
} // namespace DlssNr::Consumers

#pragma once

#include <cctype>
#include <cstddef>
#include <optional>
#include <string>
#include <string_view>

// The parts of the model's own diagnostics (DlssNr_ModelLog) that OptiScaler reads back. Header-only and
// free of <windows.h>, so tests/nr-model-loader runs it as shipped. Each function takes one line as the
// model wrote it, with or without the "DLSSNR: " prefix, and the formats are the model's printf strings
// (read from 310.8.2.0 and 310.9.16.0):
//
//   "DLSSNR: %zu config(s) available:"
//   "DLSSNR:   [%zu] %s (backbone: %s)"
//   "DLSSNR: Created feature %u (output %ux%u, network %ux%u, preset=%d -> %s)"
//   "DLSSNR: BuildActiveNetwork config=%s N=%d W=%d H=%d"
//
// They were seen on Windows on 2026-09-21 (neural-amd's ETS2 trace, with NGX logging switched on), and no
// Proton run here has shown one. The model reads its log level from the registry
// (HKLM\SOFTWARE\NVIDIA Corporation\Global\NGXCore, LogLevel), which nothing here sets: the likely
// reason, not yet tested. Until a line arrives every reader below has nothing to report.
namespace DlssNr::ModelLogParse
{
// "N config(s) available" -> N.
inline std::optional<int> ConfigCount(std::string_view text)
{
    constexpr std::string_view kMarker = " config(s) available";
    const auto at = text.find(kMarker);

    if (at == std::string_view::npos)
        return std::nullopt;

    size_t start = at;
    while (start > 0 && std::isdigit(static_cast<unsigned char>(text[start - 1])) && at - start < 4)
        --start;

    if (start == at)
        return std::nullopt;

    int count = 0;
    for (size_t i = start; i < at; ++i)
        count = count * 10 + (text[i] - '0');

    return count;
}

struct ConfigEntry
{
    size_t index = 0;
    std::string name;
    std::string backbone;
};

// "[i] name (backbone: b)" -> { i, name, b }, one of the lines that follow the count. The name runs
// from "] " to the last " (backbone: ", so a name with brackets or parentheses of its own survives.
inline std::optional<ConfigEntry> ConfigLine(std::string_view text)
{
    constexpr std::string_view kBackbone = " (backbone: ";

    while (!text.empty() && (text.back() == ' ' || text.back() == '\r' || text.back() == '\n'))
        text.remove_suffix(1);

    const auto backboneAt = text.rfind(kBackbone);

    if (backboneAt == std::string_view::npos || text.empty() || text.back() != ')')
        return std::nullopt;

    const auto open = text.find('[');

    if (open == std::string_view::npos || open > backboneAt)
        return std::nullopt;

    size_t digitsEnd = open + 1;
    while (digitsEnd < backboneAt && std::isdigit(static_cast<unsigned char>(text[digitsEnd])) && digitsEnd - open <= 4)
        ++digitsEnd;

    if (digitsEnd == open + 1 || text.substr(digitsEnd, 2) != "] ")
        return std::nullopt;

    // Only spaces and the "DLSSNR:" prefix may come before the bracket, so an unrelated line that
    // happens to hold "[3] ... (backbone: ...)" in the middle is not taken for one of these.
    std::string_view lead = text.substr(0, open);
    if (lead.substr(0, 7) == "DLSSNR:")
        lead.remove_prefix(7);
    if (lead.find_first_not_of(' ') != std::string_view::npos)
        return std::nullopt;

    ConfigEntry entry;
    for (size_t i = open + 1; i < digitsEnd; ++i)
        entry.index = entry.index * 10 + static_cast<size_t>(text[i] - '0');

    const size_t nameStart = digitsEnd + 2;

    if (nameStart >= backboneAt)
        return std::nullopt;

    entry.name = std::string(text.substr(nameStart, backboneAt - nameStart));
    const size_t backboneStart = backboneAt + kBackbone.size();
    entry.backbone = std::string(text.substr(backboneStart, text.size() - 1 - backboneStart));
    return entry;
}

// The weight set the model actually built, whatever preset was asked for, and the preset when the line
// says it.
struct ActiveBuild
{
    std::string name;
    std::optional<int> preset;
};

// "Created feature 1 (output ..., preset=1 -> name)" -> { name, 1 };
// "BuildActiveNetwork config=name N=..." -> { name }.
inline std::optional<ActiveBuild> ActiveConfig(std::string_view text)
{
    constexpr std::string_view kCreated = "Created feature ";
    constexpr std::string_view kPreset = ", preset=";
    constexpr std::string_view kArrow = " -> ";

    if (const auto created = text.find(kCreated); created != std::string_view::npos)
    {
        const auto presetAt = text.find(kPreset, created);
        const auto arrow = presetAt == std::string_view::npos ? presetAt : text.find(kArrow, presetAt);

        while (!text.empty() && (text.back() == ' ' || text.back() == '\r' || text.back() == '\n'))
            text.remove_suffix(1);

        if (arrow == std::string_view::npos || text.empty() || text.back() != ')')
            return std::nullopt;

        size_t i = presetAt + kPreset.size();
        const bool negative = i < arrow && text[i] == '-';
        if (negative)
            ++i;

        if (i == arrow || arrow - i > 4)
            return std::nullopt;

        int preset = 0;
        for (; i < arrow; ++i)
        {
            if (!std::isdigit(static_cast<unsigned char>(text[i])))
                return std::nullopt;
            preset = preset * 10 + (text[i] - '0');
        }

        const size_t nameStart = arrow + kArrow.size();
        const std::string_view name = text.substr(nameStart, text.size() - 1 - nameStart);

        if (name.empty() || name.find(' ') != std::string_view::npos)
            return std::nullopt;

        return ActiveBuild { std::string(name), negative ? -preset : preset };
    }

    constexpr std::string_view kMarker = "BuildActiveNetwork config=";
    const auto at = text.find(kMarker);

    if (at == std::string_view::npos)
        return std::nullopt;

    const size_t start = at + kMarker.size();
    const auto end = text.find(" N=", start);
    const std::string_view name = text.substr(start, end == std::string_view::npos ? text.size() - start : end - start);

    if (name.empty() || name.find(' ') != std::string_view::npos)
        return std::nullopt;

    return ActiveBuild { std::string(name), std::nullopt };
}

// The weight sets a released model is known to carry, recognised by the part of the name that
// identifies them. 310.8 has only the first; 310.9.16 adds the second, which NVIDIA announced on
// 2026-10-13 as its faster model. Anything else is Unknown and shown by its raw name.
enum class WeightSet
{
    Unknown,
    September, // CC_Control_History_Blend_Quantize_With_Teacher_honest_tench_2026_07_04_22_30_weights
    October,   // hnet-crazy-cuckoo-small-fp8
};

inline WeightSet Classify(std::string_view name)
{
    if (name.find("honest_tench") != std::string_view::npos)
        return WeightSet::September;
    if (name.find("crazy-cuckoo-small") != std::string_view::npos ||
        name.find("crazy_cuckoo_small") != std::string_view::npos)
        return WeightSet::October;
    return WeightSet::Unknown;
}
} // namespace DlssNr::ModelLogParse

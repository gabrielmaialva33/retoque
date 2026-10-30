#pragma once

// The model's own count of the VRAM its features hold (docs/dlssnr/design/model-vram-stats.md).
//
// Asked once after each successful build of the D3D12 feature, through the stats callback the model publishes beside
// its scaling-ratio callback (DLSSNRGetStatsCallback -> GetStatsCommon -> SizeInBytes, in 310.8.2 and 310.9.16; the
// call shape is RTX Remix's queryDlssNeuralRenderingVramAllocation, dxvk-remix 09808cc70, MIT). The query itself lives
// in both loaders (dlssnr_query_stats, DirectRuntime::QueryStats); this header is only the host's text about the
// answer. Standard library only, so tests/nr-model-stats compiles it as it is.

#include <cstdint>
#include <cstdio>
#include <string>

namespace DlssNr::ModelStats
{
// No figure. Also the value both loaders write into SizeInBytes before the callback, so it is never a real answer.
inline constexpr unsigned long long kUnknown = ~0ull;

// The loader has no stats query at all (a forwarder from before dlssnr_query_stats).
inline constexpr int kNoQuery = -2;

// The loaders' refusal after a fault inside the model (kFaulted in both).
inline constexpr int kModelFaulted = static_cast<int>(0xBAD0FA17u);

inline constexpr bool Known(unsigned long long bytes) { return bytes != kUnknown; }

inline constexpr unsigned long long kMiB = 1024ull * 1024ull;

// Nearest whole MiB, halves up, without overflowing at the top of the range.
inline constexpr unsigned long long RoundedMiB(unsigned long long bytes)
{
    return bytes / kMiB + (bytes % kMiB >= kMiB / 2 ? 1 : 0);
}

inline std::string MiBText(unsigned long long bytes)
{
    const auto mib = RoundedMiB(bytes);
    if (mib == 0 && bytes != 0)
        return "under 1 MiB";
    return std::to_string(mib) + " MiB";
}

// The INFO line for a figure. The model's count covers every feature it holds when asked: at the first build of the
// process that is the one just built; at a later one a feature still being retired (DlssNr_Dx12_Lifetime.cpp keeps it
// until the GPU is past it) may be counted too, and the line says so.
inline std::string InfoLine(unsigned long long bytes, bool firstBuild)
{
    const auto exact = std::to_string(bytes) + " bytes, its own count";
    if (firstBuild)
        return "DLSS-NR: the model's feature uses " + MiBText(bytes) + " (" + exact + ")";
    return "DLSS-NR: the model's features use " + MiBText(bytes) + " (" + exact +
           " at this build; a feature still being retired may be in it)";
}

// The DEBUG line when there is no figure. rc is the query's answer (1, 0, -1) or kNoQuery; lastResult the callback's.
inline std::string Unreported(int rc, int lastResult)
{
    std::string why;
    if (rc == kNoQuery)
        why = "this forwarder predates the stats query";
    else if (rc == 0)
        why = "it publishes no stats callback, or NGX is not up in this loader";
    else if (lastResult == kModelFaulted)
        why = "its stats callback faulted; the fault has a line of its own";
    else if (lastResult == 1)
        why = "its stats callback answered without a size";
    else
    {
        char code[16] {};
        std::snprintf(code, sizeof(code), "0x%08X", static_cast<unsigned int>(lastResult));
        why = std::string("its stats callback answered ") + code;
    }
    return "DLSS-NR: no VRAM figure from the model (" + why + ")";
}
} // namespace DlssNr::ModelStats

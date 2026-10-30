// The model is never built below kDlssNrMinExtent on Vulkan: the shared decision (DlssNr_Common.h) and the present
// host's count and line for it (DlssNr_VkPresentTransport.h, DlssNr_VkPresentNr.h), production headers, no Vulkan run.

#include <shaders/dlssnr/DlssNr_Common.h>
#include <dlssnr/DlssNr_VkPresentTransport.h>

#include <cstdint>
#include <cstdio>
#include <string>

namespace
{
int g_failures = 0;

void Check(bool condition, const std::string& what)
{
    if (!condition)
    {
        std::fprintf(stderr, "FAIL: %s\n", what.c_str());
        ++g_failures;
    }
}

std::string Size(uint32_t w, uint32_t h, uint32_t ww, uint32_t wh)
{
    return std::to_string(w) + "x" + std::to_string(h) + " working " + std::to_string(ww) + "x" + std::to_string(wh);
}
} // namespace

// The rule is a compile-time fact as well.
static_assert(kDlssNrMinExtent == 64, "the floor the D3D12 route and DLSS5VKLayer use");
static_assert(DlssNr::BelowMinExtent(1, 1, 1, 1), "the 1x1 probe swapchain that hung the GPU (Xid 109)");
static_assert(!DlssNr::BelowMinExtent(64, 64, 64, 64), "64 is built");

int main()
{
    // Each of the four sizes alone, at and either side of the floor.
    for (int which = 0; which < 4; ++which)
    {
        for (uint32_t v : { 0u, 1u, 63u, 64u, 65u, 3440u })
        {
            uint32_t s[4] = { 1920, 1080, 1920, 1080 };
            s[which] = v;
            const bool refused = DlssNr::BelowMinExtent(s[0], s[1], s[2], s[3]);
            Check(refused == (v < 64), Size(s[0], s[1], s[2], s[3]) + (refused ? " refused" : " built"));
        }
    }

    // Probe swapchains, and a real frame made small by the working scale (the slider reaches 0.25; the D3D11/Vulkan
    // after-upscale working size is the frame times it, rounded).
    Check(DlssNr::BelowMinExtent(1, 1, 1, 1), "1x1 probe");
    Check(DlssNr::BelowMinExtent(8, 8, 8, 8), "8x8 launcher window");
    Check(DlssNr::BelowMinExtent(256, 200, 64, 50), "a 256x200 window at 0.25 works at 64x50");
    Check(!DlssNr::BelowMinExtent(256, 256, 64, 64), "a 256x256 window at 0.25 works at 64x64");
    Check(!DlssNr::BelowMinExtent(841, 671, 841, 671), "DOOM Eternal's start window");
    Check(!DlssNr::BelowMinExtent(7680, 4320, 15360, 8640), "large sizes are not refused");

    // The present host: a refusal is counted per reason and the line says it, with the first refusal's text.
    using namespace DlssNr::VkPresentHost;
    TransportStats stats;
    for (int i = 0; i < 3; ++i)
    {
        Outcome o;
        o.counted = true;
        o.transported = true;
        o.nrSkip = NrSkip::TooSmall;
        o.nrNote = "1x1 (working 1x1) is below 64 pixels; the model is not built that small";
        stats.Add(o);
    }
    Check(stats.nr.refused == 3 && stats.nr.skips[static_cast<size_t>(NrSkip::TooSmall)] == 3,
          "three too-small refusals counted");
    const std::string line = NrText(stats.nr);
    Check(line.find("refused 3 (too small 3)") != std::string::npos, "the line counts them: " + line);
    Check(line.find("first refusal: 1x1 (working 1x1) is below 64 pixels") != std::string::npos,
          "the line gives the reason: " + line);
    std::printf("  present host: %s\n", line.c_str());

    if (g_failures != 0)
        return 1;
    std::printf("PASS: the Vulkan model's floor decided once, counted and said\n");
    return 0;
}

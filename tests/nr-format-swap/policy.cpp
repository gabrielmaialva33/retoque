// The pure rule in retoque/dlssnr/DlssNr_FormatTwin.h (docs/dlssnr/design/format-swap-history.md), on its own.
#include <dlssnr/DlssNr_FormatTwin.h>

#include <cstdio>
#include <limits>

using namespace DlssNr::FormatTwin;

static int failures = 0;

static void Check(bool ok, const char* what)
{
    std::printf("%s: %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok)
        ++failures;
}

// DXGI_FORMAT values.
constexpr int kRgba32F = 2, kRgba16F = 10, kRgb10A2 = 24, kR11G11B10 = 26, kRgba8Typeless = 27, kRgba8 = 28,
              kRgba8Srgb = 29, kBgra8 = 87, kBgrx8 = 88, kBgra8Typeless = 90, kBgra8Srgb = 91, kBgrx8Typeless = 92,
              kBgrx8Srgb = 93;

static_assert(ChannelOrderCanonical(kBgra8) == kRgba8);
static_assert(ChannelOrderCanonical(kBgra8Typeless) == kRgba8Typeless);
static_assert(ChannelOrderCanonical(kBgra8Srgb) == kRgba8Srgb);

static Contract At(int format, Transfer t = {})
{
    Contract c;
    c.format = format;
    c.width = 3440;
    c.height = 1440;
    c.workWidth = 1720;
    c.workHeight = 720;
    c.transfer = t;
    return c;
}

int main()
{
    // Only BGRA8 moves, each to its own RGBA8 member; everything else is its own canonical form.
    const int others[] = { 0,      kRgba32F,   kRgba16F, kRgb10A2,       kR11G11B10, kRgba8Typeless,
                           kRgba8, kRgba8Srgb, kBgrx8,   kBgrx8Typeless, kBgrx8Srgb };
    bool fixed = true;
    for (const int f : others)
        fixed = fixed && ChannelOrderCanonical(f) == f;
    Check(fixed, "every format but BGRA8 is its own canonical form");

    Check(ChannelOrderTwins(kRgba8, kBgra8) && ChannelOrderTwins(kBgra8, kRgba8), "28 and 87 are twins, both ways");
    Check(ChannelOrderTwins(kRgba8Typeless, kBgra8Typeless), "27 and 90 are twins");
    Check(ChannelOrderTwins(kRgba8Srgb, kBgra8Srgb), "29 and 91 are twins");
    Check(!ChannelOrderTwins(kRgba8, kRgba8), "a format is not its own twin: no change");
    Check(!ChannelOrderTwins(0, kRgba8) && !ChannelOrderTwins(kRgba8, 0), "no surfaces yet is not a twin");
    Check(!ChannelOrderTwins(kRgba8, kRgba8Srgb) && !ChannelOrderTwins(kBgra8, kBgra8Srgb), "UNORM and sRGB are not");
    Check(!ChannelOrderTwins(kRgba8, kBgra8Srgb) && !ChannelOrderTwins(kRgba8Srgb, kBgra8), "nor across the order");
    Check(!ChannelOrderTwins(kRgba8Typeless, kRgba8) && !ChannelOrderTwins(kRgba8Typeless, kBgra8),
          "typeless and typed are not");
    Check(!ChannelOrderTwins(kBgra8, kBgrx8) && !ChannelOrderTwins(kRgba8, kBgrx8) &&
              !ChannelOrderTwins(kBgra8Typeless, kBgrx8Typeless) && !ChannelOrderTwins(kBgra8Srgb, kBgrx8Srgb),
          "BGRX8 is not: alpha reads as 1");
    Check(!ChannelOrderTwins(kRgba8, kRgba16F) && !ChannelOrderTwins(kR11G11B10, kRgba16F) &&
              !ChannelOrderTwins(kRgb10A2, kRgba8) && !ChannelOrderTwins(kRgba16F, kRgba32F),
          "HDR and float formats are never twins");

    Check(KeepsHistory(At(kRgba8), At(kBgra8)), "a twin swap at the same extent and transfer keeps history");
    Check(KeepsHistory(At(kBgra8Typeless), At(kRgba8Typeless)), "and the other way, typeless");
    Check(!KeepsHistory(At(kRgba8), At(kRgba8)), "no format change is not this rule's business");
    Check(!KeepsHistory(At(kRgba8), At(kRgba16F)), "a real format change does not");

    Contract other = At(kBgra8);
    other.width = 1920;
    Check(!KeepsHistory(At(kRgba8), other), "frame width moved");
    other = At(kBgra8);
    other.height = 1080;
    Check(!KeepsHistory(At(kRgba8), other), "frame height moved");
    other = At(kBgra8);
    other.workWidth = 1720 * 2;
    Check(!KeepsHistory(At(kRgba8), other), "working width moved");
    other = At(kBgra8);
    other.workHeight = 360;
    Check(!KeepsHistory(At(kRgba8), other), "working height moved");

    Transfer hdr;
    hdr.gameSaysHdr = true;
    Check(!KeepsHistory(At(kRgba8), At(kBgra8, hdr)), "the game's HDR claim changed");
    Check(KeepsHistory(At(kRgba8, hdr), At(kBgra8, hdr)), "the same claim on both sides keeps");
    Transfer white;
    white.presentWhite = 2.5f;
    Check(!KeepsHistory(At(kRgba8), At(kBgra8, white)), "the present white changed");
    Transfer quant;
    quant.presentQuantRel = 1.0f / 1024.0f;
    Check(!KeepsHistory(At(kRgba8, quant), At(kBgra8)), "the output code changed");
    Transfer nan;
    nan.presentWhite = std::numeric_limits<float>::quiet_NaN();
    Check(!KeepsHistory(At(kRgba8, nan), At(kBgra8, nan)), "a NaN never matches");

    if (failures != 0)
        std::printf("%d policy check(s) failed\n", failures);
    return failures == 0 ? 0 : 1;
}

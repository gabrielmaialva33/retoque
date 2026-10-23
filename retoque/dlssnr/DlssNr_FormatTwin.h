#pragma once

// Which colour format changes leave the model's history and the calibration valid
// (docs/dlssnr/design/format-swap-history.md).
//
// Some games hand DLSS their colour as R8G8B8A8 on some frames and B8G8R8A8 on others (Watch Dogs: Legion, reported in
// janblade's fork): the same picture with the red and blue bytes swapped. Every view the pass builds takes the
// resource's own format, typeless translated to its UNORM member, so a shader reads the same values from either, and
// the model's history -- inside feature 18, filled through those views -- describes both. The surfaces still follow
// the format, because a copy from the game's colour needs the same typeless family; that is AltSurfaces' job, and
// this header only decides what the swap may keep.
//
// Kept to the pairs that only swap bytes and read through the same transfer. UNORM against sRGB (the sRGB view
// decodes), typeless against typed (no game is known to do it), BGRX8 (alpha reads as 1) and every HDR or float
// format stay a real change.

namespace DlssNr::FormatTwin
{
// DXGI_FORMAT values, kept as ints so this header needs no Windows headers.
constexpr int kR8G8B8A8Typeless = 27, kR8G8B8A8Unorm = 28, kR8G8B8A8UnormSrgb = 29;
constexpr int kB8G8R8A8Unorm = 87, kB8G8R8A8Typeless = 90, kB8G8R8A8UnormSrgb = 91;

// The form a change detector compares: BGRA8 becomes its RGBA8 twin (typeless, UNORM and sRGB each to their own),
// everything else stays as it is.
constexpr int ChannelOrderCanonical(int format)
{
    switch (format)
    {
    case kB8G8R8A8Typeless:
        return kR8G8B8A8Typeless;
    case kB8G8R8A8Unorm:
        return kR8G8B8A8Unorm;
    case kB8G8R8A8UnormSrgb:
        return kR8G8B8A8UnormSrgb;
    default:
        return format;
    }
}

// Two different formats that only differ in channel order. 0 (UNKNOWN, no surfaces yet) is nobody's twin.
constexpr bool ChannelOrderTwins(int a, int b)
{
    return a != 0 && b != 0 && a != b && ChannelOrderCanonical(a) == ChannelOrderCanonical(b);
}

// What the pass does to the colour beyond reading it: the game's linear-HDR claim (with the format, whether the colour
// transform runs), the present host's HDR white and the code the frame goes back out in. Compared as floats, so a NaN
// on both sides is a change.
struct Transfer
{
    bool gameSaysHdr = false;
    float presentWhite = 0.0f;
    float presentQuantRel = 0.0f;
};

constexpr bool SameTransfer(const Transfer& a, const Transfer& b)
{
    return a.gameSaysHdr == b.gameSaysHdr && a.presentWhite == b.presentWhite && a.presentQuantRel == b.presentQuantRel;
}

// The contract one frame's surfaces were made for: format, frame extent, working extent, transfer.
struct Contract
{
    int format = 0;
    unsigned int width = 0;
    unsigned int height = 0;
    unsigned int workWidth = 0;
    unsigned int workHeight = 0;
    Transfer transfer {};
};

// A format change from `was` to `now` that keeps the model's history and the calibration: channel-order twins, at
// the same extents, through the same transfer. Anything else -- no change at all included -- is not this rule's.
constexpr bool KeepsHistory(const Contract& was, const Contract& now)
{
    return ChannelOrderTwins(was.format, now.format) && was.width == now.width && was.height == now.height &&
           was.workWidth == now.workWidth && was.workHeight == now.workHeight &&
           SameTransfer(was.transfer, now.transfer);
}
} // namespace DlssNr::FormatTwin

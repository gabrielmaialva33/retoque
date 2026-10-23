#pragma once

#include <cstdint>

// The guides the model is handed below its frame size, and the motion-vector scale that goes with
// them. docs/dlssnr/design/reduced-scale-guides.md.
//
// Both rules are jlrouzies-fr's, found and fixed in v0.8.92 of wilsjo2's
// OptiScaler-DLSSNR-PreSR-Multipass (GPL-3.0): this tree's reduced path had the same shape that
// fork's had, and so the same two faults.
//
//   1. Below native the colour shrinks to the working size and the guides did not. Nothing documents
//      that the model resamples a guide larger than its input, and the picture says it does not: each
//      pixel's depth and motion described some other place, history never lined up, and the frame
//      flickered and kept "settling" after the camera stopped.
//   2. The model reads its motion-vector scale in pixels of the motion texture it is handed. The game's
//      scale turns its vectors into pixels of the size they are measured in: the render size for
//      low-resolution vectors, the output size otherwise. Converting with working / frame shrank every
//      low-resolution vector below native, however the guides were handed over.
//
// Pure functions of sizes and flags, so the host suite can pin them without a device.
namespace DlssNr::GuideMatch
{
// The size the game's motion-vector scale converts its vectors into pixels of.
constexpr uint32_t MotionReference(bool lowResolutionVectors, uint32_t renderExtent, uint32_t outputExtent)
{
    return lowResolutionVectors ? renderExtent : outputExtent;
}

// The scale the model is given: the game's, re-expressed in pixels of the motion texture handed over.
constexpr float ModelMotionScale(float gameScale, uint32_t handedExtent, uint32_t referenceExtent)
{
    return referenceExtent != 0 ? gameScale * (float) handedExtent / (float) referenceExtent : gameScale;
}

// The conversion this tree used before, kept for the A/B key: working size over frame size, whatever
// texture the model was actually reading.
constexpr float LegacyMotionScale(float gameScale, uint32_t workExtent, uint32_t frameExtent)
{
    return frameExtent != 0 ? gameScale * (float) workExtent / (float) frameExtent : gameScale;
}

// Whether depth and motion are resampled to the working size before the model sees them: only when a
// guide is larger than the model's input along some axis. A guide smaller than the input is the
// upscaler's ordinary contract -- render resolution under display resolution -- which the model has
// always taken as a subrect, and a point resample up would only throw information away.
constexpr bool Wanted(bool enabled, bool available, uint32_t workWidth, uint32_t workHeight, uint32_t depthWidth,
                      uint32_t depthHeight, uint32_t motionWidth, uint32_t motionHeight)
{
    if (!enabled || !available || workWidth == 0 || workHeight == 0)
        return false;

    return depthWidth > workWidth || depthHeight > workHeight || motionWidth > workWidth || motionHeight > workHeight;
}

// A guide region as the model is handed it: where it starts in its texture, and its size.
struct Region
{
    uint32_t baseX = 0;
    uint32_t baseY = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

// What the model is handed for depth and motion, and the motion-vector scale that goes with it.
struct Handed
{
    bool matched = false; // the working-size pair the resample wrote, rather than the game's regions
    Region depth;
    Region motion;
    float scaleX = 1.0f;
    float scaleY = 1.0f;
};

// Which guides a route that cannot fall back to frame-size guides hands over (the Vulkan route,
// docs/dlssnr/design/reduced-scale-guides-vk.md): the game's regions when no resample is wanted, the matched pair when
// it is wanted and can run, and otherwise none -- the frame is not run, rather than handed guides that describe some
// other place than its colour.
enum class Guides
{
    Game,
    Matched,
    Refused,
};

constexpr Guides Choose(bool wanted, bool resampleReady)
{
    return !wanted ? Guides::Game : resampleReady ? Guides::Matched : Guides::Refused;
}

// The plan the model gets. Matched: both guides a full zero-origin region at the working size. Otherwise the game's
// regions as they are. The scale is fix 2 for the motion texture handed over, or the legacy working / frame conversion
// with renderMotionScale off: the D3D12 route's arithmetic, in one place.
constexpr Handed ForModel(bool matched, bool renderMotionScale, Region depth, Region motion, uint32_t workWidth,
                          uint32_t workHeight, uint32_t frameWidth, uint32_t frameHeight, uint32_t referenceWidth,
                          uint32_t referenceHeight, float gameScaleX, float gameScaleY)
{
    Handed handed;
    handed.matched = matched;
    handed.depth = matched ? Region { 0, 0, workWidth, workHeight } : depth;
    handed.motion = matched ? Region { 0, 0, workWidth, workHeight } : motion;
    handed.scaleX = renderMotionScale ? ModelMotionScale(gameScaleX, handed.motion.width, referenceWidth)
                                      : LegacyMotionScale(gameScaleX, workWidth, frameWidth);
    handed.scaleY = renderMotionScale ? ModelMotionScale(gameScaleY, handed.motion.height, referenceHeight)
                                      : LegacyMotionScale(gameScaleY, workHeight, frameHeight);
    return handed;
}

// Below native the resample turns the region it is given into a whole working-size guide, so a region guessed from
// the allocation is materialised as if it were the picture. When the frame's render geometry is known to be partial
// (one dimension published) or stale (the creation fallback retired by a newer evaluate), that guess is refused
// rather than resampled (V-04). A frame with no geometry at all keeps the allocation, as D3D12 and this route always
// have. nullptr when the geometry may be used.
constexpr const char* GeometryRefusal(bool incomplete, bool stale)
{
    if (incomplete)
        return "the game's render geometry is incomplete; the allocation's margin may not be the picture";
    if (stale)
        return "the game's render geometry is stale; the allocation's margin may not be the picture";

    return nullptr;
}

// The texel of a guide region a working-size texel reads: its centre mapped into the region, floored.
// The shader computes the same thing; this is the one the tests hold it to.
constexpr uint32_t PointSource(uint32_t destination, uint32_t destinationExtent, uint32_t regionBase,
                               uint32_t regionExtent)
{
    if (destinationExtent == 0 || regionExtent == 0)
        return regionBase;

    const uint64_t scaled = ((uint64_t) destination * 2 + 1) * regionExtent / ((uint64_t) destinationExtent * 2);
    return regionBase + (uint32_t) (scaled < regionExtent ? scaled : regionExtent - 1);
}
} // namespace DlssNr::GuideMatch

// DLSS-NR model cadence: the model runs on one frame in N, and on the frames between this pass moves the edit
// it made onto the current frame. docs/dlssnr/design/model-cadence.md is the reasoning; the per-pixel rules are in
// dlssnr_cadence_rule.h beside this file, shared with the host test tests/nr-cadence, and are taken from
// BeliyG3's optimizer-fps-dlss5 (MIT, Copyright (c) 2026 Yuri Grib; Licenses/OptimizerFps_LICENSE.txt).
//
// One shader, nine modes, one dispatch each, all at the model's working size:
//   0 record        a model frame: the edit (answer - proxy), the proxy and the depth, kept
//   1 coarse        the edit averaged over 16x16 blocks, for the fill
//   2 chain         a carried frame: one link of the displacement back to the model frame
//   3 synthesize    a carried frame: the proxy plus the moved edit (plus the walk's share of the fade, when
//                   smoothing), into the surface the model writes or, with the limiter, into the shown answer
//   4 model motion  the model frame after carried ones: the chain as the model's motion vectors
// Smoothing (docs/dlssnr/design/cadence-smoothing.md), only with [DlssNr] CadenceSmoothing:
//   5 fade          the model frame after carried ones: the carried answer minus the model's (kept), and the
//                   model's answer with the walk's first share of the carried one (the shown answer)
//   6 light         the shown answer's and the proxy's light, summed over a 16x16 grid of tiles, the interface out
//   7 limit         one group: the grid summed, the edit's light held to the last frame's, the gain kept
//   8 apply         the shown answer times the gain (not on the interface), into the surface the resolve reads
// The regional light limiter (docs/dlssnr/design/light-limiter.md), with [DlssNr] LightLimiter=2, on an answer of any
// cadence: the global limiter's three steps over tiles of gRegionTile pixels instead of one picture's light
//  10 region light  the answer's, the proxy's and the motion's sum per tile, the interface out
//  11 region limit  one thread a tile: that tile's edit light held to its last frame's, the gain kept
//  12 region apply  the answer times the tile gains, bilinear between tile centres (not on the interface)
//
// Precompiled like dlssnr.hlsl: editing this or the rule alone changes nothing. From this directory (dxc under
// the msvc-wine prefix on Linux, never while a build is using it):
//   dxc.exe -T cs_6_0 -E CSMain -O3 -Qstrip_debug -Qstrip_reflect dlssnr_cadence.hlsl -Fo DlssNr_Cadence_Shader.cso
//   python3 ../../../../OptiScaler/shaders/shader_tools/create_header.py DlssNr_Cadence_Shader.cso DlssNr_Cadence_Shader.h DlssNr_Cadence_cso
// DlssNr_Cadence_Dx12.cpp picks the header up through __has_include; without it the cadence reports itself
// unavailable and the model runs every frame.

cbuffer Params : register(b0)
{
    uint  gMode;
    uint  gWidth;       // the working size
    uint  gHeight;
    uint  gLowWidth;    // the coarse edit's size
    uint  gLowHeight;
    uint  gLowBlock;    // working pixels per coarse texel
    uint  gChainStart;  // 1 on the first carried frame after a model frame: no chain to extend yet
    uint  gHaveUi;      // 1 when t9 is this frame's UI alpha
    uint  gDepthBaseX;  // the region of t2 that holds the depth, mapped point-wise onto the working size
    uint  gDepthBaseY;
    uint  gDepthWidth;
    uint  gDepthHeight;
    uint  gMotionBaseX; // the same for t3's motion
    uint  gMotionBaseY;
    uint  gMotionWidth;
    uint  gMotionHeight;
    float gMvToWorkX;   // the game's vectors into working pixels
    float gMvToWorkY;
    float gDepthTol;
    float gColourTol;
    float gScale;       // working size / frame size, clamped
    float gDepthStepX;  // the depth region's texel spacing, in working pixels
    float gDepthStepY;
    float gFadeWeight;  // fade and synthesize: the share of the previous answer still shown (0: none, t10 unread)
    uint  gPassthrough; // light and apply: the frame is already tone mapped, so its values are its light
    uint  gLimitValid;  // limit: the last frame was limited, so the state holds something to hold this one to
    float gLimitStep;   // limit: how far the edit's light may move this frame, in stops
    float gLimitInputTol; // limit: a step of the frame's own light, in stops, that lets the limit go
    float gLimitMaxGain;  // limit: the most the gain may move a frame's light, in stops
    uint gProbeHistory; // adaptive probe only: a valid previous model edit exists
    uint  gRegionTile;          // modes 10-12: working pixels per tile edge
    uint  gRegionTilesX;        // the tile grid
    uint  gRegionTilesY;
    uint  gHaveMotion;          // mode 10: t3 is this frame's motion
    float gRegionStep;          // mode 11: how far a tile's edit light may move this frame, in stops
    float gRegionReleaseLow;    // mode 11: the drift, in stops, where the hold starts to let go ...
    float gRegionReleaseHigh;   // ... and where it has let go
    float gRegionMotionPixels;  // mode 11: the tile's mean motion, in working pixels, at which it is not held
};

Texture2D<float4>   gAnswer      : register(t0); // record: the model's final answer
Texture2D<float4>   gProxy       : register(t1); // this frame's proxy, the model's input
Texture2D<float>    gDepth       : register(t2); // this frame's depth, read through its region
Texture2D<float2>   gMotion      : register(t3); // this frame's motion, read through its region
Texture2D<float2>   gChain       : register(t4); // chain/model motion: the previous chain. synthesize: this frame's
Texture2D<float>    gDepthThen   : register(t5); // the depth snapshot of the model frame
Texture2D<float4>   gProxyThen   : register(t6); // the proxy snapshot of the model frame
Texture2D<float4>   gResidual    : register(t7); // the stored edit
Texture2D<float4>   gResidualLow : register(t8); // the coarse edit
Texture2D<float4>   gUi          : register(t9); // the UI alpha (.x), when gHaveUi
Texture2D<float4>   gFade        : register(t10); // synthesize: the stored fade, when gFadeWeight > 0
RWTexture2D<float4> gOut0        : register(u0); // record: edit. coarse: coarse edit. chain/motion: chain. synthesize, fade:
                                                 // answer. light: the light grid. apply: the answer the resolve reads
RWTexture2D<float4> gOut1        : register(u1); // record: proxy snapshot. fade: the fade
RWTexture2D<float>  gOut2        : register(u2); // record: depth snapshot. limit, apply: the limiter's state, 4x1
// gAnswer (t0) is the model's answer in record and fade, the shown answer in light and apply, the light grid in limit.

// The texel of a region a working texel reads: its centre mapped into the region, floored -- the guide
// resample's rule (DlssNr::GuideMatch::PointSource).
uint PointSource(uint destination, uint destinationExtent, uint regionBase, uint regionExtent)
{
    const uint scaled = ((destination * 2u + 1u) * regionExtent) / (destinationExtent * 2u);
    return regionBase + min(scaled, regionExtent - 1u);
}

int2 Clamped(int tx, int ty) { return int2(clamp(tx, 0, int(gWidth) - 1), clamp(ty, 0, int(gHeight) - 1)); }

float DepthNowAt(int tx, int ty)
{
    const int2 p = Clamped(tx, ty);
    return gDepth.Load(int3(PointSource(uint(p.x), gWidth, gDepthBaseX, gDepthWidth),
                            PointSource(uint(p.y), gHeight, gDepthBaseY, gDepthHeight), 0));
}

float2 MotionAt(int tx, int ty)
{
    const int2 p = Clamped(tx, ty);
    const float2 raw = gMotion.Load(int3(PointSource(uint(p.x), gWidth, gMotionBaseX, gMotionWidth),
                                         PointSource(uint(p.y), gHeight, gMotionBaseY, gMotionHeight), 0));
    return raw * float2(gMvToWorkX, gMvToWorkY);
}

float  DepthThenAt(int tx, int ty) { return gDepthThen.Load(int3(Clamped(tx, ty), 0)); }
float3 ProxyNowAt(int tx, int ty) { return gProxy.Load(int3(Clamped(tx, ty), 0)).rgb; }
float3 ProxyThenAt(int tx, int ty) { return gProxyThen.Load(int3(Clamped(tx, ty), 0)).rgb; }
float2 ChainAt(int tx, int ty) { return gChain.Load(int3(Clamped(tx, ty), 0)); }
float3 ResidualAt(int tx, int ty) { return gResidual.Load(int3(Clamped(tx, ty), 0)).rgb; }
float  UiAt(int tx, int ty) { return gUi.Load(int3(Clamped(tx, ty), 0)).x; }

float3 ResidualLowAt(int tx, int ty)
{
    return gResidualLow.Load(int3(clamp(tx, 0, int(gLowWidth) - 1), clamp(ty, 0, int(gLowHeight) - 1), 0)).rgb;
}

float3 FadeAt(int tx, int ty) { return gFade.Load(int3(Clamped(tx, ty), 0)).rgb; }

#define CR_FN
#define CR_UNROLL [unroll]
#define CR_LOOP [loop]
#define CR_DEPTH_NOW(ax_, ay_) DepthNowAt(ax_, ay_)
#define CR_DEPTH_THEN(ax_, ay_) DepthThenAt(ax_, ay_)
#define CR_PROXY_NOW(ax_, ay_) ProxyNowAt(ax_, ay_)
#define CR_PROXY_THEN(ax_, ay_) ProxyThenAt(ax_, ay_)
#define CR_MOTION(ax_, ay_) MotionAt(ax_, ay_)
#define CR_CHAIN_PREV(ax_, ay_) ChainAt(ax_, ay_)
#define CR_RESIDUAL(ax_, ay_) ResidualAt(ax_, ay_)
#define CR_RESIDUAL_LOW(ax_, ay_) ResidualLowAt(ax_, ay_)
#define CR_UI(ax_, ay_) UiAt(ax_, ay_)
#define CR_FADE(ax_, ay_) FadeAt(ax_, ay_)
#include "dlssnr_cadence_rule.h"

// The light limiter's grid (DlssNr::Cadence::kLightGrid) and its state, four R32F texels in u2: the edit's light shown
// last frame, the frame's own light last frame, whether they hold anything, and this frame's gain -- all in stops.
static const uint kLightGrid = 16;
groupshared float2 gLightSum[64];

CadenceParams LoadParams()
{
    CadenceParams P;
    P.width = int(gWidth);
    P.height = int(gHeight);
    P.lowWidth = int(gLowWidth);
    P.lowHeight = int(gLowHeight);
    P.lowBlock = int(gLowBlock);
    P.haveUi = int(gHaveUi);
    P.depthTol = gDepthTol;
    P.colourTol = gColourTol;
    P.scale = gScale;
    P.depthStepX = gDepthStepX;
    P.depthStepY = gDepthStepY;
    return P;
}

void PixelModes(uint3 id)
{
    const CadenceParams P = LoadParams();

    // Sparse adaptive feedback, including model frames: refusal must not latch by stopping its own probe.
    if (gMode == 9)
    {
        if (id.x >= 16 || id.y >= 16) return;
        const int x = int(((2 * id.x + 1) * gWidth) / 32);
        const int y = int(((2 * id.y + 1) * gHeight) / 32);
        const float d = DepthNowAt(x, y);
        const float2 motion = MotionAt(x, y);
        float4 sample = 0.0;
        if (!isfinite(d) || !CadenceFinite2(motion)) sample.z = -1.0;
        else if (d > 0.0 && d < 1.0 && (gHaveUi == 0 || UiAt(x, y) < 0.5))
        {
            sample.z = -1.0;
            if (gProbeHistory != 0)
            {
                const float2 acc = CadenceChainStep(x, y, gChainStart != 0, P);
                const CadenceAcceptance accepted = CadenceAccept(x, y, acc, P);
                sample = float4(length(motion) * (1080.0 / float(gHeight)), 1.0 - accepted.weight, 1.0, 0.0);
            }
        }
        gOut0[id.xy] = sample;
        return;
    }

    if (gMode == 1)
    {
        if (id.x >= gLowWidth || id.y >= gLowHeight)
            return;
        gOut0[id.xy] = float4(CadenceCoarse(int(id.x), int(id.y), P), 0.0);
        return;
    }

    if (id.x >= gWidth || id.y >= gHeight)
        return;

    const int px = int(id.x);
    const int py = int(id.y);

    if (gMode == 0)
    {
        const float3 proxy = ProxyNowAt(px, py);
        gOut0[id.xy] = float4(CadenceResidual(gAnswer.Load(int3(id.xy, 0)).rgb, proxy), 0.0);
        gOut1[id.xy] = float4(proxy, 1.0);
        gOut2[id.xy] = DepthNowAt(px, py);
        return;
    }

    if (gMode == 2)
    {
        gOut0[id.xy] = float4(CadenceChainStep(px, py, gChainStart != 0, P), 0.0, 0.0);
        return;
    }

    if (gMode == 3)
    {
        // The alpha the model would return is the frame's own; the resolve reads colour only. At a weight of zero
        // this is CadenceSynthesize exactly, and the fade is not read.
        const float alpha = gProxy.Load(int3(id.xy, 0)).a;
        gOut0[id.xy] = float4(CadenceSynthesizeFaded(px, py, ChainAt(px, py), gFadeWeight, P), alpha);
        return;
    }

    if (gMode == 4)
    {
        const float2 acc = CadenceChainStep(px, py, false, P);
        gOut0[id.xy] = float4(CadenceModelMotion(px, py, acc, P), 0.0, 0.0);
        return;
    }

    if (gMode == 5)
    {
        // The same last link mode 4 computed for the model's vectors, from the same chain: the carried answer here
        // is what a carried frame would have shown.
        const float2 acc = CadenceChainStep(px, py, false, P);
        const float4 answer = gAnswer.Load(int3(id.xy, 0));
        const float3 fade = CadenceFade(px, py, acc, answer.rgb, P);
        gOut0[id.xy] = float4(CadenceFadeShown(answer.rgb, fade, gFadeWeight), answer.a);
        gOut1[id.xy] = float4(fade, 0.0);
        return;
    }

        if (gMode == 8)
    {
        // A pixel taken for interface keeps its answer as it is: the gain holds back the scenery's edit, not the HUD.
        const float4 shown = gAnswer.Load(int3(id.xy, 0));
        const float keep = gHaveUi != 0u ? 1.0 - saturate(UiAt(px, py)) : 1.0;
        gOut0[id.xy] = float4(CadenceApplyGain(shown.rgb, gOut2[uint2(3, 0)] * keep, int(gPassthrough)), shown.a);
        return;
    }
}

// The light limiter's two group-wide passes. Separate from CSMain's per-pixel early returns, which are not uniform
// across a group, because both synchronise the group.
void LightGrid(uint3 groupId, uint3 groupThreadId)
{
    const uint lane = groupThreadId.y * 8u + groupThreadId.x;
    const uint x0 = (groupId.x * gWidth) / kLightGrid;
    const uint x1 = min(max(((groupId.x + 1u) * gWidth) / kLightGrid, x0 + 1u), gWidth);
    const uint y0 = (groupId.y * gHeight) / kLightGrid;
    const uint y1 = min(max(((groupId.y + 1u) * gHeight) / kLightGrid, y0 + 1u), gHeight);

    // Every second pixel each way: a mean, and a quarter of the pixels is plenty for one.
    float2 sum = 0.0;
    [loop] for (uint y = y0 + groupThreadId.y * 2u; y < y1; y += 16u)
    {
        [loop] for (uint x = x0 + groupThreadId.x * 2u; x < x1; x += 16u)
        {
                        // The interface is left out of the light, as the gain leaves it alone.
            const int3 p = int3(x, y, 0);
            const float scenery = gHaveUi != 0u ? 1.0 - saturate(gUi.Load(p).x) : 1.0;
            sum += scenery * float2(CadenceLight(gAnswer.Load(p).rgb, int(gPassthrough)),
                                    CadenceLight(gProxy.Load(p).rgb, int(gPassthrough)));
        }
    }

    gLightSum[lane] = sum;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint stride = 32u; stride > 0u; stride >>= 1u)
    {
        if (lane < stride)
            gLightSum[lane] += gLightSum[lane + stride];
        GroupMemoryBarrierWithGroupSync();
    }

    if (lane == 0u)
        gOut0[groupId.xy] = float4(gLightSum[0], 0.0, 0.0);
}

// The regional limiter's group-wide pass: one group a tile, every second pixel each way as the global grid does, summing
// the answer's light, the proxy's light, the motion's length and the weight (the interface out) for mode 11.
groupshared float4 gRegionSum[64];

void RegionGrid(uint3 groupId, uint3 groupThreadId)
{
    const uint lane = groupThreadId.y * 8u + groupThreadId.x;
    const uint x0 = groupId.x * gRegionTile;
    const uint x1 = min(x0 + gRegionTile, gWidth);
    const uint y0 = groupId.y * gRegionTile;
    const uint y1 = min(y0 + gRegionTile, gHeight);

    float4 sum = 0.0;
    [loop] for (uint y = y0 + groupThreadId.y * 2u; y < y1; y += 16u)
    {
        [loop] for (uint x = x0 + groupThreadId.x * 2u; x < x1; x += 16u)
        {
            const int3 p = int3(x, y, 0);
            const float scenery = gHaveUi != 0u ? 1.0 - saturate(gUi.Load(p).x) : 1.0;
            float speed = 0.0;
            if (gHaveMotion != 0u)
            {
                const float2 motion = MotionAt(int(x), int(y));
                speed = CadenceFinite2(motion) ? length(motion) : 0.0;
            }
            sum += scenery * float4(CadenceLight(gAnswer.Load(p).rgb, int(gPassthrough)),
                                    CadenceLight(gProxy.Load(p).rgb, int(gPassthrough)), speed, 1.0);
        }
    }

    gRegionSum[lane] = sum;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint stride = 32u; stride > 0u; stride >>= 1u)
    {
        if (lane < stride)
            gRegionSum[lane] += gRegionSum[lane + stride];
        GroupMemoryBarrierWithGroupSync();
    }

    if (lane == 0u)
        gOut0[groupId.xy] = gRegionSum[0];
}

void LightLimit(uint3 groupThreadId)
{
    const uint lane = groupThreadId.y * 8u + groupThreadId.x;
    float2 sum = 0.0;
    [unroll] for (uint k = 0u; k < (kLightGrid * kLightGrid) / 64u; ++k)
    {
        const uint tile = lane + k * 64u;
        sum += gAnswer.Load(int3(tile % kLightGrid, tile / kLightGrid, 0)).xy;
    }

    gLightSum[lane] = sum;
    GroupMemoryBarrierWithGroupSync();
    [unroll] for (uint stride = 32u; stride > 0u; stride >>= 1u)
    {
        if (lane < stride)
            gLightSum[lane] += gLightSum[lane + stride];
        GroupMemoryBarrierWithGroupSync();
    }

    if (lane == 0u)
    {
        const float shownSum = gLightSum[0].x;
        const float proxySum = gLightSum[0].y;
        const bool measurable = shownSum > 0.0 && proxySum > 0.0;
        const float raw = measurable ? log2(shownSum / proxySum) : 0.0;
        const float input = measurable ? log2(proxySum) : 0.0;
        const int valid = gLimitValid != 0u && gOut2[uint2(2, 0)] > 0.5 ? 1 : 0;
        const CadenceLimit limit = CadenceLimitLight(raw, input, gOut2[uint2(0, 0)], gOut2[uint2(1, 0)],
                                                     measurable ? valid : 0, gLimitStep, gLimitInputTol, gLimitMaxGain);
        gOut2[uint2(0, 0)] = limit.shown;
        gOut2[uint2(1, 0)] = input;
        gOut2[uint2(2, 0)] = measurable ? 1.0 : 0.0;
        gOut2[uint2(3, 0)] = limit.gainStops;
    }
}

// The regional limiter's per-tile step: one thread a tile. State: four R32F texels a tile in u2, at x = 4 * tile: the edit's
// light shown last frame, the tile's own light last frame, whether they hold anything, and this frame's gain -- all in stops.
void RegionLimit(uint3 id)
{
    if (id.x >= gRegionTilesX || id.y >= gRegionTilesY)
        return;
    const float4 tile = gAnswer.Load(int3(id.xy, 0));
    const bool measurable = tile.x > 0.0 && tile.y > 0.0 && tile.w > 0.0;
    const float raw = measurable ? log2(tile.x / tile.y) : 0.0;
    const float input = measurable ? log2(tile.y / tile.w) : 0.0;
    const float motion = measurable ? tile.z / tile.w : 0.0;
    const uint s = id.x * 4u;
    const int valid = measurable && gLimitValid != 0u && gOut2[uint2(s + 2u, id.y)] > 0.5 ? 1 : 0;
    const CadenceLimit limit = CadenceLimitRegion(raw, input, gOut2[uint2(s, id.y)], gOut2[uint2(s + 1u, id.y)], valid,
                                                  gRegionStep, gLimitInputTol, gRegionReleaseLow, gRegionReleaseHigh,
                                                  motion, gRegionMotionPixels, gLimitMaxGain);
    gOut2[uint2(s, id.y)] = limit.shown;
    gOut2[uint2(s + 1u, id.y)] = input;
    gOut2[uint2(s + 2u, id.y)] = measurable ? 1.0 : 0.0;
    gOut2[uint2(s + 3u, id.y)] = limit.gainStops;
}

// The tile gains, bilinear between tile centres, so the band the limiter moves is a tile wide and what is finer than that
// -- the detail -- passes with the answer's own light. A pixel taken for interface keeps its answer.
void RegionApply(uint3 id)
{
    if (id.x >= gWidth || id.y >= gHeight)
        return;
    const int px = int(id.x);
    const int py = int(id.y);
    const float4 shown = gAnswer.Load(int3(id.xy, 0));
    const float keep = gHaveUi != 0u ? 1.0 - saturate(UiAt(px, py)) : 1.0;
    const float2 centre = (float2(id.xy) + 0.5) / float(gRegionTile) - 0.5;
    const float2 base = floor(centre);
    const float2 weight = centre - base;
    const int lastX = int(gRegionTilesX) - 1;
    const int lastY = int(gRegionTilesY) - 1;
    const int x0 = clamp(int(base.x), 0, lastX);
    const int x1 = clamp(int(base.x) + 1, 0, lastX);
    const int y0 = clamp(int(base.y), 0, lastY);
    const int y1 = clamp(int(base.y) + 1, 0, lastY);
    const float g00 = gOut2[uint2(uint(x0) * 4u + 3u, uint(y0))];
    const float g10 = gOut2[uint2(uint(x1) * 4u + 3u, uint(y0))];
    const float g01 = gOut2[uint2(uint(x0) * 4u + 3u, uint(y1))];
    const float g11 = gOut2[uint2(uint(x1) * 4u + 3u, uint(y1))];
    const float gain = lerp(lerp(g00, g10, weight.x), lerp(g01, g11, weight.x), weight.y);
    gOut0[id.xy] = float4(CadenceApplyGain(shown.rgb, (isfinite(gain) ? gain : 0.0) * keep, int(gPassthrough)), shown.a);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID, uint3 groupId : SV_GroupID, uint3 groupThreadId : SV_GroupThreadID)
{
    // Uniform across a dispatch, so a whole group takes the same branch and the barriers in these are safe.
    if (gMode == 6)
    {
        if (groupId.x < kLightGrid && groupId.y < kLightGrid)
            LightGrid(groupId, groupThreadId);
        return;
    }
    if (gMode == 7)
    {
        if (groupId.x == 0u && groupId.y == 0u)
            LightLimit(groupThreadId);
        return;
    }
    if (gMode == 10)
    {
        if (groupId.x < gRegionTilesX && groupId.y < gRegionTilesY)
            RegionGrid(groupId, groupThreadId);
        return;
    }
    if (gMode == 11)
    {
        RegionLimit(id);
        return;
    }
    if (gMode == 12)
    {
        RegionApply(id);
        return;
    }
    PixelModes(id);
}

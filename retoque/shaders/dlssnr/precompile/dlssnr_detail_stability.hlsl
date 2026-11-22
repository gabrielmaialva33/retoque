// DLSS-NR detail stability: a temporal filter on the model's edit in the log domain, clipped to the current 3x3 and rejected
// where the input moved under it (docs/dlssnr/design/detail-stability.md). One dispatch at the model's working size, with
// [DlssNr] DetailStability on. The per-pixel rule is in dlssnr_detail_stability_rule.h beside this file, shared with the host
// test tests/nr-detail-stability; it is DLSS5-NR-Nexus's "Detail stability" filter (williamstosh, MIT,
// Licenses/NRNexus_LICENSE.txt), the near binary rejection band of A-ENTROPY's note and DLSS5-Feeder's exact repeat
// (jlrouzies-fr, MIT, Licenses/DLSS5-Feeder_LICENSE.txt).
//
// Precompiled like dlssnr.hlsl: editing this or the rule alone changes nothing. From this directory (dxc under the
// msvc-wine prefix on Linux, never while a build is using it):
//   dxc.exe -T cs_6_0 -E CSMain -O3 -Qstrip_debug -Qstrip_reflect dlssnr_detail_stability.hlsl -Fo DlssNr_DetailStability_Shader.cso
//   python ../../../../OptiScaler/shaders/shader_tools/create_header.py DlssNr_DetailStability_Shader.cso DlssNr_DetailStability_Shader.h DlssNr_DetailStability_cso

cbuffer Params : register(b0)
{
    uint  gWidth;        // the working size
    uint  gHeight;
    uint  gHistoryValid; // 0 after a reset, a gap or a size change: nothing to filter with
    uint  gHoldRepeats;
    uint  gHaveUi;       // 1 when t4 is this frame's UI alpha
    uint  gMotionBaseX;  // the region of t2 that holds the motion, mapped point-wise onto the working size
    uint  gMotionBaseY;
    uint  gMotionWidth;
    uint  gMotionHeight;
    float gMvToWorkX;    // the game's vectors into working pixels
    float gMvToWorkY;
    float gAlpha;        // weight of the current frame per frame
    float gBandLow;
    float gBandHigh;
};

Texture2D<float4>   gProxy   : register(t0); // the model's input
Texture2D<float4>   gAnswer  : register(t1); // the model's answer
Texture2D<float2>   gMotion  : register(t2); // the game's motion, read through its region
Texture2D<float4>   gHistory : register(t3); // last frame's history: .xyz filtered edit, .w input (below zero: nothing)
Texture2D<float4>   gUi      : register(t4); // the UI alpha (.x), when gHaveUi
RWTexture2D<float4> gOut     : register(u0); // the answer the resolve reads
RWTexture2D<float4> gHistoryOut : register(u1); // this frame's history

// The texel of a region a working texel reads: its centre mapped into the region, floored (the cadence's and the guide
// resample's rule).
uint PointSource(uint destination, uint destinationExtent, uint regionBase, uint regionExtent)
{
    const uint scaled = ((destination * 2u + 1u) * regionExtent) / (destinationExtent * 2u);
    return regionBase + min(scaled, regionExtent - 1u);
}

int2 Clamped(int tx, int ty) { return int2(clamp(tx, 0, int(gWidth) - 1), clamp(ty, 0, int(gHeight) - 1)); }

float3 ProxyAt(int tx, int ty) { return gProxy.Load(int3(Clamped(tx, ty), 0)).rgb; }
float3 AnswerAt(int tx, int ty) { return gAnswer.Load(int3(Clamped(tx, ty), 0)).rgb; }
float4 HistoryAt(int tx, int ty) { return gHistory.Load(int3(Clamped(tx, ty), 0)); }
float  UiAt(int tx, int ty) { return gUi.Load(int3(Clamped(tx, ty), 0)).x; }

float2 MotionAt(int tx, int ty)
{
    const int2 p = Clamped(tx, ty);
    const float2 raw = gMotion.Load(int3(PointSource(uint(p.x), gWidth, gMotionBaseX, gMotionWidth),
                                         PointSource(uint(p.y), gHeight, gMotionBaseY, gMotionHeight), 0));
    return raw * float2(gMvToWorkX, gMvToWorkY);
}

float Round16(float v) { return f16tof32(f32tof16(v)); }

#define DS_FN
#define DS_UNROLL [unroll]
#define DS_PROXY(ax_, ay_) ProxyAt(ax_, ay_)
#define DS_ANSWER(ax_, ay_) AnswerAt(ax_, ay_)
#define DS_MOTION(ax_, ay_) MotionAt(ax_, ay_)
#define DS_HISTORY(ax_, ay_) HistoryAt(ax_, ay_)
#define DS_UI(ax_, ay_) UiAt(ax_, ay_)
#define DS_ROUND16(v_) Round16(v_)
#include "dlssnr_detail_stability_rule.h"

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gWidth || id.y >= gHeight)
        return;

    DsParams P;
    P.width = int(gWidth);
    P.height = int(gHeight);
    P.alpha = gAlpha;
    P.holdRepeats = int(gHoldRepeats);
    P.historyValid = int(gHistoryValid);
    P.haveUi = int(gHaveUi);
    P.bandLow = gBandLow;
    P.bandHigh = gBandHigh;

    const DsResult result = DsFilter(int(id.x), int(id.y), P);
    // The alpha the model would return is the frame's own; the resolve reads colour only.
    gOut[id.xy] = float4(result.answer, gAnswer.Load(int3(id.xy, 0)).a);
    gHistoryOut[id.xy] = result.history;
}

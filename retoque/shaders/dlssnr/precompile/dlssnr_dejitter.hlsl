// Render-resolution motion-vector de-jitter pass for post-upscale DLSS-NR
// (docs/dlssnr/design/2026-11-22-dejitter-motion-vectors.md).
// Adapted from kkyleeb21's OptiScaler_DLSSNR D18 research branch (GPL-3.0).
//
// Build (from retoque/shaders/dlssnr/precompile, dxc under the msvc-wine prefix on Linux):
//   dxc -T cs_6_0 -E CSMain -O3 -Qstrip_debug -Qstrip_reflect -Fo DlssNr_Dejitter_Shader.cso dlssnr_dejitter.hlsl
//   python3 ../../../../OptiScaler/shaders/shader_tools/create_header.py DlssNr_Dejitter_Shader.cso DlssNr_Dejitter_Shader.h DlssNr_Dejitter_cso

cbuffer Params : register(b0)
{
    uint gBaseX;
    uint gBaseY;
    uint gWidth;
    uint gHeight;
    float gDeltaMvX;
    float gDeltaMvY;
    uint gPad0;
    uint gPad1;
};

Texture2D<float2> gMotionIn : register(t0);
RWTexture2D<float2> gMotionOut : register(u0);

#define DJ_FN
#include "dlssnr_dejitter_rule.h"
#undef DJ_FN

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gWidth || id.y >= gHeight)
        return;

    const uint2 at = uint2(gBaseX + id.x, gBaseY + id.y);
    const float2 mv = gMotionIn.Load(int3(at, 0));
    gMotionOut[at] = ApplyMotionDejitter(mv, float2(gDeltaMvX, gDeltaMvY));
}

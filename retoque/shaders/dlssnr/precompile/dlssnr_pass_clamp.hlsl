// DLSS-NR interpass clamp and PassFeedback hand-off between sequential model passes (D3D12).
// docs/dlssnr/design/2026-11-22-interpass-clamp.md; per-pixel math in dlssnr_pass_clamp_rule.h.
//
// Build (from retoque/shaders/dlssnr/precompile, dxc under the msvc-wine prefix lock on Linux):
//   dxc -T cs_6_0 -E CSMain -O3 -Qstrip_debug -Qstrip_reflect -Fo DlssNr_PassClamp_Shader.cso dlssnr_pass_clamp.hlsl
//   python3 ../../../../OptiScaler/shaders/shader_tools/create_header.py DlssNr_PassClamp_Shader.cso DlssNr_PassClamp_Shader.h DlssNr_PassClamp_cso

Texture2D<float4> gRaw : register(t0);
Texture2D<float4> gProxy : register(t1);
RWTexture2D<float4> gOut : register(u0);

cbuffer Params : register(b0)
{
    uint gWidth;
    uint gHeight;
    uint gClampEnabled;
    float gPassFeedback;
};

#define PC_FN
#define PC_UNROLL [unroll]
#include "dlssnr_pass_clamp_rule.h"

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gWidth || id.y >= gHeight)
        return;

    const float4 raw = gRaw.Load(int3(id.xy, 0));
    const float4 proxy = gProxy.Load(int3(id.xy, 0));
    const float3 handed = PassClampStep(raw.rgb, proxy.rgb, gClampEnabled, gPassFeedback);
    const float alpha = isfinite(raw.a) ? raw.a : (isfinite(proxy.a) ? proxy.a : 1.0);
    gOut[id.xy] = float4(handed, alpha);
}

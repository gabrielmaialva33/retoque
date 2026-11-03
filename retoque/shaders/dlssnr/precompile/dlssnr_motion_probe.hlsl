// Zero-motion detection (DlssNr_MotionProbe.h, docs/dlssnr/design/zero-motion-detection.md): a sparse look at the
// game's depth and motion vectors, read back on the CPU, which decides whether the vectors are a field nobody writes.
//
// One thread per cell of a 32x32 grid over the game's region of each guide. Per cell: the depth at its centre, that
// depth's largest step to a direct neighbour (one pixel of slope there, which is what TAA jitter can move it by), and
// how many of a 4x4 lattice of motion samples are not exactly zero. A NaN is not zero: !(x == 0) is true for it.
//
// Separate from dlssnr.hlsl and dlssnr_guides.hlsl so neither the composition nor the resample changes for an
// instrument. Every read is a Load inside the region the host computed; nothing is written but the grid.
//
// Precompiled like dlssnr.hlsl: editing this alone changes nothing. From this directory (dxc under the msvc-wine
// prefix on Linux, never while a build is using it):
//   dxc.exe -T cs_6_0 -E CSMain -O3 -Qstrip_debug -Qstrip_reflect dlssnr_motion_probe.hlsl -Fo DlssNr_MotionProbe_Shader.cso
//   python3 ../../../../OptiScaler/shaders/shader_tools/create_header.py DlssNr_MotionProbe_Shader.cso DlssNr_MotionProbe_Shader.h DlssNr_MotionProbe_cso
// DlssNr_MotionProbe_Dx12.cpp picks the header up through __has_include; without it the probe reports itself
// unavailable and no verdict ever forms.
//
// Also built as SPIR-V for the Vulkan after-upscale route (DlssNr_MotionProbe_Vk.cpp, which includes its header through
// __has_include as well; docs/dlssnr/design/zero-motion-detection.md, "Vulkan"):
//   dxc.exe -spirv -T cs_6_0 -E CSMain -O3 -Qstrip_debug -D VK_MODE dlssnr_motion_probe.hlsl -Fo DlssNr_MotionProbe_Shader_Vk.spv
//   python3 ../../../../OptiScaler/shaders/shader_tools/create_header.py DlssNr_MotionProbe_Shader_Vk.spv DlssNr_MotionProbe_Shader_Vk.h dlssnr_motion_probe_spv
// Everything VK_MODE adds is under it, so the DXIL above is the same bytes with or without it. Under VK_MODE the
// constants are a push-constant block (DlssNr::MotionProbe::Vk::Push: recorded into the command buffer, nothing a host
// write can race), the reads are sampled images at bindings 0 and 1 of set 0, the grid a storage image at 2.

#ifdef VK_MODE
// The D3D12 cbuffer's eight scalars, in its order, as one push-constant block of 32 bytes.
struct ProbeParams
{
    uint DepthBaseX;
    uint DepthBaseY;
    uint DepthWidth;
    uint DepthHeight;
    uint MotionBaseX;
    uint MotionBaseY;
    uint MotionWidth;
    uint MotionHeight;
};

[[vk::push_constant]] ProbeParams gParams;

#define gDepthBaseX gParams.DepthBaseX
#define gDepthBaseY gParams.DepthBaseY
#define gDepthWidth gParams.DepthWidth
#define gDepthHeight gParams.DepthHeight
#define gMotionBaseX gParams.MotionBaseX
#define gMotionBaseY gParams.MotionBaseY
#define gMotionWidth gParams.MotionWidth
#define gMotionHeight gParams.MotionHeight

[[vk::binding(0, 0)]] Texture2D<float> gDepth;
[[vk::binding(1, 0)]] Texture2D<float2> gMotion;
[[vk::binding(2, 0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> gGrid; // as the D3D12 u0 below
#else
cbuffer Params : register(b0)
{
    uint gDepthBaseX;
    uint gDepthBaseY;
    uint gDepthWidth;
    uint gDepthHeight;
    uint gMotionBaseX;
    uint gMotionBaseY;
    uint gMotionWidth;
    uint gMotionHeight;
};

Texture2D<float>    gDepth  : register(t0);
Texture2D<float2>   gMotion : register(t1);
RWTexture2D<float4> gGrid   : register(u0); // per cell: (depth, depth slope, non-zero motion samples, motion samples)
#endif

static const uint kGrid = 32;
static const uint kMotionTaps = 4;

// The same integer expressions as DlssNr::MotionProbe::Centre and TapAt, which the host tests pin.
uint Centre(uint cell, uint extent) { return ((2u * cell + 1u) * extent) / (2u * kGrid); }
uint TapAt(uint cell, uint tap, uint extent)
{
    return (((cell * kMotionTaps + tap) * 2u + 1u) * extent) / (2u * kGrid * kMotionTaps);
}

// Depth inside the region, the edge repeated: a neighbour outside it is not this frame's scene.
float DepthAt(int x, int y)
{
    const int cx = clamp(x, 0, int(gDepthWidth) - 1);
    const int cy = clamp(y, 0, int(gDepthHeight) - 1);
    return gDepth.Load(int3(int(gDepthBaseX) + cx, int(gDepthBaseY) + cy, 0));
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= kGrid || id.y >= kGrid)
        return;

    const int x = int(Centre(id.x, gDepthWidth));
    const int y = int(Centre(id.y, gDepthHeight));
    const float d = DepthAt(x, y);
    const float slope = max(max(abs(DepthAt(x - 1, y) - d), abs(DepthAt(x + 1, y) - d)),
                            max(abs(DepthAt(x, y - 1) - d), abs(DepthAt(x, y + 1) - d)));

    uint moving = 0u;
    [unroll] for (uint ty = 0u; ty < kMotionTaps; ++ty)
    {
        [unroll] for (uint tx = 0u; tx < kMotionTaps; ++tx)
        {
            const float2 mv = gMotion.Load(int3(int(gMotionBaseX + TapAt(id.x, tx, gMotionWidth)),
                                                int(gMotionBaseY + TapAt(id.y, ty, gMotionHeight)), 0));
            if (!(mv.x == 0.0 && mv.y == 0.0))
                ++moving;
        }
    }

    gGrid[id.xy] = float4(d, slope, float(moving), float(kMotionTaps * kMotionTaps));
}

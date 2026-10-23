// Depth and motion vectors, point-resampled from the upscaler's region to the NR model's working size.
// docs/dlssnr/design/reduced-scale-guides.md.
//
// Point, not linear: a depth between two surfaces is a surface that does not exist, and a vector
// averaged across an edge belongs to neither side. The vectors keep the game's units; the host
// re-expresses the motion-vector scale for this texture (DlssNr_GuideMatch.h).
//
// The texel read is the destination texel's centre mapped into the region, floored -- the same integer
// expression as DlssNr::GuideMatch::PointSource, which the host tests pin.
//
// Build (from retoque/shaders/dlssnr/precompile, dxc under the msvc-wine prefix on Linux):
//   dxc -T cs_6_0 -E CSMain -O3 -Qstrip_debug -Qstrip_reflect -Fo DlssNr_Guides_Shader.cso dlssnr_guides.hlsl
//   python3 ../../../../OptiScaler/shaders/shader_tools/create_header.py DlssNr_Guides_Shader.cso DlssNr_Guides_Shader.h DlssNr_Guides_cso
// DlssNr_GuideMatch_Dx12.cpp picks the header up through __has_include; without it the pass reports
// itself unavailable and the model keeps the frame-size guides.
//
// Also built as SPIR-V for the Vulkan route (DlssNr_GuideMatch_Vk.cpp, docs/dlssnr/design/reduced-scale-guides-vk.md),
// which includes its header unconditionally:
//   dxc -spirv -T cs_6_0 -E CSMain -O3 -Qstrip_debug -D VK_MODE -Fo DlssNr_Guides_Shader_Vk.spv dlssnr_guides.hlsl
//   python3 ../../../../OptiScaler/shaders/shader_tools/create_header.py DlssNr_Guides_Shader_Vk.spv DlssNr_Guides_Shader_Vk.h dlssnr_guides_spv
// Everything VK_MODE adds is under it, so the DXIL above is the same bytes with or without it. Under VK_MODE the
// constants are a push-constant block (DlssNr::GuideMatch::Vk::Push: no buffer, nothing to align, nothing a host write
// can race), the reads are sampled images at bindings 0 and 1 of set 0, the writes storage images at 2 and 3.

#ifdef VK_MODE
// The D3D12 cbuffer's twelve scalars, in its order, as one push-constant block of 48 bytes.
struct GuideParams
{
    uint DepthBaseX;
    uint DepthBaseY;
    uint DepthWidth;
    uint DepthHeight;
    uint MotionBaseX;
    uint MotionBaseY;
    uint MotionWidth;
    uint MotionHeight;
    uint OutWidth;
    uint OutHeight;
    uint Pad0;
    uint Pad1;
};

[[vk::push_constant]] GuideParams gParams;

#define gDepthBaseX gParams.DepthBaseX
#define gDepthBaseY gParams.DepthBaseY
#define gDepthWidth gParams.DepthWidth
#define gDepthHeight gParams.DepthHeight
#define gMotionBaseX gParams.MotionBaseX
#define gMotionBaseY gParams.MotionBaseY
#define gMotionWidth gParams.MotionWidth
#define gMotionHeight gParams.MotionHeight
#define gOutWidth gParams.OutWidth
#define gOutHeight gParams.OutHeight

[[vk::binding(0, 0)]] Texture2D<float> gDepth;
[[vk::binding(1, 0)]] Texture2D<float2> gMotion;
[[vk::binding(2, 0)]] [[vk::image_format("r32f")]] RWTexture2D<float> gDepthOut;
[[vk::binding(3, 0)]] [[vk::image_format("rg32f")]] RWTexture2D<float2> gMotionOut;
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
    uint gOutWidth;
    uint gOutHeight;
    uint gPad0;
    uint gPad1;
};

Texture2D<float> gDepth : register(t0);
Texture2D<float2> gMotion : register(t1);
RWTexture2D<float> gDepthOut : register(u0);
RWTexture2D<float2> gMotionOut : register(u1);
#endif

uint PointSource(uint destination, uint destinationExtent, uint regionBase, uint regionExtent)
{
    const uint scaled = ((destination * 2u + 1u) * regionExtent) / (destinationExtent * 2u);
    return regionBase + min(scaled, regionExtent - 1u);
}

[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= gOutWidth || id.y >= gOutHeight)
        return;

    const uint2 depthAt = uint2(PointSource(id.x, gOutWidth, gDepthBaseX, gDepthWidth),
                                PointSource(id.y, gOutHeight, gDepthBaseY, gDepthHeight));
    const uint2 motionAt = uint2(PointSource(id.x, gOutWidth, gMotionBaseX, gMotionWidth),
                                 PointSource(id.y, gOutHeight, gMotionBaseY, gMotionHeight));

    gDepthOut[id.xy] = gDepth.Load(int3(depthAt, 0));
    gMotionOut[id.xy] = gMotion.Load(int3(motionAt, 0));
}

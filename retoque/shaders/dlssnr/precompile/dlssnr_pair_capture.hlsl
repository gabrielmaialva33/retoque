// Retoque's own numeric snapshot shader. No tone map, clamp, filtering, gamma or FP16 intermediate.
// The source is the pass's 2D view in GENERAL; immutable descriptors select each private readback.
[[vk::binding(0, 0)]] Texture2D<float4> Source;
[[vk::binding(1, 0)]] RWStructuredBuffer<float4> Pixels;
struct Size { uint Width; uint Height; };
[[vk::push_constant]] ConstantBuffer<Size> Extent;
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x < Extent.Width && id.y < Extent.Height)
        Pixels[id.y * Extent.Width + id.x] = Source.Load(int3(id.xy, 0));
}

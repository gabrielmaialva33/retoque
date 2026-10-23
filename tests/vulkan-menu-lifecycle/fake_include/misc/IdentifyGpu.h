#pragma once
// Host fake: only the predicate DxOverlayOwnsBackend reads.
struct FakeGpuInfo
{
    bool dxgiIsDxvk = false;
};
namespace IdentifyGpu
{
inline FakeGpuInfo& getPrimaryGpu()
{
    static FakeGpuInfo info;
    return info;
}
} // namespace IdentifyGpu

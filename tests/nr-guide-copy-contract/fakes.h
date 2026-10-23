#include <cassert>
#include <cstdint>
#include <iostream>
#include <memory>
#include <vector>
struct DXGI_SAMPLE_DESC
{
    unsigned Count = 1, Quality = 0;
};
struct D3D12_RESOURCE_DESC
{
    int Dimension = 3;
    uint64_t Alignment = 0, Width = 640;
    unsigned Height = 360;
    unsigned short DepthOrArraySize = 1, MipLevels = 1;
    int Format = 0;
    DXGI_SAMPLE_DESC SampleDesc;
    int Layout = 0;
    unsigned Flags = 0;
};
struct ID3D12Resource
{
    D3D12_RESOURCE_DESC desc;
    auto GetDesc() { return desc; }
};
struct ID3D12Device
{
};
struct ID3D12GraphicsCommandList
{
    unsigned copies = 0;
    void CopyResource(ID3D12Resource* dst, ID3D12Resource* src)
    {
        const auto a = dst->GetDesc(), b = src->GetDesc();
        assert(a.Dimension == b.Dimension && a.Width == b.Width && a.Height == b.Height);
        assert(a.MipLevels == b.MipLevels && a.DepthOrArraySize == b.DepthOrArraySize);
        assert(a.SampleDesc.Count == b.SampleDesc.Count && a.SampleDesc.Quality == b.SampleDesc.Quality);
        ++copies;
    }
};
static std::vector<std::unique_ptr<ID3D12Resource>> retired;
static unsigned allocations = 0;
static bool failAllocation = false;
bool IsTypeless(int f) { return f == 0; }
int TypedGuideFormat(int) { return 1; }
ID3D12Resource* CreateGuideClone(ID3D12Device*, ID3D12Resource* source)
{
    ++allocations;
    if (failAllocation)
        return nullptr;
    auto* clone = new ID3D12Resource(*source);
    // A successful fallback format differs from TypedGuideFormat; it must remain reusable.
    clone->desc.Format = 2;
    clone->desc.Alignment = 0;
    clone->desc.Flags = 0;
    return clone;
}
void ParkNrResource(ID3D12Resource*& clone)
{
    retired.emplace_back(clone);
    clone = nullptr;
}
void Barrier(ID3D12GraphicsCommandList*, ID3D12Resource*, int, int) {}
constexpr int D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE = 1, D3D12_RESOURCE_STATE_COPY_SOURCE = 2,
              D3D12_RESOURCE_STATE_COPY_DEST = 3;
#define LOG_DEBUG(...) ((void) 0)

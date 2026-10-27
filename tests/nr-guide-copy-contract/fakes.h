#include <cassert>
#include <cstdint>
#include <iostream>
#include <memory>
#include <set>
#include <vector>
using UINT = unsigned;
using UINT64 = uint64_t;
using HRESULT = int;
using DXGI_FORMAT = int;
using D3D12_RESOURCE_STATES = int;
constexpr int S_OK = 0, E_FAIL = -1;
#define FAILED(x) ((x) < 0)
#define SUCCEEDED(x) ((x) >= 0)
#define IID_PPV_ARGS(x) (x)
#define LOG_INFO(...) ((void) 0)
#define LOG_WARN(...) ((void) 0)
#define LOG_ERROR(...) ((void) 0)
#define LOG_DEBUG(...) ((void) 0)
enum : int
{
    DXGI_FORMAT_UNKNOWN,
    DXGI_FORMAT_R32_TYPELESS,
    DXGI_FORMAT_R32_FLOAT,
    DXGI_FORMAT_D32_FLOAT,
    DXGI_FORMAT_R16_TYPELESS,
    DXGI_FORMAT_R16_UNORM,
    DXGI_FORMAT_R16_FLOAT,
    DXGI_FORMAT_R24G8_TYPELESS,
    DXGI_FORMAT_R24_UNORM_X8_TYPELESS,
    DXGI_FORMAT_D24_UNORM_S8_UINT,
    DXGI_FORMAT_R32G8X24_TYPELESS,
    DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS,
    DXGI_FORMAT_D32_FLOAT_S8X24_UINT,
    DXGI_FORMAT_R32G32_TYPELESS,
    DXGI_FORMAT_R32G32_FLOAT,
    DXGI_FORMAT_R16G16_TYPELESS,
    DXGI_FORMAT_R16G16_FLOAT,
    DXGI_FORMAT_R8G8B8A8_TYPELESS,
    DXGI_FORMAT_R8G8B8A8_UNORM,
    DXGI_FORMAT_R16G16B16A16_TYPELESS,
    DXGI_FORMAT_R16G16B16A16_FLOAT
};
int ExpectedFamily(int f)
{
    switch (f)
    {
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_D32_FLOAT:
        return 1;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_FLOAT:
        return 2;
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
        return 3;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        return 4;
    case DXGI_FORMAT_R32G32_TYPELESS:
    case DXGI_FORMAT_R32G32_FLOAT:
        return 5;
    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R16G16_FLOAT:
        return 6;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
        return 7;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return 8;
    default:
        return -1;
    }
}
constexpr int D3D12_RESOURCE_DIMENSION_TEXTURE2D = 3;
constexpr int D3D12_RESOURCE_FLAG_NONE = 0;
constexpr int D3D12_HEAP_TYPE_DEFAULT = 0, D3D12_HEAP_FLAG_NONE = 0;
constexpr int D3D12_TEXTURE_LAYOUT_UNKNOWN = 0, D3D12_FEATURE_FORMAT_INFO = 1;
constexpr int D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE = 1, D3D12_RESOURCE_STATE_COPY_SOURCE = 2,
              D3D12_RESOURCE_STATE_COPY_DEST = 3;
struct DXGI_SAMPLE_DESC
{
    unsigned Count = 1, Quality = 0;
};
struct D3D12_RESOURCE_DESC
{
    int Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    uint64_t Alignment = 0, Width = 640;
    unsigned Height = 360;
    unsigned short DepthOrArraySize = 1, MipLevels = 1;
    int Format = DXGI_FORMAT_R32_TYPELESS;
    DXGI_SAMPLE_DESC SampleDesc;
    int Layout = 0;
    unsigned Flags = 0;
};
struct D3D12_HEAP_PROPERTIES
{
    int Type = 0;
};
struct D3D12_FEATURE_DATA_FORMAT_INFO
{
    DXGI_FORMAT Format;
    unsigned char PlaneCount = 0;
};
struct ID3D12Device;
struct ID3D12DeviceChild
{
    ID3D12Device* device = nullptr;
    int GetDevice(ID3D12Device** out);
};
struct ID3D12Resource : ID3D12DeviceChild
{
    D3D12_RESOURCE_DESC desc;
    auto GetDesc() { return desc; }
};
static unsigned allocations = 0;
struct ID3D12Device
{
    unsigned refs = 1;
    bool failAllocation = false, failFormatInfo = false, wrongPlanes = false;
    int allowedOnly = -1;
    std::set<int> failedFormats;
    std::vector<int> attemptedFormats;
    unsigned AddRef() { return ++refs; }
    unsigned Release()
    {
        assert(refs > 0);
        return --refs;
    }
    int CheckFeatureSupport(int feature, D3D12_FEATURE_DATA_FORMAT_INFO* info, size_t)
    {
        assert(feature == D3D12_FEATURE_FORMAT_INFO);
        if (failFormatInfo)
            return E_FAIL;
        const auto family = ExpectedFamily(info->Format);
        info->PlaneCount = static_cast<unsigned char>((family == 3 || family == 4) ? 2 : 1);
        if (wrongPlanes && info->Format != DXGI_FORMAT_R32_TYPELESS)
            info->PlaneCount = 2;
        return S_OK;
    }
    int CreateCommittedResource(const D3D12_HEAP_PROPERTIES*, int, const D3D12_RESOURCE_DESC* desc, int, void*,
                                ID3D12Resource** out)
    {
        ++allocations;
        attemptedFormats.push_back(desc->Format);
        *out = nullptr;
        if (failAllocation || failedFormats.contains(desc->Format) ||
            (allowedOnly >= 0 && desc->Format != allowedOnly) || desc->Format == DXGI_FORMAT_R24_UNORM_X8_TYPELESS ||
            desc->Format == DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS)
            return E_FAIL;
        *out = new ID3D12Resource;
        (*out)->device = this;
        (*out)->desc = *desc;
        return S_OK;
    }
};
int ID3D12DeviceChild::GetDevice(ID3D12Device** out)
{
    *out = device;
    if (!device)
        return E_FAIL;
    device->AddRef();
    return S_OK;
}
struct ID3D12GraphicsCommandList : ID3D12DeviceChild
{
    unsigned copies = 0, barriers = 0;
    void CopyResource(ID3D12Resource* dst, ID3D12Resource* src)
    {
        const auto a = dst->GetDesc(), b = src->GetDesc();
        assert(a.Dimension == b.Dimension && a.Width == b.Width && a.Height == b.Height);
        assert(a.MipLevels == b.MipLevels && a.DepthOrArraySize == b.DepthOrArraySize);
        assert(a.SampleDesc.Count == b.SampleDesc.Count && a.SampleDesc.Quality == b.SampleDesc.Quality);
        assert(ExpectedFamily(a.Format) == ExpectedFamily(b.Format) && "cross-family CopyResource");
        assert(device == src->device && device == dst->device && "cross-device CopyResource");
        ++copies;
    }
};
static std::vector<std::unique_ptr<ID3D12Resource>> retired;
void ParkNrResource(ID3D12Resource*& clone)
{
    retired.emplace_back(clone);
    clone = nullptr;
}
void Barrier(ID3D12GraphicsCommandList* cmd, ID3D12Resource*, int, int) { ++cmd->barriers; }

#pragma once
// The few D3D12 declarations DlssNr_ExposureScan.cpp and DlssNr_Submission.h name, shaped so the production code
// compiles unchanged. Interfaces are abstract; the fakes live in cases.cpp.
#include <cstddef>
#include <cstdint>

using UINT = unsigned;
using UINT64 = uint64_t;
using ULONG = unsigned long;
using HRESULT = long;
constexpr HRESULT S_OK = 0;
constexpr HRESULT E_FAIL = -1;
#define FAILED(value) ((value) < 0)
#define SUCCEEDED(value) ((value) >= 0)
#define IID_PPV_ARGS(pointer) 0, reinterpret_cast<void**>(pointer)

enum DXGI_FORMAT
{
    DXGI_FORMAT_UNKNOWN = 0,
    DXGI_FORMAT_R32G32_FLOAT = 16,
    DXGI_FORMAT_R8G8B8A8_UNORM = 28,
    DXGI_FORMAT_R16G16_FLOAT = 34,
    DXGI_FORMAT_R32_FLOAT = 41,
    DXGI_FORMAT_R16_FLOAT = 54,
};

enum D3D12_RESOURCE_DIMENSION
{
    D3D12_RESOURCE_DIMENSION_UNKNOWN = 0,
    D3D12_RESOURCE_DIMENSION_BUFFER = 1,
    D3D12_RESOURCE_DIMENSION_TEXTURE2D = 3,
};

enum D3D12_RESOURCE_FLAGS
{
    D3D12_RESOURCE_FLAG_NONE = 0,
    D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS = 0x4,
};

enum D3D12_RESOURCE_STATES
{
    D3D12_RESOURCE_STATE_UNORDERED_ACCESS = 0x8,
    D3D12_RESOURCE_STATE_COPY_DEST = 0x400,
    D3D12_RESOURCE_STATE_COPY_SOURCE = 0x800,
};

enum D3D12_HEAP_TYPE
{
    D3D12_HEAP_TYPE_DEFAULT = 1,
    D3D12_HEAP_TYPE_READBACK = 3,
};
enum D3D12_HEAP_FLAGS
{
    D3D12_HEAP_FLAG_NONE = 0,
};
enum D3D12_TEXTURE_LAYOUT
{
    D3D12_TEXTURE_LAYOUT_UNKNOWN = 0,
    D3D12_TEXTURE_LAYOUT_ROW_MAJOR = 1,
};
enum D3D12_RESOURCE_BARRIER_TYPE
{
    D3D12_RESOURCE_BARRIER_TYPE_TRANSITION = 0,
};
enum D3D12_RESOURCE_BARRIER_FLAGS
{
    D3D12_RESOURCE_BARRIER_FLAG_NONE = 0,
};
constexpr UINT D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES = 0xffffffff;
enum D3D12_TEXTURE_COPY_TYPE
{
    D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX = 0,
    D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT = 1,
};
enum D3D12_COMMAND_LIST_TYPE
{
    D3D12_COMMAND_LIST_TYPE_DIRECT = 0,
};

struct DXGI_SAMPLE_DESC
{
    UINT Count = 0;
    UINT Quality = 0;
};
struct D3D12_RESOURCE_DESC
{
    D3D12_RESOURCE_DIMENSION Dimension = D3D12_RESOURCE_DIMENSION_UNKNOWN;
    UINT64 Alignment = 0;
    UINT64 Width = 0;
    UINT Height = 0;
    uint16_t DepthOrArraySize = 0;
    uint16_t MipLevels = 0;
    DXGI_FORMAT Format = DXGI_FORMAT_UNKNOWN;
    DXGI_SAMPLE_DESC SampleDesc {};
    D3D12_TEXTURE_LAYOUT Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    D3D12_RESOURCE_FLAGS Flags = D3D12_RESOURCE_FLAG_NONE;
};
struct D3D12_HEAP_PROPERTIES
{
    D3D12_HEAP_TYPE Type = D3D12_HEAP_TYPE_DEFAULT;
};
struct D3D12_RANGE
{
    size_t Begin = 0, End = 0;
};
struct D3D12_BOX
{
    UINT left, top, front, right, bottom, back;
};
struct D3D12_UNORDERED_ACCESS_VIEW_DESC
{
    DXGI_FORMAT Format = DXGI_FORMAT_UNKNOWN;
};

struct ID3D12Resource;
struct D3D12_RESOURCE_TRANSITION_BARRIER
{
    ID3D12Resource* pResource = nullptr;
    UINT Subresource = 0;
    D3D12_RESOURCE_STATES StateBefore {};
    D3D12_RESOURCE_STATES StateAfter {};
};
struct D3D12_RESOURCE_BARRIER
{
    D3D12_RESOURCE_BARRIER_TYPE Type {};
    D3D12_RESOURCE_BARRIER_FLAGS Flags {};
    D3D12_RESOURCE_TRANSITION_BARRIER Transition {};
};
struct D3D12_SUBRESOURCE_FOOTPRINT
{
    DXGI_FORMAT Format = DXGI_FORMAT_UNKNOWN;
    UINT Width = 0, Height = 0, Depth = 0, RowPitch = 0;
};
struct D3D12_PLACED_SUBRESOURCE_FOOTPRINT
{
    UINT64 Offset = 0;
    D3D12_SUBRESOURCE_FOOTPRINT Footprint {};
};
struct D3D12_TEXTURE_COPY_LOCATION
{
    ID3D12Resource* pResource = nullptr;
    D3D12_TEXTURE_COPY_TYPE Type {};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT PlacedFootprint {}; // a union in the SDK; both members here
    UINT SubresourceIndex = 0;
};

struct IUnknown
{
    virtual ~IUnknown() = default;
    virtual ULONG AddRef() = 0;
    virtual ULONG Release() = 0;
};
struct ID3D12Heap : IUnknown
{
};
struct ID3D12Resource : IUnknown
{
    virtual HRESULT GetDevice(int, void**) = 0; // ID3D12DeviceChild's
    virtual D3D12_RESOURCE_DESC GetDesc() = 0;
    virtual HRESULT Map(UINT, const D3D12_RANGE*, void**) = 0;
    virtual void Unmap(UINT, const D3D12_RANGE*) = 0;
};
struct ID3D12Device : IUnknown
{
    virtual HRESULT GetDeviceRemovedReason() = 0;
    virtual HRESULT CreateCommittedResource(const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS, const D3D12_RESOURCE_DESC*,
                                            D3D12_RESOURCE_STATES, const void*, int, void**) = 0;
};
struct ID3D12CommandList : IUnknown
{
};
struct ID3D12GraphicsCommandList : ID3D12CommandList
{
    virtual D3D12_COMMAND_LIST_TYPE GetType() = 0;
    virtual void ResourceBarrier(UINT, const D3D12_RESOURCE_BARRIER*) = 0;
    virtual void CopyBufferRegion(ID3D12Resource*, UINT64, ID3D12Resource*, UINT64, UINT64) = 0;
    virtual void CopyTextureRegion(const D3D12_TEXTURE_COPY_LOCATION*, UINT, UINT, UINT,
                                   const D3D12_TEXTURE_COPY_LOCATION*, const D3D12_BOX*) = 0;
};
struct ID3D12CommandQueue : IUnknown
{
};

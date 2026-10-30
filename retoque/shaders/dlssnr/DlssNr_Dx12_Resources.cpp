#include "pch.h"

#include "DlssNr_Dx12_Internal.h"
#include <Logger.h>
#include <dlssnr/DlssNr_DeviceMatch.h>

namespace DlssNr::Dx12Internal
{
ID3D12Resource* CreateScratch(ID3D12Device* device, DXGI_FORMAT format, unsigned int width, unsigned int height)
{
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    // The model writes its result, so the destination has to be a UAV.
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;

    ID3D12Resource* res = nullptr;
    device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr,
                                    IID_PPV_ARGS(&res));

    if (res != nullptr && g_nrScratchGeneration != UINT64_MAX)
        ++g_nrScratchGeneration;

    return res;
}

// The before-upscale copy. A scratch surface with one extra allowance: some games leave their colour
// in RENDER_TARGET and say so through ColorResourceBarrier, and the upscaler then transitions whatever
// it is handed as colour from that state. A copy standing in for the colour has to be able to sit in
// it, and a transition into RENDER_TARGET on a surface created without the flag is invalid. Colour
// formats are checked before allocation; failure falls back without returning an incompatible copy.
ID3D12Resource* CreatePreUpscaleScratch(ID3D12Device* device, DXGI_FORMAT format, unsigned int width,
                                        unsigned int height)
{
    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    D3D12_RESOURCE_DESC desc {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    desc.Width = width;
    desc.Height = height;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS | D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;

    ID3D12Resource* res = nullptr;

    if (FAILED(device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                               D3D12_RESOURCE_STATE_UNORDERED_ACCESS, nullptr, IID_PPV_ARGS(&res))))
        return nullptr;

    return res;
}

// An sRGB surface cannot be written through a UAV, and its typed twin would be read by the upscaler
// as linear -- a different picture, gamma-shifted. Such a colour is left to the after-upscale path.
bool IsSrgb(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
        return true;
    default:
        return false;
    }
}

void Barrier(ID3D12GraphicsCommandList* cmdList, ID3D12Resource* res, D3D12_RESOURCE_STATES from,
             D3D12_RESOURCE_STATES to)
{
    if (from == to)
        return;

    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = res;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = from;
    b.Transition.StateAfter = to;
    cmdList->ResourceBarrier(1, &b);
}

// A typeless resource cannot be viewed, and NGX builds its own views with nothing to tell it which
// format to use. Depth is very often declared typeless, so the typed member of the same family is
// substituted; CopyResource accepts that as a destination for the typeless original.
DXGI_FORMAT TypedGuideFormat(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R32_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R16_TYPELESS:
        return DXGI_FORMAT_R16_UNORM;
    case DXGI_FORMAT_R24G8_TYPELESS:
        return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
        return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R32G32_TYPELESS:
        return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R16G16_TYPELESS:
        return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        return DXGI_FORMAT_R16G16B16A16_FLOAT;
    default:
        return f;
    }
}

bool IsTypeless(DXGI_FORMAT f) { return TypedGuideFormat(f) != f; }

// Device-child queries return a temporary reference; the session owner is retained separately.
bool DeviceChildBelongsTo(ID3D12Device* device, ID3D12DeviceChild* child)
{
    if (device == nullptr || child == nullptr)
        return false;
    ID3D12Device* owner = nullptr;
    const HRESULT hr = child->GetDevice(IID_PPV_ARGS(&owner));
    const bool matches = SUCCEEDED(hr) && owner == device;
    if (owner != nullptr)
        owner->Release();
    return matches;
}

// A resource also belongs to the device when it reports another interface on the same adapter: a wrapper layer
// (ReShade through [Plugins] LoadReShade, a Streamline proxy) hands out lists of its own and leaves resources
// unwrapped, so a texture reports the device underneath while the list it is recorded on reports the wrapper. Lists
// and queues keep the overload above and must be the exact device (DlssNr_DeviceMatch.h).
bool DeviceChildBelongsTo(ID3D12Device* device, ID3D12Resource* resource)
{
    if (device == nullptr || resource == nullptr)
        return false;
    ID3D12Device* owner = nullptr;
    const HRESULT hr = resource->GetDevice(IID_PPV_ARGS(&owner));
    const auto relation =
        SUCCEEDED(hr) ? DlssNr::DeviceMatch::Relate(device, owner) : DlssNr::DeviceMatch::Relation::Foreign;
    static std::atomic<bool> said { false };
    if (relation == DlssNr::DeviceMatch::Relation::Wrapper && !said.exchange(true))
        LOG_INFO("DLSS-NR: a resource reports device {} and the command list device {}, on the same adapter (a "
                 "wrapper layer such as ReShade); NR works on the command list's device",
                 (void*) owner, (void*) device);
    if (owner != nullptr)
        owner->Release();
    return relation != DlssNr::DeviceMatch::Relation::Foreign;
}

// Only the typeless guide families translated above are admitted for a typed twin.
// Different typed alternatives in one family remain reusable (e.g. R32_FLOAT / D32_FLOAT).
DXGI_FORMAT GuideCopyFamily(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R32_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_D32_FLOAT:
        return DXGI_FORMAT_R32_TYPELESS;
    case DXGI_FORMAT_R16_TYPELESS:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R16_FLOAT:
        return DXGI_FORMAT_R16_TYPELESS;
    case DXGI_FORMAT_R24G8_TYPELESS:
    case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
    case DXGI_FORMAT_D24_UNORM_S8_UINT:
        return DXGI_FORMAT_R24G8_TYPELESS;
    case DXGI_FORMAT_R32G8X24_TYPELESS:
    case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
    case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
        return DXGI_FORMAT_R32G8X24_TYPELESS;
    case DXGI_FORMAT_R32G32_TYPELESS:
    case DXGI_FORMAT_R32G32_FLOAT:
        return DXGI_FORMAT_R32G32_TYPELESS;
    case DXGI_FORMAT_R16G16_TYPELESS:
    case DXGI_FORMAT_R16G16_FLOAT:
        return DXGI_FORMAT_R16G16_TYPELESS;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
        return DXGI_FORMAT_R8G8B8A8_TYPELESS;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
        return DXGI_FORMAT_R16G16B16A16_TYPELESS;
    default:
        return DXGI_FORMAT_UNKNOWN;
    }
}

bool GuideCopyCompatible(ID3D12Device* device, const D3D12_RESOURCE_DESC& source, const D3D12_RESOURCE_DESC& clone)
{
    if (device == nullptr || source.Dimension != clone.Dimension || source.Width != clone.Width ||
        source.Height != clone.Height || source.DepthOrArraySize != clone.DepthOrArraySize ||
        source.MipLevels != clone.MipLevels || source.SampleDesc.Count == 0 ||
        source.SampleDesc.Count != clone.SampleDesc.Count || source.SampleDesc.Quality != clone.SampleDesc.Quality)
        return false;
    const DXGI_FORMAT family = GuideCopyFamily(source.Format);
    if (family == DXGI_FORMAT_UNKNOWN || family != GuideCopyFamily(clone.Format))
        return false;
    D3D12_FEATURE_DATA_FORMAT_INFO srcInfo { source.Format }, dstInfo { clone.Format };
    return SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &srcInfo, sizeof(srcInfo))) &&
           SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &dstInfo, sizeof(dstInfo))) &&
           srcInfo.PlaneCount != 0 && srcInfo.PlaneCount == dstInfo.PlaneCount;
}

// The present ring copies only mip zero of depth plane zero (not stencil), using
// CopyTextureRegion with a null box and destination origin zero. The donor can have
// additional mips/planes; their numeric subresource indices are deliberately not reused.
bool PresentGuideCopyCompatible(ID3D12Device* device, const D3D12_RESOURCE_DESC& source,
                                const D3D12_RESOURCE_DESC& clone)
{
    if (device == nullptr || source.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
        clone.Dimension != source.Dimension || source.Width == 0 || source.Height == 0 || clone.Width != source.Width ||
        clone.Height != source.Height || source.DepthOrArraySize != 1 || clone.DepthOrArraySize != 1 ||
        source.MipLevels == 0 || clone.MipLevels != 1 || source.SampleDesc.Count != 1 || clone.SampleDesc.Count != 1 ||
        source.SampleDesc.Quality != 0 || clone.SampleDesc.Quality != 0 || source.Format == DXGI_FORMAT_UNKNOWN ||
        source.Format != clone.Format)
        return false;
    D3D12_FEATURE_DATA_FORMAT_INFO info { source.Format };
    return SUCCEEDED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_INFO, &info, sizeof(info))) &&
           info.PlaneCount != 0;
}

// Creates a typed twin of a guide buffer, matching everything but the format.
ID3D12Resource* CreateGuideClone(ID3D12Device* device, ID3D12Resource* source)
{
    if (!DeviceChildBelongsTo(device, source))
        return nullptr;
    const D3D12_RESOURCE_DESC src = source->GetDesc();

    // More than one candidate, because the obvious typed member of a family is not always a format a
    // resource may be created with. R32_FLOAT_X8X24_TYPELESS is the case that forced this: it is the
    // right way to VIEW an R32G8X24_TYPELESS depth buffer and CreateCommittedResource refuses it, so
    // the single-candidate version could never clone a depth buffer of that family at all. Ordered
    // most faithful first; each failure is logged with its HRESULT so a new game's format does not
    // have to be guessed at again.
    DXGI_FORMAT candidates[4] = {};
    uint32_t count = 0;
    candidates[count++] = TypedGuideFormat(src.Format);

    switch (src.Format)
    {
    case DXGI_FORMAT_R32G8X24_TYPELESS:
        candidates[count++] = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
        break;
    case DXGI_FORMAT_R24G8_TYPELESS:
        candidates[count++] = DXGI_FORMAT_D24_UNORM_S8_UINT;
        break;
    case DXGI_FORMAT_R32_TYPELESS:
        candidates[count++] = DXGI_FORMAT_D32_FLOAT;
        break;
    case DXGI_FORMAT_R16_TYPELESS:
        candidates[count++] = DXGI_FORMAT_R16_FLOAT;
        break;
    default:
        break;
    }

    D3D12_HEAP_PROPERTIES heap {};
    heap.Type = D3D12_HEAP_TYPE_DEFAULT;

    for (uint32_t i = 0; i < count; i++)
    {
        D3D12_RESOURCE_DESC desc = src;
        desc.Format = candidates[i];
        desc.Flags = D3D12_RESOURCE_FLAG_NONE;

        // Let the runtime pick the alignment rather than inheriting the source's.
        //
        // A guide created with D3D12_RESOURCE_FLAG_USE_TIGHT_ALIGNMENT (0x400, Agility SDK) reports an
        // alignment that is only legal together with that flag. The clone drops the flags, so carrying
        // the number over made every CreateCommittedResource fail with E_INVALIDARG before the format
        // was even considered -- which is why a candidate as ordinary as R16G16B16A16_FLOAT was refused.
        desc.Alignment = 0;
        if (!GuideCopyCompatible(device, src, desc))
            continue;

        ID3D12Resource* res = nullptr;
        const HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                           D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&res));

        if (SUCCEEDED(hr) && res != nullptr)
        {
            if (i != 0)
                LOG_INFO("DLSS-NR guide clone: source format {} cloned as {} (candidate {} of {})", (int) src.Format,
                         (int) candidates[i], i + 1, count);

            return res;
        }

        LOG_WARN("DLSS-NR guide clone: source format {}, candidate {} ({}) refused, hr 0x{:08X}", (int) src.Format,
                 i + 1, (int) candidates[i], (UINT) hr);
    }

    LOG_ERROR("DLSS-NR guide clone failed for every candidate. source desc: dim {} {}x{} arr {} mips {} "
              "fmt {} samples {} layout {} flags 0x{:X} alignment {}",
              (int) src.Dimension, (UINT64) src.Width, src.Height, src.DepthOrArraySize, src.MipLevels,
              (int) src.Format, src.SampleDesc.Count, (int) src.Layout, (UINT) src.Flags, (UINT64) src.Alignment);

    return nullptr;
}

// Hands back something the model can actually read: the guide itself when it is typed, or a typed copy
// of it when it is not. NGX requires its inputs in NON_PIXEL_SHADER_RESOURCE at evaluate time, which is
// a documented contract rather than a guess about any one game's frame graph, so that is the state
// transitioned away from and back to here.
// Freezing is a diagnostic, and it reuses this function because the clone it already keeps is
// exactly the thing a frozen guide is: a private copy the model reads instead of the live resource.
// Freezing is then not a new mechanism but the absence of one -- stop refreshing the copy.
//
// A frozen guide is valid data that is wrong for this frame, which is a far better probe than a
// constant would be. A constant is degenerate and a model may special-case it; stale depth is
// ordinary depth that simply disagrees with the picture, and anything reading it has to notice.
// Hands back something the model can actually read: the guide itself when it is typed, or a typed
// copy of it when it is not. NGX requires its inputs in NON_PIXEL_SHADER_RESOURCE at evaluate time,
// which is a documented contract rather than a guess about any one game's frame graph, so that is
// the state transitioned away from and back to here.
// Internal type: RestoreResourceState (DlssNr_Dx12_Internal.h)
// Internal type: ReadResourceScope (DlssNr_Dx12_Internal.h)
void RestoreResourceState::Restore()
{
    if (resource)
        Barrier(cmd, resource, from, to);
    resource = nullptr;
}
RestoreResourceState::~RestoreResourceState() { Restore(); }

ReadResourceScope::ReadResourceScope(ID3D12GraphicsCommandList* cmd, ID3D12Resource* resource,
                                     D3D12_RESOURCE_STATES arrival)
    : restore { cmd, resource, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, arrival }
{
    if (resource)
        Barrier(cmd, resource, arrival, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
}

// The codec binds Texture2D<float4>, never an integer, array, MSAA or depth view.
// Keep typeless translations identical to Shader_Dx12::TranslateTypelessFormats.
bool ExposureTextureUsable(ID3D12Device* device, ID3D12Resource* resource)
{
    if (device == nullptr || resource == nullptr)
        return false;
    const auto desc = resource->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.Width == 0 || desc.Height == 0 ||
        desc.DepthOrArraySize != 1 || desc.SampleDesc.Count != 1 || desc.MipLevels == 0 ||
        (desc.Flags & D3D12_RESOURCE_FLAG_DENY_SHADER_RESOURCE) != 0)
        return false;

    DXGI_FORMAT view = desc.Format;
    switch (view)
    {
    case DXGI_FORMAT_R32_TYPELESS:
        view = DXGI_FORMAT_R32_FLOAT;
        break;
    case DXGI_FORMAT_R32G32_TYPELESS:
        view = DXGI_FORMAT_R32G32_FLOAT;
        break;
    case DXGI_FORMAT_R16G16_TYPELESS:
        view = DXGI_FORMAT_R16G16_FLOAT;
        break;
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
        view = DXGI_FORMAT_R32G32B32A32_FLOAT;
        break;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
        view = DXGI_FORMAT_R16G16B16A16_FLOAT;
        break;
    case DXGI_FORMAT_R16_FLOAT:
    case DXGI_FORMAT_R32_FLOAT:
    case DXGI_FORMAT_R16G16_FLOAT:
    case DXGI_FORMAT_R32G32_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R8_UNORM:
    case DXGI_FORMAT_R16_UNORM:
    case DXGI_FORMAT_R8G8_UNORM:
    case DXGI_FORMAT_R16G16_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R16G16B16A16_UNORM:
        break;
    default:
        return false; // Unknown interpretations must not become an exposure reading.
    }
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support { view };
    return !FAILED(device->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) &&
           (support.Support1 & D3D12_FORMAT_SUPPORT1_SHADER_LOAD) != 0;
}

ID3D12Resource* ReadableGuide(ID3D12Device* device, ID3D12GraphicsCommandList* cmdList, ID3D12Resource* source,
                              ID3D12Resource** clone)
{
    if (!DeviceChildBelongsTo(device, cmdList) || !DeviceChildBelongsTo(device, source))
        return nullptr;
    if (!IsTypeless(source->GetDesc().Format))
        return source;
    if (clone == nullptr)
        return nullptr;

    // A dynamic-resolution game reallocates its depth and motion vectors as the render size moves, so
    // the clone made for the old size no longer matches -- and CopyResource demands identical
    // dimensions. Copying a 1970x1108 source into a 984x554 clone is undefined and removes the device,
    // which is the DRS crash. Rebuild the clone whenever the source's shape has changed under it.
    if (*clone != nullptr)
    {
        const D3D12_RESOURCE_DESC have = (*clone)->GetDesc();
        const D3D12_RESOURCE_DESC want = source->GetDesc();

        // CopyResource needs the whole shape, not just the base extent: games can replace a guide
        // with different mips, slices or samples at the same resolution. Keep the successful candidate
        // format: comparing it with TypedGuideFormat's first choice would rebuild on every frame.
        if (!DeviceChildBelongsTo(device, *clone) || !GuideCopyCompatible(device, want, have))
        {
            // Retired, not released: the previous copy may still be in flight on the game's queue.
            ParkNrResource(*clone);
        }
    }

    if (*clone == nullptr)
    {
        *clone = CreateGuideClone(device, source);

        if (*clone == nullptr)
            return nullptr;

        LOG_DEBUG("DLSS-NR cloned a typeless guide as format {}", (int) TypedGuideFormat(source->GetDesc().Format));
    }

    if (!DeviceChildBelongsTo(device, *clone) || !GuideCopyCompatible(device, source->GetDesc(), (*clone)->GetDesc()))
        return nullptr;

    Barrier(cmdList, source, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
    cmdList->CopyResource(*clone, source);
    Barrier(cmdList, source, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Barrier(cmdList, *clone, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    return *clone;
}

// The upscaler's own names differ between super resolution and ray reconstruction, and only one set is
// present on any given block.
// Whether a surface can physically hold linear HDR.
//
// Only a float format can: linear light is open-ended and runs far past 1.0, which a normalised
// integer surface cannot represent. An 8-bit UNORM frame is finished, display-referred output, and
// so is a 10-bit one -- HDR10 is PQ-encoded, which is display-referred too.
//
// The game's IsHDR flag is a statement of intent that is not always true, and believing it over a
// format that cannot hold linear light means encoding an already-encoded frame a second time.
bool FormatCanHoldLinearHdr(DXGI_FORMAT format)
{
    switch (format)
    {
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R16G16B16A16_TYPELESS:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_TYPELESS:
    case DXGI_FORMAT_R32G32B32_FLOAT:
    case DXGI_FORMAT_R11G11B10_FLOAT:
        return true;
    default:
        return false;
    }
}

ID3D12Resource* GetResource(NVSDK_NGX_Parameter* params, const char* a, const char* b)
{
    ID3D12Resource* res = nullptr;

    if (params->Get(a, &res) == NVSDK_NGX_Result_Success && res != nullptr)
        return res;

    res = nullptr;

    if (params->Get(b, &res) == NVSDK_NGX_Result_Success && res != nullptr)
        return res;

    // The same key again, as a plain pointer.
    //
    // NVSDK_NGX_Parameter has a typed setter per resource kind and an untyped one, and on a real NGX
    // parameter block those are separate slots: what goes in through Set(name, void*) does not come
    // back out of Get(name, ID3D12Resource**). A game running its own D3D12 upscaler sets these
    // typed, so the typed read above is enough and always was.
    //
    // Both of OptiScaler's bridges write them untyped. IFeature_Dx11wDx12 and IFeature_VkwDx12 turn
    // the game's D3D11 textures or Vulkan images into D3D12 resources and hand them over with
    // Set(name, (void*) resource) -- so the typed read came back null a few lines after the resource
    // had been written, and the pass quietly did nothing. That is the whole reason this never ran in
    // a DirectX 11 or Vulkan game.
    void* untyped = nullptr;

    if (params->Get(a, &untyped) == NVSDK_NGX_Result_Success && untyped != nullptr)
        return static_cast<ID3D12Resource*>(untyped);

    untyped = nullptr;

    if (params->Get(b, &untyped) == NVSDK_NGX_Result_Success && untyped != nullptr)
        return static_cast<ID3D12Resource*>(untyped);

    return nullptr;
}

} // namespace DlssNr::Dx12Internal

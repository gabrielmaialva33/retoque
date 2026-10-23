#pragma once

#include <atomic>

#if __has_include(<nvsdk_ngx.h>)
#include <nvsdk_ngx.h>
#endif

#if __has_include(<nvsdk_ngx_defs.h>)
#include <nvsdk_ngx_defs.h>
#endif

namespace DlssNr
{
struct RenderSize
{
    unsigned int width = 0;
    unsigned int height = 0;
};

enum class RenderSizeSource
{
    Missing,
    Subrect,
    Dimensions,
    Creation,
    Incomplete,
    Stale
};
struct RenderSizeResult
{
    RenderSize size;
    RenderSizeSource source = RenderSizeSource::Missing;
};

inline RenderSize ReadDisplaySize(const NVSDK_NGX_Parameter* params)
{
    RenderSize size;
    if (params)
    {
        auto* p = const_cast<NVSDK_NGX_Parameter*>(params);
        p->Get(NVSDK_NGX_Parameter_OutWidth, &size.width);
        p->Get(NVSDK_NGX_Parameter_OutHeight, &size.height);
    }
    return size;
}

// Only current metadata. A partial or invalid contract must never fall through to creation.
inline RenderSizeResult ReadRenderSize(const NVSDK_NGX_Parameter* params)
{
    if (!params)
        return {};
    auto* p = const_cast<NVSDK_NGX_Parameter*>(params);
    RenderSize sub;
    p->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Width, &sub.width);
    p->Get(NVSDK_NGX_Parameter_DLSS_Render_Subrect_Dimensions_Height, &sub.height);
    if (sub.width || sub.height)
        return { sub, sub.width && sub.height ? RenderSizeSource::Subrect : RenderSizeSource::Incomplete };

    RenderSize size;
    p->Get(NVSDK_NGX_Parameter_Width, &size.width);
    p->Get(NVSDK_NGX_Parameter_Height, &size.height);
    if (!size.width && !size.height)
        return {};
    const auto display = ReadDisplaySize(params);
    if (!size.width || !size.height || (display.width && size.width > display.width) ||
        (display.height && size.height > display.height))
        return { {}, RenderSizeSource::Incomplete };
    return { size, RenderSizeSource::Dimensions };
}

namespace RenderGeometry
{
struct Binding
{
    const NVSDK_NGX_Parameter* params = nullptr;
    RenderSize render, display;
    const std::atomic_bool* stale = nullptr;
};
inline thread_local const Binding* active = nullptr;
inline bool Compatible(const Binding& binding, const NVSDK_NGX_Parameter* params)
{
    if (binding.stale && binding.stale->load(std::memory_order_relaxed))
        return false;
    const auto display = ReadDisplaySize(params);
    return (!display.width || display.width == binding.display.width) &&
           (!display.height || display.height == binding.display.height);
}
} // namespace RenderGeometry

inline RenderSizeResult ResolveRenderSize(const NVSDK_NGX_Parameter* params)
{
    const auto current = ReadRenderSize(params);
    if (current.source != RenderSizeSource::Missing)
        return current;
    const auto* binding = RenderGeometry::active;
    if (!binding || binding->params != params)
        return {};
    if (!RenderGeometry::Compatible(*binding, params))
        return { {}, RenderSizeSource::Stale };
    return { binding->render, RenderSizeSource::Creation };
}

inline RenderSize GetRenderSize(const NVSDK_NGX_Parameter* params) { return ResolveRenderSize(params).size; }

inline RenderSize GetDisplaySize(const NVSDK_NGX_Parameter* params)
{
    const auto current = ReadDisplaySize(params);
    if (current.width || current.height)
        return current; // No mixing axes from different contracts.
    const auto* binding = RenderGeometry::active;
    if (binding && binding->params == params && RenderGeometry::Compatible(*binding, params))
        return binding->display;
    return {};
}

inline const char* RenderSizeSourceName(RenderSizeSource source)
{
    switch (source)
    {
    case RenderSizeSource::Subrect:
        return "evaluate-subrect";
    case RenderSizeSource::Dimensions:
        return "evaluate-dimensions";
    case RenderSizeSource::Creation:
        return "feature-creation";
    case RenderSizeSource::Incomplete:
        return "incomplete";
    case RenderSizeSource::Stale:
        return "stale-creation";
    default:
        return "unknown";
    }
}
} // namespace DlssNr

#pragma once

#include "DlssNr_VkPreRuntime.h"
#include <nvsdk_ngx_defs.h>

namespace DlssNr::VkPre
{
inline void RememberNgxFeature(VkDevice device, NVSDK_NGX_Feature type, NVSDK_NGX_Handle** handle,
                               NVSDK_NGX_Result result) noexcept
{
    if (result != NVSDK_NGX_Result_Success || handle == nullptr || *handle == nullptr)
        return;
    Feature kind = Feature::Unknown;
    switch (type)
    {
    case NVSDK_NGX_Feature_SuperSampling:
        kind = Feature::SuperSampling;
        break;
    case NVSDK_NGX_Feature_RayReconstruction:
        kind = Feature::RayReconstruction;
        break;
    case NVSDK_NGX_Feature_FrameGeneration:
        kind = Feature::FrameGeneration;
        break;
    default:
        break;
    }
    Runtime::RememberFeature(device, (*handle)->Id, kind, true, reinterpret_cast<uintptr_t>(*handle));
}
} // namespace DlssNr::VkPre

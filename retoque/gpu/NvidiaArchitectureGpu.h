#pragma once

#include "NvidiaArchitecture.h"

#include <Logger.h>
#include <misc/IdentifyGpu.h>

#include <atomic>

namespace Retoque::NvidiaArchitecture
{
// Inline keeps the adapter bridge shared without adding another Windows translation unit. The one
// process-wide flag is shared by NR, the MFG probe and the Ada kernel gate, including concurrent calls.
inline uint32_t ForGpu(const GpuInformation& gpu)
{
    const uint32_t reported = gpu.nvidiaArchInfo.architecture_id;
    const uint32_t resolved = ForAdapter(gpu.vendorId == VendorId::Nvidia, gpu.deviceId, gpu.name, reported);
    if (resolved != reported)
    {
        static std::atomic<bool> logged { false };
        if (!logged.exchange(true, std::memory_order_relaxed))
            LOG_INFO("Retoque: NVAPI architecture {:#x} disagrees with {} (PCI device {:#x}); using {:#x} for "
                     "Retoque decisions only (possible dlssg_for_sm86 spoof)",
                     reported, gpu.name, gpu.deviceId, resolved);
    }
    return resolved;
}
} // namespace Retoque::NvidiaArchitecture

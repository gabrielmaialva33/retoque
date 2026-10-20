#pragma once
// Identity/link fixture only: platform/domain behavior is deliberately not simulated.
#include <cstdint>
#include <cstddef>
#define __cdecl
class Config;
// Declared only: the diagnostics helpers (split step 6) take and return these by reference or value.
struct DlssNrPassSnapshot;
constexpr uint32_t kDlssNrMeterGrid = 64; // DlssNr_Common.h, for the meter sizes the header defines
namespace DlssNr::GpuTiming
{
struct Metadata;
}
using HMODULE = void*;
using HANDLE = void*;
using UINT64 = uint64_t;
using D3D12_RESOURCE_STATES = int;
enum DXGI_FORMAT
{
    DXGI_FORMAT_UNKNOWN
};
enum D3D12_RESOURCE_FLAGS
{
    D3D12_RESOURCE_FLAG_NONE
};
enum class Scaler
{
    Count
};
struct ID3D12Device;
struct ID3D12GraphicsCommandList;
struct ID3D12Resource;
struct ID3D12CommandAllocator;
struct ID3D12Fence;
struct NVSDK_NGX_Parameter;
struct DlssNrResolvedPassSettings
{
};
struct DlssNrFrameInfo
{
};
class DlssNr_Dx12
{
};
class DlssNr_DetailStats_Dx12;
class DlssNr_Pulse_Dx12;
constexpr unsigned DLSSNR_DETAIL_STATS_SLOTS = 4;
constexpr unsigned DLSSNR_PULSE_SLOTS = 4;
namespace capture
{
struct FrameCapture
{
};
} // namespace capture
namespace DlssNr
{
struct PeripheryState
{
};
struct DetailMeasureStatus
{
};
struct PulseStatus
{
};
class SynthMotionGuide;
enum class PresentColour
{
    Declined
};
namespace Detail
{
struct StageCoverage
{
};
struct CoverageTotals
{
};
struct StableExtent
{
};
struct CreationFrameGate
{
};
} // namespace Detail
namespace Exposure
{
struct PlausibilityLatch
{
};
} // namespace Exposure
namespace Submission
{
namespace Detail
{
enum class Admission;
}
struct Usage
{
};
inline uint64_t Clock() { return 0; }
} // namespace Submission
namespace Chain
{
class RecordingLease;
struct RecordingGate
{
};
struct Schedule
{
};
} // namespace Chain
namespace Cadence
{
struct Decision;
struct Scheduler
{
};
enum class Reason
{
    Off
};
inline const char* Describe(Reason, int) { return "off"; }
namespace Adaptive
{
struct Controller
{
};
} // namespace Adaptive
} // namespace Cadence
namespace DetailStats
{
struct Accumulator
{
};
} // namespace DetailStats
namespace Pulse
{
enum class Kind
{
    Every
};
struct Tracker
{
};
struct LogGate
{
};
} // namespace Pulse
namespace PrivateSr
{
struct Feature
{
};
} // namespace PrivateSr
namespace Enlarge
{
struct Decision;
constexpr uint32_t kTransferMatched = 1;
enum class Why
{
    NotYet
};
} // namespace Enlarge
namespace PresentGuides
{
constexpr unsigned kRingSize = 4;
struct Ring
{
};
} // namespace PresentGuides
namespace PresentGuideIdentity
{
struct Tracker
{
};
struct Consumer
{
    const char* name;
};
struct SourceHistory
{
};
struct StatusWindow
{
};
enum class Fallback
{
    Zero
};
} // namespace PresentGuideIdentity
} // namespace DlssNr

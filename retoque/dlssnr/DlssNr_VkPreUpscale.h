#pragma once

#include "DlssNr_VkRecording.h"
#include <cstdint>

// Proposed Stage-1 policy only. The passive implementation never calls this from an evaluate seam.
namespace DlssNr::VkPre
{
enum class Feature : uint8_t
{
    Unknown,
    SuperSampling,
    RayReconstruction,
    FrameGeneration
};
enum class Backend : uint8_t
{
    Native,
    DLSS_on12,
    XeSS_on12,
    FSR21_on12,
    FSR22_on12,
    FFX_on12,
    RayReconstruction,
    Unknown
};
enum class Refusal : uint8_t
{
    None,
    Off,
    Stage,
    UnknownFeature,
    RayReconstruction,
    FrameGeneration,
    Bridge,
    Identity,
    OtherPicture,
    Observation,
    Recording,
    Busy
};
struct Admission
{
    bool enabled = false;
    uint32_t stage = 0;
    Feature feature = Feature::Unknown;
    Backend backend = Backend::Native;
    bool identity = false, mainPicture = false, observed = false;
    bool primary = false, oneTime = false, simultaneous = false, outstanding = false;
};
constexpr Refusal Admit(const Admission& a)
{
    if (!a.enabled)
        return Refusal::Off;
    if (a.stage != 1)
        return Refusal::Stage;
    if (a.feature == Feature::RayReconstruction)
        return Refusal::RayReconstruction;
    if (a.feature == Feature::FrameGeneration)
        return Refusal::FrameGeneration;
    if (a.feature != Feature::SuperSampling)
        return Refusal::UnknownFeature;
    if (a.backend == Backend::RayReconstruction)
        return Refusal::RayReconstruction;
    if (a.backend != Backend::Native)
        return Refusal::Bridge;
    if (!a.identity)
        return Refusal::Identity;
    if (!a.mainPicture)
        return Refusal::OtherPicture;
    if (!a.observed)
        return Refusal::Observation;
    if (!a.primary || !a.oneTime || a.simultaneous)
        return Refusal::Recording;
    if (a.outstanding)
        return Refusal::Busy;
    return Refusal::None;
}
enum class Outcome : uint8_t
{
    Unselected,
    StandDown,
    Rejected,
    Partial,
    Bound
};
// Rejection of selected Stage 1 never falls back to a second NR attempt.
constexpr bool AllowPost(Outcome outcome) { return outcome == Outcome::Unselected; }
// Use the recording lifetime registry's identity, never a second observer epoch.
using Ticket = VkRecording::Ticket;
constexpr bool ValidTicket(Ticket t) { return t.device && t.buffer && t.epoch; }
constexpr bool SameTicket(Ticket a, Ticket b)
{
    return a.device == b.device && a.buffer == b.buffer && a.epoch == b.epoch;
}
// The future route owns synchronization. Only exact recording retirement releases this loan.
class Outstanding
{
    Ticket _ticket;

  public:
    bool Busy() const { return ValidTicket(_ticket); }
    bool Take(Ticket ticket)
    {
        if (Busy() || !ValidTicket(ticket))
            return false;
        _ticket = ticket;
        return true;
    }
    void Retire(Ticket ticket)
    {
        if (SameTicket(_ticket, ticket))
            _ticket = {};
    }
};
} // namespace DlssNr::VkPre

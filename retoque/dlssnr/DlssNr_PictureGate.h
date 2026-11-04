#pragma once

#include "DlssNr_Pictures.h"
#include "DlssNr_RenderSize.h"

#include <atomic>
#include <cstdint>

// Glue for the main-picture rule (DlssNr_Pictures.h): asked by each NR seam before it touches any NR state, with the
// colour allocation it reads. The feature and its output size come from the evaluate's render-geometry binding
// (DlssNr_RenderGeometry.h). An evaluate without a binding (NR off at the feature's creation, a replay, a caller
// outside the NGX inputs) is main, as every evaluate was before.
//
// Logging only: the including unit has the log macros (every unit starts with pch.h).
namespace DlssNr::Pictures
{
inline std::atomic<uint64_t> g_asideEvaluates { 0 };

inline const char* TookText(Took took)
{
    switch (took)
    {
    case Took::Vacant:
        return "the feature it followed was released";
    case Took::Larger:
        return "its picture is clearly larger";
    case Took::Quiet:
        return "the feature it followed stopped evaluating";
    case Took::Waited:
        return "the feature it followed was released and no successor came";
    default:
        return "";
    }
}

// Which picture this evaluate is. The first seam to ask judges it; a later seam of the same evaluate gets the same
// answer with took = No. `colour` reads the identity of the colour allocation, and is called only when there is an
// evaluate to judge: an unbound one (every evaluate of a game whose features predate NR, every replay) reads nothing.
template <class ColourFn> Judgement Ask(const NVSDK_NGX_Parameter* params, ColourFn&& colour)
{
    const auto* binding = RenderGeometry::active;
    if (binding == nullptr || params == nullptr || binding->params != params || binding->key == 0)
        return {};
    if (binding->judged)
        return { static_cast<Role>(binding->role), Took::No, 0, false, binding->epoch };

    auto display = ReadDisplaySize(params);
    if (display.width == 0 || display.height == 0)
        display = binding->display;
    const auto judgement =
        Instance().Judge({ binding->key, uint64_t(display.width) * display.height, static_cast<uintptr_t>(colour()) });
    binding->judged = true;
    binding->role = static_cast<uint8_t>(judgement.role);
    binding->epoch = judgement.epoch;

    [[maybe_unused]] const auto handle = static_cast<uint32_t>(binding->key); // the log's
    if (judgement.role == Role::Aside)
    {
        const auto count = g_asideEvaluates.fetch_add(1, std::memory_order_relaxed) + 1;
        if (judgement.firstAside)
            LOG_INFO("DLSS-NR: the game evaluates another DLSS picture (feature {}, {}x{}); NR follows feature {} and "
                     "leaves this one as it is",
                     handle, display.width, display.height, static_cast<uint32_t>(Instance().Main()));
        else if (count % 3600 == 0)
            LOG_INFO("DLSS-NR: {} evaluates of other DLSS pictures left as they are so far", count);
    }
    else if (NewPicture(judgement.took) ||
             (judgement.took == Took::Vacant && g_asideEvaluates.load(std::memory_order_relaxed) != 0))
    {
        LOG_INFO("DLSS-NR: NR now follows the game's DLSS feature {} ({}x{}): {}", handle, display.width,
                 display.height, TookText(judgement.took));
    }
    return judgement;
}

// The picture epoch of the evaluate this parameter block belongs to, for its frame (GatherGuideFrame): 0 when the rule
// has not judged it (no binding, another block) or left it aside. Read by the holder of the model's history under its
// own lock (EpochGate), so the frame that restarts is the new picture's whatever order evaluates reach that lock in.
inline uint64_t EvaluateEpoch(const NVSDK_NGX_Parameter* params)
{
    const auto* binding = RenderGeometry::active;
    if (binding == nullptr || params == nullptr || binding->params != params || !binding->judged)
        return 0;
    return binding->epoch;
}
} // namespace DlssNr::Pictures

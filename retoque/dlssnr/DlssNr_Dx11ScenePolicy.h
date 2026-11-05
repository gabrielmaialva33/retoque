#pragma once

#include <cstdint>

namespace DlssNr::Dx11Scene
{
constexpr uint64_t MaxBytes = 512ull * 1024 * 1024;
constexpr bool Eligible(bool startup, bool enabled, bool explicitNative, unsigned hook, int stage)
{
    return startup && enabled && explicitNative && hook == 1 && stage == 0;
}
constexpr bool Complete(uint64_t completed, uint64_t wanted) { return completed != UINT64_MAX && completed >= wanted; }
constexpr const char* QueueRefusal(uintptr_t nr, uintptr_t consumer, uintptr_t present)
{
    if (!nr || !consumer || !present)
        return "Dx11SceneStage: QueueIdentityUnavailable";
    if (nr == consumer || nr == present)
        return "Dx11SceneStage: SharedVkQueue";
    return nullptr;
}
} // namespace DlssNr::Dx11Scene

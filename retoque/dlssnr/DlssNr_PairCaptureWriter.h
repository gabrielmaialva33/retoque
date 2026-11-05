// CPU-only hand-off to the process-wide pair writer. Callbacks never run with the writer queue lock held.
#pragma once
#include "DlssNr_PairCapture.h"
#include <functional>
#include <memory>
namespace DlssNr::PairCapture
{
struct CpuJob
{
    FrameRecordInputs frame;
    UtcStamp stamp;
    std::string gpu = "unknown";
    std::string driver = "unknown";
    uint64_t bytes = 0;
    // Allocates CPU snapshots outside the route lock, then copies under it, after revalidating the device.
    std::function<bool(std::vector<uint8_t> (&)[2])> prepare;
    std::function<void(bool, const char*, std::string)> finish;
};
bool EnqueueCpuPair(std::shared_ptr<CpuJob> job);
bool DirectoryFailed();
// One active request across both APIs; the writer, filename sequence and folder cap are shared.
enum class Api : uint8_t
{
    None,
    Dx12,
    Vulkan
};
inline std::mutex brokerMutex;
inline std::atomic<Api> owner { Api::None };
inline std::atomic<Api> lastApi { Api::Dx12 };
inline bool Claim(Api api)
{
    Api idle = Api::None;
    if (!owner.compare_exchange_strong(idle, api))
        return false;
    lastApi.store(api);
    return true;
}
inline void Relinquish(Api api) { owner.compare_exchange_strong(api, Api::None); }
template <class B> void ReleaseIfIdle(Api api, Session<B>& session)
{
    std::lock_guard<std::mutex> lock(brokerMutex);
    if (session.Current() == Phase::Idle)
        Relinquish(api);
}
} // namespace DlssNr::PairCapture

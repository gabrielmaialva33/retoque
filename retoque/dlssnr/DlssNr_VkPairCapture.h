// Host-testable rules for the native Vulkan pair capture. No API calls or GPU-owning destructors.
#pragma once
#include <dlssnr/DlssNr_ImageFile.h>
#include <dlssnr/DlssNr_VkRecording.h>
#include <atomic>
#include <cstdint>
#include <memory>
namespace DlssNr::VkPairCapture
{
inline constexpr uint64_t kBudgetBytes = 512ull * 1024 * 1024;
struct Plan
{
    ImageFile::Plan image {};
    uint64_t dataBytes = 0;
    explicit operator bool() const { return dataBytes != 0; }
};
inline Plan PlanFor(uint32_t width, uint32_t height, uint32_t format, bool linearHdr, uint64_t maxStorageRange)
{
    // Vulkan format numbers, not DXGI. Storage is always sampled RGBA32F, never a raw image copy.
    if (width == 0 || height == 0 || (format != 37 && format != 44 && format != 97 && format != 109) ||
        (linearHdr && format != 97 && format != 109))
        return {};
    const uint64_t pixels = uint64_t(width) * height;
    // Two readbacks plus two writer snapshots, and the two markers. Check before multiplication.
    if (pixels > (kBudgetBytes - 16) / 64 || pixels > maxStorageRange / 16)
        return {};
    return { ImageFile::PlanFor(ImageFile::Dxgi::R32G32B32A32_FLOAT, linearHdr), pixels * 16 };
}
class Budget
{
    uint64_t used_ = 0;

  public:
    bool Reserve(uint64_t bytes)
    {
        if (bytes > kBudgetBytes - used_)
            return false;
        used_ += bytes;
        return true;
    }
    void Release(uint64_t bytes) { used_ -= bytes; }
    uint64_t Used() const { return used_; }
};
inline bool Admissible(const VkRecording::Ticket& t)
{
    return t.device != 0 && t.buffer != 0 && t.epoch != 0 && t.reuse == VkRecording::Reuse::Serial;
}
struct Evidence
{
    std::atomic<bool> retired { false };
    std::atomic<bool> invalid { false };
    uint64_t cookie = 0;
};
struct Retirement
{
    std::shared_ptr<Evidence> evidence;
    explicit Retirement(std::shared_ptr<Evidence> e) : evidence(std::move(e)) {}
    ~Retirement() { evidence->retired.store(true, std::memory_order_release); }
};
enum class Result
{
    Pending,
    Readable,
    Discarded
};
template <class Marker> Result Inspect(const Evidence& e, Marker marker)
{
    if (e.invalid.load(std::memory_order_acquire))
        return Result::Discarded;
    if (!e.retired.load(std::memory_order_acquire))
        return Result::Pending;
    return marker() == e.cookie ? Result::Readable : Result::Discarded;
}
} // namespace DlssNr::VkPairCapture

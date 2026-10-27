#pragma once

#include <shaders/output_scaling/OS_Vk.h>
#include <dlssnr/DlssNr_VkRecording.h>

// NR owns this recording-safe RGBA16F resampler. OS_Vk supplies the shared resource layouts;
// NR replaces its unused pipeline with a format-compatible variant of the selected filter.
// Output Scaling and Magnifier continue to use OS_Vk and its original shaders directly.
class DlssNr_Scaler_Vk final : public OS_Vk
{
    static constexpr uint32_t kSlots = 3;
    const Scaler _scaler;
    const bool _upsample;
    VkDeviceSize _constantSize = 0;
    VkDeviceSize _slotStride = 0;
    uint32_t _maxGroupsX = 0;
    uint32_t _maxGroupsY = 0;
    DlssNr::VkRecording::SlotPool _slots { kSlots };

  public:
    struct Batch
    {
        std::shared_ptr<DlssNr::VkRecording::SlotPool::Lease> lease;
        VkCommandBuffer commandBuffer;
        const DlssNr_Scaler_Vk* owner;
        DlssNr::VkRecording::Ticket ticket;
        uint32_t next = 0;

        Batch(std::shared_ptr<DlssNr::VkRecording::SlotPool::Lease> slots, VkCommandBuffer cmd,
              const DlssNr_Scaler_Vk* pass, DlssNr::VkRecording::Ticket recording)
            : lease(std::move(slots)), commandBuffer(cmd), owner(pass), ticket(recording)
        {
        }
        Batch(const Batch&) = delete;
        Batch& operator=(const Batch&) = delete;
        Batch(Batch&&) = default;
        Batch& operator=(Batch&&) = default;
    };

    DlssNr_Scaler_Vk(std::string InName, VkDevice InDevice, VkPhysicalDevice InPhysicalDevice, bool InUpsample,
                     Scaler InScaler);

    // Reserve all passes before the frame records any GPU command. Registry pins retain the lease
    // through End, submission and completion: executable command buffers can be submitted again.
    std::optional<Batch> Reserve(VkCommandBuffer cmd, uint32_t count);
    bool Dispatch(VkCommandBuffer InCmdList, Batch& batch, const VkImageInfo& InResourceView,
                  const VkImageInfo& OutResourceView);
};

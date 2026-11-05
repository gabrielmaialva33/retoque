#pragma once
#include "DlssNr_PairCaptureWriter.h"
#include "DlssNr_VkRecording.h"
#include <vulkan/vulkan.h>
#include <mutex>
namespace DlssNr
{
struct PairCaptureStatus;
// The after-upscale route's existing mutex; writer callbacks take it only for validation and CPU snapshots.
std::mutex& PairCaptureVkMutex();
namespace PairCapture
{
struct VkFrame
{
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkImageSubresourceRange range {};
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkRecording::Ticket recording {};
    FrameRecordInputs facts;
    UtcStamp stamp;
    const char* refusal = nullptr;
};
bool RequestVk(const char** why);
PairCaptureStatus StatusVk();
bool WantedVk();
// All *Locked calls require g_vkMutex. Never call Vulkan from a registry lease destructor.
void PollVkLocked();
bool BeginVkLocked(VkFrame frame);
void CommitVkLocked(VkCommandBuffer cmd);
void CancelVkLocked();
// BEFORE registry.ForgetDevice; drained=true means the existing transport proved every queue idle.
void DeviceGoneVkLocked(VkDevice device, bool drained);
} // namespace PairCapture
} // namespace DlssNr

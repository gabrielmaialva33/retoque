#include "pch.h"
#include <dlssnr/DlssNr_VkPreRuntime.h>
#include <dlssnr/DlssNr_VkPreNgx.h>
#include <cassert>
#include <iostream>
#include <string>
#include <unordered_map>
#include <array>
#include <utility>
using namespace DlssNr::VkPre;
std::unordered_map<std::string, PFN_vkVoidFunction> functions;
std::unordered_map<void*, void*> detours;
std::unordered_map<std::string, PFN_vkVoidFunction> exportOverrides;
int driverCalls = 0, attachCalls = 0;
void* failAttachment = nullptr;
bool lifecycleObserved = true;
namespace Recording = DlssNr::VkRecording;
VkResult driverResult = VK_SUCCESS;
Config cfg;
Config* Config::Instance() { return &cfg; }
PFN_vkVoidFunction VulkanHooks::GetDeviceProcAddr(VkDevice, const char* name) { return functions[name]; }
void* exported(void*, const char* name)
{
    return reinterpret_cast<void*>(exportOverrides.count(name) ? exportOverrides.at(name) : functions[name]);
}
LONG DetourTransactionBegin() { return 0; }
LONG DetourUpdateThread(void*) { return 0; }
LONG DetourAttach(void** address, void* hook)
{
    ++attachCalls;
    if (*address == failAttachment)
        return 1;
    detours[*address] = hook;
    return 0;
}
LONG DetourTransactionCommit() { return 0; }
LONG DetourTransactionAbort() { return 0; }
VkResult VKAPI_CALL createImage(VkDevice, const VkImageCreateInfo*, const VkAllocationCallbacks*, VkImage* image)
{
    ++driverCalls;
    if (driverResult == VK_SUCCESS)
        *image = reinterpret_cast<VkImage>(30);
    return driverResult;
}
VkResult VKAPI_CALL createView(VkDevice, const VkImageViewCreateInfo*, const VkAllocationCallbacks*, VkImageView* view)
{
    ++driverCalls;
    if (driverResult == VK_SUCCESS)
        *view = reinterpret_cast<VkImageView>(40);
    return driverResult;
}
void VKAPI_CALL destroyDevice(VkDevice, const VkAllocationCallbacks*) { ++driverCalls; }
VkResult VKAPI_CALL createPool(VkDevice, const VkCommandPoolCreateInfo*, const VkAllocationCallbacks*,
                               VkCommandPool* out)
{
    if (driverResult == VK_SUCCESS)
        *out = reinterpret_cast<VkCommandPool>(10);
    return driverResult;
}
VkResult VKAPI_CALL allocate(VkDevice d, const VkCommandBufferAllocateInfo* i, VkCommandBuffer* out)
{
    if (driverResult == VK_SUCCESS)
    {
        *out = reinterpret_cast<VkCommandBuffer>(20);
        Recording::Recordings().Allocate((uint64_t) d, (uint64_t) i->commandPool, { 20 });
    }
    return driverResult;
}
VkResult VKAPI_CALL begin(VkCommandBuffer c, const VkCommandBufferBeginInfo* i)
{
    if (driverResult == VK_SUCCESS && lifecycleObserved)
        Recording::Recordings().Begin((uint64_t) c, Recording::ReuseOf(true, i->flags));
    return driverResult;
}
VkResult VKAPI_CALL end(VkCommandBuffer c)
{
    if (driverResult == VK_SUCCESS)
        Recording::Recordings().End((uint64_t) c);
    return driverResult;
}
void VKAPI_CALL barrier2(VkCommandBuffer, const VkDependencyInfo*) { ++driverCalls; }
void VKAPI_CALL bindPipeline(VkCommandBuffer, VkPipelineBindPoint, VkPipeline) { ++driverCalls; }
void VKAPI_CALL push(VkCommandBuffer, VkPipelineLayout, VkShaderStageFlags, uint32_t, uint32_t, const void*)
{
    ++driverCalls;
}
void VKAPI_CALL bindSets(VkCommandBuffer, VkPipelineBindPoint, VkPipelineLayout, uint32_t, uint32_t,
                         const VkDescriptorSet*, uint32_t, const uint32_t*)
{
    ++driverCalls;
}
void VKAPI_CALL beginRendering(VkCommandBuffer, const VkRenderingInfo*) { ++driverCalls; }
void VKAPI_CALL endRendering(VkCommandBuffer) { ++driverCalls; }
template <class F> F hook(F original) { return reinterpret_cast<F>(detours.at(reinterpret_cast<void*>(original))); }
// Distinct addresses stand for device commands. These cases only install hooks, never call them.
template <size_t I> void VKAPI_CALL addressOnly() {}
template <size_t... I> auto addresses(std::index_sequence<I...>)
{
    return std::array<PFN_vkVoidFunction, sizeof...(I)> { addressOnly<I>... };
}
int coverageCase(const std::string& request)
{
    const auto separator = request.find(':');
    const std::string scenario = request.substr(0, separator);
    const std::string selected = separator == std::string::npos ? "vkCmdBeginRendering" : request.substr(separator + 1);
    const char* core[] = { "vkCreateImage",        "vkDestroyImage",           "vkCreateImageView",
                           "vkDestroyImageView",   "vkCreateCommandPool",      "vkDestroyCommandPool",
                           "vkResetCommandPool",   "vkAllocateCommandBuffers", "vkFreeCommandBuffers",
                           "vkBeginCommandBuffer", "vkEndCommandBuffer",       "vkResetCommandBuffer",
                           "vkDestroyDevice",      "vkCmdPipelineBarrier",     "vkCmdPipelineBarrier2",
                           "vkCmdBeginRenderPass", "vkCmdEndRenderPass",       "vkCmdBeginRenderPass2",
                           "vkCmdEndRenderPass2",  "vkCmdBeginRendering",      "vkCmdEndRendering",
                           "vkCmdExecuteCommands", "vkCmdWaitEvents",          "vkCmdWaitEvents2" };
    auto targets = addresses(std::make_index_sequence<64> {});
    size_t n = 0;
    for (const char* name : core)
        functions[name] = targets[n++];
    bool complete = true;
    if (scenario == "all-addresses")
    {
        for (const char* name : { "vkCmdPipelineBarrier2", "vkCmdBeginRenderPass2", "vkCmdEndRenderPass2",
                                  "vkCmdBeginRendering", "vkCmdEndRendering", "vkCmdWaitEvents2" })
        {
            const auto alias = std::string(name) + "KHR";
            functions[alias] = targets[n++];
            exportOverrides[name] = targets[n++];
            exportOverrides[alias] = targets[n++];
        }
    }
    else if (scenario == "export-only")
    {
        functions.erase(selected);
        exportOverrides[selected] = targets[n++];
        exportOverrides[selected + "KHR"] = targets[n++];
        complete = false; // Loader stubs alone do not establish device-command availability.
    }
    else if (scenario == "core-missing")
    {
        functions.erase(selected);
        complete = false;
    }
    else if (scenario == "alias-only")
    {
        for (const char* name : { "vkCmdPipelineBarrier2", "vkCmdBeginRenderPass2", "vkCmdEndRenderPass2",
                                  "vkCmdBeginRendering", "vkCmdEndRendering", "vkCmdWaitEvents2" })
        {
            functions[std::string(name) + "KHR"] = functions.at(name);
            functions.erase(name);
        }
    }
    else if (scenario == "alias-failure")
    {
        functions[selected + "KHR"] = targets[n++];
        failAttachment = reinterpret_cast<void*>(functions.at(selected + "KHR"));
        complete = false;
    }
    else if (scenario == "core-failure")
    {
        functions["vkCmdBeginRenderingKHR"] = targets[n++];
        failAttachment = reinterpret_cast<void*>(functions.at("vkCmdBeginRendering"));
        complete = false;
    }
    else if (scenario == "export-failure")
    {
        exportOverrides["vkCmdBeginRendering"] = targets[n++];
        failAttachment = reinterpret_cast<void*>(exportOverrides.at("vkCmdBeginRendering"));
        complete = false;
    }
    else if (scenario == "diagnostic-failure")
    {
        functions["vkCmdBindDescriptorBuffersEXT"] = targets[n++];
        failAttachment = reinterpret_cast<void*>(functions.at("vkCmdBindDescriptorBuffersEXT"));
    }
    else
        assert(scenario == "core-only");
    cfg.DlssNrEnabled.value = true;
    cfg.DlssNrStage.value = 1;
    auto device = reinterpret_cast<VkDevice>(1);
    Runtime::Install(device, nullptr, VK_API_VERSION_1_3);
    const auto status = Runtime::Status(device);
    assert(status.complete == complete);
    if (complete)
        assert(status.requiredMissing == 0 && status.requiredFailed == 0);
    if (scenario == "alias-failure" || scenario == "core-failure" || scenario == "export-failure")
        assert(status.requiredFailed > 0);
    if (scenario == "diagnostic-failure")
        assert(status.failed > 0);
    if (scenario == "export-only")
        assert(status.requiredMissing > 0);
    if (scenario == "all-addresses")
    {
        assert(status.failed == 0);
        for (size_t i = 0; i < n; ++i)
            assert(detours.count(reinterpret_cast<void*>(targets[i])) == 1);
    }
    std::cout << "PASS: runtime coverage " << scenario << "\n";
    return 0;
}
int main(int argc, char** argv)
{
    if (argc == 2)
        return coverageCase(argv[1]);
    auto device = reinterpret_cast<VkDevice>(1);
    assert(Runtime::Facts() == nullptr);
    functions["vkCreateImage"] = reinterpret_cast<PFN_vkVoidFunction>(createImage);
    functions["vkCreateImageView"] = reinterpret_cast<PFN_vkVoidFunction>(createView);
    functions["vkDestroyDevice"] = reinterpret_cast<PFN_vkVoidFunction>(destroyDevice);
    functions["vkCreateCommandPool"] = reinterpret_cast<PFN_vkVoidFunction>(createPool);
    functions["vkAllocateCommandBuffers"] = reinterpret_cast<PFN_vkVoidFunction>(allocate);
    functions["vkBeginCommandBuffer"] = reinterpret_cast<PFN_vkVoidFunction>(begin);
    functions["vkEndCommandBuffer"] = reinterpret_cast<PFN_vkVoidFunction>(end);
    functions["vkCmdPipelineBarrier2"] = reinterpret_cast<PFN_vkVoidFunction>(barrier2);
    functions["vkCmdPipelineBarrier2KHR"] = reinterpret_cast<PFN_vkVoidFunction>(barrier2);
    functions["vkCmdBindPipeline"] = reinterpret_cast<PFN_vkVoidFunction>(bindPipeline);
    functions["vkCmdPushConstants"] = reinterpret_cast<PFN_vkVoidFunction>(push);
    functions["vkCmdBindDescriptorSets"] = reinterpret_cast<PFN_vkVoidFunction>(bindSets);
    functions["vkCmdBeginRendering"] = reinterpret_cast<PFN_vkVoidFunction>(beginRendering);
    functions["vkCmdBeginRenderingKHR"] = reinterpret_cast<PFN_vkVoidFunction>(beginRendering);
    functions["vkCmdEndRendering"] = reinterpret_cast<PFN_vkVoidFunction>(endRendering);
    Runtime::Install(device);
    assert(attachCalls == 0);
    assert(Runtime::Facts() == nullptr);
    cfg.DlssNrEnabled.value = true;
    Runtime::Install(device);
    assert(attachCalls == 0);
    assert(Runtime::Facts() == nullptr);
    cfg.DlssNrStage.value = 1;
    Runtime::Install(device);
    assert(attachCalls == 13);
    assert(Runtime::Status(device).missing > 0 && !Runtime::Status(device).complete);
    Runtime::Install(device);
    assert(attachCalls == 13); // same device/export addresses, not hooked twice
    assert(!Runtime::Status(device).formatless && !Runtime::Status(device).formatFlags2);
    VkPhysicalDeviceFeatures enabled {};
    enabled.shaderStorageImageWriteWithoutFormat = true;
    const char* extension = VK_KHR_FORMAT_FEATURE_FLAGS_2_EXTENSION_NAME;
    VkDeviceCreateInfo creation {};
    creation.pEnabledFeatures = &enabled;
    creation.enabledExtensionCount = 1;
    creation.ppEnabledExtensionNames = &extension;
    Runtime::Install(device, &creation, VK_API_VERSION_1_1);
    assert(Runtime::Status(device).formatless && Runtime::Status(device).formatFlags2 && attachCalls == 13);
    assert(Runtime::Status(device).requiredMissing > 0);
    assert(Runtime::Status(device).missing > Runtime::Status(device).requiredMissing); // diagnostic targets excluded

    VkImageCreateInfo info {};
    info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    info.extent = { 100, 50, 1 };
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
    VkImage image {};
    driverResult = VK_ERROR_OUT_OF_DEVICE_MEMORY;
    assert(hook(createImage)(device, &info, nullptr, &image) == driverResult && driverCalls == 1);
    driverResult = VK_SUCCESS;
    assert(hook(createImage)(device, &info, nullptr, &image) == VK_SUCCESS && driverCalls == 2);
    VkImageViewCreateInfo vi {};
    vi.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    vi.image = image;
    vi.format = info.format;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    VkImageView view {};
    assert(hook(createView)(device, &vi, nullptr, &view) == VK_SUCCESS);
    auto fact = Runtime::Facts()->ImageForView(1, 40);
    assert(fact && fact->width == 100 && fact->height == 50);
    VkCommandPoolCreateInfo pi {};
    pi.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
    pi.queueFamilyIndex = 4;
    VkCommandPool pool {};
    assert(hook(createPool)(device, &pi, nullptr, &pool) == VK_SUCCESS);
    VkCommandBufferAllocateInfo ai {};
    ai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    ai.commandPool = pool;
    ai.commandBufferCount = 1;
    ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    VkCommandBuffer command {};
    assert(hook(allocate)(device, &ai, &command) == VK_SUCCESS);
    VkCommandBufferBeginInfo bi {};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    assert(hook(begin)(command, &bi) == VK_SUCCESS);
    assert(Runtime::Facts()->Command(20)->family == 4 && Runtime::Facts()->Command(20)->oneTime);
    auto ticket = Runtime::CurrentTicket(device, command);
    assert(ticket && ticket->epoch == Recording::Recordings().Current(1, 20)->epoch);
    driverResult = VK_ERROR_OUT_OF_HOST_MEMORY;
    assert(hook(begin)(command, &bi) == driverResult);
    assert(Runtime::CurrentTicket(device, command)->epoch == ticket->epoch);
    driverResult = VK_SUCCESS;
    lifecycleObserved = false;
    assert(hook(begin)(command, &bi) == VK_SUCCESS);
    assert(!Runtime::CurrentTicket(device, command)); // unchanged old ticket is not this Begin
    Recording::Recordings().End(20);
    assert(hook(begin)(command, &bi) == VK_SUCCESS);
    Recording::Recordings().Begin(20, Recording::Reuse::Serial); // observer inside an outer lifecycle hook
    assert(!Runtime::CurrentTicket(device, command));            // no late repair
    lifecycleObserved = true;
    assert(hook(begin)(command, &bi) == VK_SUCCESS);
    assert(Runtime::CurrentTicket(device, command));
    Recording::Recordings().Begin(20, Recording::Reuse::Serial); // duplicate/outer lifecycle observation
    assert(!Runtime::CurrentTicket(device, command));
    assert(hook(begin)(command, &bi) == VK_SUCCESS);
    assert(Runtime::CurrentTicket(device, command));
    VkImageMemoryBarrier2 barrier {};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.image = image;
    barrier.subresourceRange = vi.subresourceRange;
    barrier.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barrier.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    VkDependencyInfo dependency {};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    hook(barrier2)(command, &dependency);
    assert(Runtime::Facts()->Layout(20, 40) == 5);
    hook(bindPipeline)(command, VK_PIPELINE_BIND_POINT_COMPUTE, reinterpret_cast<VkPipeline>(77));
    uint32_t bytes = 123;
    hook(push)(command, reinterpret_cast<VkPipelineLayout>(71), VK_SHADER_STAGE_COMPUTE_BIT, 0, 4, &bytes);
    VkDescriptorSet sets[] { reinterpret_cast<VkDescriptorSet>(81) };
    uint32_t offsets[] { 64 };
    hook(bindSets)(command, VK_PIPELINE_BIND_POINT_COMPUTE, reinterpret_cast<VkPipelineLayout>(71), 0, 1, sets, 1,
                   offsets);
    auto commandFacts = Runtime::Facts()->Command(20);
    assert(commandFacts->pipelines[1] == 77 && commandFacts->push[0].bytes[0] == 123);
    assert(commandFacts->descriptors[0].offsets[0] == 64 && commandFacts->descriptors[0].sets[0] == 81);
    VkRenderingInfo rendering {};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    hook(beginRendering)(command, &rendering);
    assert(Runtime::Facts()->Command(20)->inside);
    assert(!Runtime::Facts()->Layout(20, 40));
    hook(endRendering)(command);
    assert(Runtime::Facts()->Layout(20, 40) == 5);
    driverResult = VK_ERROR_OUT_OF_HOST_MEMORY;
    assert(hook(end)(command) == driverResult);
    assert(Runtime::Facts()->Command(20)->recording);
    driverResult = VK_SUCCESS;
    assert(hook(end)(command) == VK_SUCCESS);
    assert(!Runtime::Facts()->Command(20)->recording);
    assert(!Runtime::CurrentTicket(device, command));
    NVSDK_NGX_Handle ngxHandle { 27 };
    auto* ngxPointer = &ngxHandle;
    RememberNgxFeature(device, NVSDK_NGX_Feature_SuperSampling, &ngxPointer, NVSDK_NGX_Result_Success);
    assert(Runtime::Facts()->FindFeature(1, 27)->kind == Feature::SuperSampling);
    RememberNgxFeature(device, NVSDK_NGX_Feature_RayReconstruction, &ngxPointer, NVSDK_NGX_Result_Success);
    assert(Runtime::Facts()->FindFeature(1, 27)->kind == Feature::RayReconstruction);
    RememberNgxFeature(device, NVSDK_NGX_Feature_FrameGeneration, &ngxPointer, NVSDK_NGX_Result_Success);
    assert(Runtime::Facts()->FindFeature(1, 27)->kind == Feature::FrameGeneration);
    RememberNgxFeature(device, static_cast<NVSDK_NGX_Feature>(99), &ngxPointer, NVSDK_NGX_Result_Success);
    assert(Runtime::Facts()->FindFeature(1, 27)->kind == Feature::Unknown);
    RememberNgxFeature(device, NVSDK_NGX_Feature_SuperSampling, &ngxPointer, NVSDK_NGX_Result_Fail);
    assert(Runtime::Facts()->FindFeature(1, 27)->kind == Feature::Unknown);
    Runtime::RememberFeature(device, 7, Feature::SuperSampling, true, 107);
    auto f = Runtime::Facts()->FindFeature(1, 7);
    assert(f && f->kind == Feature::SuperSampling);
    Runtime::RememberFeature(device, 8, Feature::SuperSampling, false);
    assert(!Runtime::Facts()->FindFeature(1, 8));
    Runtime::ForgetFeature(107, 7);
    assert(!Runtime::Facts()->FindFeature(1, 7));
    Runtime::RememberFeature(device, 7, Feature::SuperSampling, true, 107);
    hook(destroyDevice)(device, nullptr);
    assert(!Runtime::Facts()->FindFeature(1, 7));
    assert(!Runtime::Facts()->ImageForView(1, 40));
    cfg.DlssNrStage.value = 0;
    hook(createImage)(device, &info, nullptr, &image);
    assert(!Runtime::Uninterrupted());
    cfg.DlssNrStage.value = 1;
    Runtime::RememberFeature(device, 90, Feature::SuperSampling, true);
    assert(!Runtime::Facts()->FindFeature(1, 90));
    assert(!Runtime::Facts()->ImageForView(1, 40));
    std::cout << "PASS: production passive hooks with fake driver; off gates, exact forwarding and identity\n";
}

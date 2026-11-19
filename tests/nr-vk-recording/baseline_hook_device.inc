// Frozen installation boundary from 44ac22cb; default detour/lookup equivalence oracle.
void BaselineHookDevice(VkDevice device)
{
    if (g_getDeviceProcAddr == nullptr)
        return;

    const auto get = [device](const char* name) { return g_getDeviceProcAddr(device, name); };

    const PFN_vkVoidFunction destroyDevice = get("vkDestroyDevice");
    const PFN_vkVoidFunction getQueue = get("vkGetDeviceQueue");
    const PFN_vkVoidFunction getQueue2 = get("vkGetDeviceQueue2");
    const PFN_vkVoidFunction createSwapchain = get("vkCreateSwapchainKHR");
    const PFN_vkVoidFunction destroySwapchain = get("vkDestroySwapchainKHR");
    const PFN_vkVoidFunction getImages = get("vkGetSwapchainImagesKHR");
    const PFN_vkVoidFunction acquire = get("vkAcquireNextImageKHR");
    const PFN_vkVoidFunction acquire2 = get("vkAcquireNextImage2KHR");
    const QueueCallTargets submit = QueueCall(device, "vkQueueSubmit");
    const QueueCallTargets submit2 = QueueCall(device, "vkQueueSubmit2");
    const QueueCallTargets submit2Khr = QueueCall(device, "vkQueueSubmit2KHR");
    const QueueCallTargets bindSparse = QueueCall(device, "vkQueueBindSparse");
    const QueueCallTargets waitIdle = QueueCall(device, "vkQueueWaitIdle");
    const QueueCallTargets recordingAllocate = QueueCall(device, "vkAllocateCommandBuffers");
    const QueueCallTargets recordingBegin = QueueCall(device, "vkBeginCommandBuffer");
    const QueueCallTargets recordingEnd = QueueCall(device, "vkEndCommandBuffer");
    const QueueCallTargets recordingReset = QueueCall(device, "vkResetCommandBuffer");
    const QueueCallTargets recordingFree = QueueCall(device, "vkFreeCommandBuffers");
    const QueueCallTargets recordingResetPool = QueueCall(device, "vkResetCommandPool");
    const QueueCallTargets recordingDestroyPool = QueueCall(device, "vkDestroyCommandPool");

    std::lock_guard lock(g_installMutex);
    AttachTarget(g_destroyDevice, kDestroyDeviceHooks, destroyDevice, "vkDestroyDevice");
    AttachTarget(g_getQueue, kGetQueueHooks, getQueue, "vkGetDeviceQueue");
    AttachTarget(g_getQueue2, kGetQueue2Hooks, getQueue2, "vkGetDeviceQueue2");
    AttachTarget(g_createSwapchain, kCreateSwapchainHooks, createSwapchain, "vkCreateSwapchainKHR");
    AttachTarget(g_destroySwapchain, kDestroySwapchainHooks, destroySwapchain, "vkDestroySwapchainKHR");
    AttachTarget(g_getImages, kGetImagesHooks, getImages, "vkGetSwapchainImagesKHR");
    AttachTarget(g_acquire, kAcquireHooks, acquire, "vkAcquireNextImageKHR");
    AttachTarget(g_acquire2, kAcquire2Hooks, acquire2, "vkAcquireNextImage2KHR");
    AttachQueueCall(submit, g_submit, kSubmitHooks, "vkQueueSubmit");
    AttachQueueCall(submit2, g_submit2, kSubmit2Hooks, "vkQueueSubmit2");
    AttachQueueCall(submit2Khr, g_submit2, kSubmit2Hooks, "vkQueueSubmit2KHR");
    AttachQueueCall(bindSparse, g_bindSparse, kBindSparseHooks, "vkQueueBindSparse");
    AttachQueueCall(waitIdle, g_waitIdle, kWaitIdleHooks, "vkQueueWaitIdle");
    AttachQueueCall(recordingAllocate, g_recordingAllocate, kRecordingAllocateHooks, "vkAllocateCommandBuffers");
    AttachQueueCall(recordingBegin, g_recordingBegin, kRecordingBeginHooks, "vkBeginCommandBuffer");
    AttachQueueCall(recordingEnd, g_recordingEnd, kRecordingEndHooks, "vkEndCommandBuffer");
    AttachQueueCall(recordingReset, g_recordingReset, kRecordingResetHooks, "vkResetCommandBuffer");
    AttachQueueCall(recordingFree, g_recordingFree, kRecordingFreeHooks, "vkFreeCommandBuffers");
    AttachQueueCall(recordingResetPool, g_recordingResetPool, kRecordingResetPoolHooks, "vkResetCommandPool");
    AttachQueueCall(recordingDestroyPool, g_recordingDestroyPool, kRecordingDestroyPoolHooks, "vkDestroyCommandPool");
}

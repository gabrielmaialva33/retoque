// Fake Vulkan driver, ImGui backend, MenuOverlayBase and Win32 windows for the production
// menu_overlay_vk.cpp, appended to the same translation unit. Every call the overlay makes is recorded
// in an ordered event log; contract violations that the real component turns into a crash, a leak or
// undefined behaviour are recorded as faults instead.

static std::mutex g_evMutex;
static std::vector<std::string> g_events;
static std::vector<std::string> g_faults;
static std::map<uint64_t, std::string> g_names;

static uint64_t U(const void* p) { return (uint64_t) (uintptr_t) p; }

static std::string N(const void* handle)
{
    std::scoped_lock lock(g_evMutex);
    auto it = g_names.find(U(handle));
    if (it != g_names.end())
        return it->second;
    char text[32];
    std::snprintf(text, sizeof(text), "#%llx", (unsigned long long) U(handle));
    return text;
}

void Ev(const std::string& event)
{
    std::scoped_lock lock(g_evMutex);
    g_events.push_back(event);
}

static void Fault(const std::string& what)
{
    Ev("FAULT " + what);
    std::scoped_lock lock(g_evMutex);
    g_faults.push_back(what);
}

void FakeLog(const char* level, const char* format) { Ev(std::string("log ") + level + " " + format); }

static size_t Mark()
{
    std::scoped_lock lock(g_evMutex);
    return g_events.size();
}

static std::vector<std::string> Since(size_t mark)
{
    std::scoped_lock lock(g_evMutex);
    return std::vector<std::string>(g_events.begin() + (long) mark, g_events.end());
}

static std::atomic<uint64_t> g_nextHandle { 0x1000 };

template <class T> static T Mint(const std::string& name = {})
{
    auto value = g_nextHandle.fetch_add(0x10);
    if (!name.empty())
    {
        std::scoped_lock lock(g_evMutex);
        g_names[value] = name;
    }
    return reinterpret_cast<T>((uintptr_t) value);
}

// ---- Win32 windows ------------------------------------------------------------------------------

static std::map<HWND, bool> g_windowAlive;

static HWND MakeWindow(const std::string& name)
{
    auto hwnd = Mint<HWND>(name);
    std::scoped_lock lock(g_evMutex);
    g_windowAlive[hwnd] = true;
    return hwnd;
}

static void DestroyWindowFake(HWND hwnd)
{
    std::scoped_lock lock(g_evMutex);
    g_windowAlive[hwnd] = false;
}

BOOL IsWindow(HWND hwnd)
{
    std::scoped_lock lock(g_evMutex);
    auto it = g_windowAlive.find(hwnd);
    return it != g_windowAlive.end() && it->second;
}

// ---- objects the overlay owns -------------------------------------------------------------------

static std::map<uint64_t, std::string> g_live;
static std::map<uint64_t, uint64_t> g_commandBufferPool;

static void Live(const void* handle, const char* kind)
{
    std::scoped_lock lock(g_evMutex);
    g_live[U(handle)] = kind;
}

static void Dead(const void* handle, const char* kind)
{
    {
        std::scoped_lock lock(g_evMutex);
        auto it = g_live.find(U(handle));
        if (it != g_live.end() && it->second == kind)
        {
            g_live.erase(it);
            return;
        }
    }
    Fault(std::string("destroy of a ") + kind + " that is not alive");
}

static size_t LiveCount()
{
    std::scoped_lock lock(g_evMutex);
    return g_live.size();
}

// Nth call (1-based) of an entry point fails. Counted per case.
static std::map<std::string, int> g_failAt;
static std::map<std::string, int> g_calls;
static VkResult g_idleResult = VK_SUCCESS;

static bool Fails(const char* name)
{
    std::scoped_lock lock(g_evMutex);
    int n = ++g_calls[name];
    auto it = g_failAt.find(name);
    return it != g_failAt.end() && it->second == n;
}

// ---- the game's swapchains ----------------------------------------------------------------------

static std::map<VkSwapchainKHR, std::vector<VkImage>> g_swapchainImages;

static std::map<std::pair<VkDevice, uint64_t>, VkQueue> g_queues;

static VkQueue QueueOf(VkDevice device, uint32_t family, uint32_t index)
{
    auto key = std::make_pair(device, ((uint64_t) family << 32) | index);
    {
        std::scoped_lock lock(g_evMutex);
        auto it = g_queues.find(key);
        if (it != g_queues.end())
            return it->second;
    }
    auto queue = Mint<VkQueue>("queue" + std::to_string(family) + "." + std::to_string(index));
    std::scoped_lock lock(g_evMutex);
    return g_queues.emplace(key, queue).first->second;
}

// ---- Vulkan entry points ------------------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL vkGetSwapchainImagesKHR(VkDevice, VkSwapchainKHR swapchain, uint32_t* count,
                                                       VkImage* images)
{
    std::vector<VkImage> list;
    {
        std::scoped_lock lock(g_evMutex);
        auto it = g_swapchainImages.find(swapchain);
        if (it == g_swapchainImages.end())
        {
            g_faults.push_back("vkGetSwapchainImagesKHR on a swapchain the driver never created");
            return VK_ERROR_SURFACE_LOST_KHR;
        }
        list = it->second;
    }
    if (images == nullptr)
    {
        *count = (uint32_t) list.size();
        return VK_SUCCESS;
    }
    uint32_t n = std::min<uint32_t>(*count, (uint32_t) list.size());
    for (uint32_t i = 0; i < n; i++)
        images[i] = list[i];
    *count = n;
    return n < list.size() ? VK_INCOMPLETE : VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice, uint32_t* count,
                                                                    VkQueueFamilyProperties* props)
{
    if (props == nullptr)
    {
        *count = 1;
        return;
    }
    *count = 1;
    props[0] = {};
    props[0].queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT;
    props[0].queueCount = 1;
}

VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue(VkDevice device, uint32_t family, uint32_t index, VkQueue* queue)
{
    *queue = QueueOf(device, family, index);
}

#define FAKE_CREATE(Fn, Info, Type, kind)                                                                         \
    VKAPI_ATTR VkResult VKAPI_CALL Fn(VkDevice, const Info*, const VkAllocationCallbacks*, Type* out)              \
    {                                                                                                              \
        if (Fails(#Fn))                                                                                            \
        {                                                                                                          \
            Ev(#Fn " -> VK_ERROR_OUT_OF_DEVICE_MEMORY");                                                           \
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;                                                                  \
        }                                                                                                          \
        *out = Mint<Type>();                                                                                       \
        Live(*out, kind);                                                                                          \
        Ev(std::string("create ") + kind);                                                                         \
        return VK_SUCCESS;                                                                                         \
    }

#define FAKE_DESTROY(Fn, Type, kind)                                                                              \
    VKAPI_ATTR void VKAPI_CALL Fn(VkDevice, Type handle, const VkAllocationCallbacks*)                            \
    {                                                                                                              \
        if (handle == VK_NULL_HANDLE)                                                                              \
            return;                                                                                                \
        Dead(handle, kind);                                                                                        \
        Ev(std::string("destroy ") + kind);                                                                        \
    }

FAKE_CREATE(vkCreateDescriptorPool, VkDescriptorPoolCreateInfo, VkDescriptorPool, "descriptor pool")
FAKE_CREATE(vkCreateRenderPass, VkRenderPassCreateInfo, VkRenderPass, "render pass")
FAKE_CREATE(vkCreateImageView, VkImageViewCreateInfo, VkImageView, "image view")
FAKE_CREATE(vkCreateFramebuffer, VkFramebufferCreateInfo, VkFramebuffer, "framebuffer")
FAKE_CREATE(vkCreateFence, VkFenceCreateInfo, VkFence, "fence")
FAKE_CREATE(vkCreateSemaphore, VkSemaphoreCreateInfo, VkSemaphore, "semaphore")
FAKE_DESTROY(vkDestroyDescriptorPool, VkDescriptorPool, "descriptor pool")
FAKE_DESTROY(vkDestroyRenderPass, VkRenderPass, "render pass")
FAKE_DESTROY(vkDestroyImageView, VkImageView, "image view")
FAKE_DESTROY(vkDestroyFramebuffer, VkFramebuffer, "framebuffer")
FAKE_DESTROY(vkDestroyFence, VkFence, "fence")
FAKE_DESTROY(vkDestroySemaphore, VkSemaphore, "semaphore")

VKAPI_ATTR VkResult VKAPI_CALL vkCreateCommandPool(VkDevice, const VkCommandPoolCreateInfo*,
                                                   const VkAllocationCallbacks*, VkCommandPool* pool)
{
    if (Fails("vkCreateCommandPool"))
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    *pool = Mint<VkCommandPool>();
    Live(*pool, "command pool");
    Ev("create command pool");
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyCommandPool(VkDevice, VkCommandPool pool, const VkAllocationCallbacks*)
{
    if (pool == VK_NULL_HANDLE)
        return;
    std::vector<uint64_t> buffers;
    {
        std::scoped_lock lock(g_evMutex);
        for (auto& [cb, owner] : g_commandBufferPool)
            if (owner == U(pool))
                buffers.push_back(cb);
        for (auto cb : buffers)
        {
            g_commandBufferPool.erase(cb);
            g_live.erase(cb);
        }
    }
    Dead(pool, "command pool");
    Ev("destroy command pool");
}

VKAPI_ATTR VkResult VKAPI_CALL vkAllocateCommandBuffers(VkDevice, const VkCommandBufferAllocateInfo* info,
                                                        VkCommandBuffer* buffers)
{
    if (Fails("vkAllocateCommandBuffers"))
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    for (uint32_t i = 0; i < info->commandBufferCount; i++)
    {
        buffers[i] = Mint<VkCommandBuffer>();
        Live(buffers[i], "command buffer");
        std::scoped_lock lock(g_evMutex);
        g_commandBufferPool[U(buffers[i])] = U(info->commandPool);
    }
    Ev("create command buffer");
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkFreeCommandBuffers(VkDevice, VkCommandPool, uint32_t count, const VkCommandBuffer* buffers)
{
    for (uint32_t i = 0; i < count; i++)
    {
        if (buffers[i] == VK_NULL_HANDLE)
            continue;
        Dead(buffers[i], "command buffer");
        std::scoped_lock lock(g_evMutex);
        g_commandBufferPool.erase(U(buffers[i]));
    }
    Ev("destroy command buffer");
}

VKAPI_ATTR VkResult VKAPI_CALL vkDeviceWaitIdle(VkDevice device)
{
    Ev("vkDeviceWaitIdle " + N(device));
    return g_idleResult;
}

VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(VkQueue queue, uint32_t, const VkSubmitInfo*, VkFence fence)
{
    Ev(std::string("vkQueueSubmit ") + N(queue) + (fence != VK_NULL_HANDLE ? " fenced" : ""));
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkWaitForFences(VkDevice, uint32_t, const VkFence*, VkBool32, uint64_t)
{
    Ev("vkWaitForFences");
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkResetFences(VkDevice, uint32_t, const VkFence*) { return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL vkResetCommandPool(VkDevice, VkCommandPool, VkCommandPoolResetFlags)
{
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkResetCommandBuffer(VkCommandBuffer, VkCommandBufferResetFlags) { return VK_SUCCESS; }
VKAPI_ATTR VkResult VKAPI_CALL vkBeginCommandBuffer(VkCommandBuffer, const VkCommandBufferBeginInfo*)
{
    return VK_SUCCESS;
}
VKAPI_ATTR VkResult VKAPI_CALL vkEndCommandBuffer(VkCommandBuffer) { return VK_SUCCESS; }
VKAPI_ATTR void VKAPI_CALL vkCmdBeginRenderPass(VkCommandBuffer, const VkRenderPassBeginInfo*, VkSubpassContents) {}
VKAPI_ATTR void VKAPI_CALL vkCmdEndRenderPass(VkCommandBuffer) {}
VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags,
                                                VkDependencyFlags, uint32_t, const VkMemoryBarrier*, uint32_t,
                                                const VkBufferMemoryBarrier*, uint32_t, const VkImageMemoryBarrier*)
{
}
VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance, const char*) { return nullptr; }

// ---- DeferredRelease ----------------------------------------------------------------------------

void DeferredRelease::FlushOwner(uint64_t owner) { Ev("DeferredRelease::FlushOwner " + N((void*) owner)); }
void DeferredRelease::RetireOwner(uint64_t owner, bool) { Ev("DeferredRelease::RetireOwner " + N((void*) owner)); }

MenuBlur_Vk::MenuBlur_Vk(std::string, VkDevice, VkPhysicalDevice) { Fault("menu blur built: no case opens the menu"); }

// ---- ImGui core and the Vulkan renderer backend -------------------------------------------------

static std::map<void*, size_t> g_imAllocations;

ImGuiIO& ImGui::GetIO()
{
    // ImGuiIO's constructor lives in imgui.cpp; the overlay only touches plain fields.
    static ImGuiIO* io = static_cast<ImGuiIO*>(std::calloc(1, sizeof(ImGuiIO)));
    return *io;
}

void* ImGui::MemAlloc(size_t size)
{
    void* p = std::malloc(size);
    std::scoped_lock lock(g_evMutex);
    g_imAllocations[p] = size;
    return p;
}

void ImGui::MemFree(void* p)
{
    if (p == nullptr)
        return;
    {
        std::scoped_lock lock(g_evMutex);
        if (g_imAllocations.erase(p) != 1)
            g_faults.push_back("IM_FREE of a pointer IM_ALLOC never returned");
    }
    std::free(p);
}

static size_t ImLive()
{
    std::scoped_lock lock(g_evMutex);
    return g_imAllocations.size();
}

void ImGui::Render() { Ev("ImGui::Render"); }
ImDrawData* ImGui::GetDrawData() { return nullptr; }

static int g_backendToken;

bool ImGui_ImplVulkan_Init(ImGui_ImplVulkan_InitInfo* info)
{
    if (ImGui::GetIO().BackendRendererUserData != nullptr)
        Fault("ImGui_ImplVulkan_Init over a live renderer backend");
    ImGui::GetIO().BackendRendererUserData = &g_backendToken;
    Ev("ImGui_ImplVulkan_Init " + N(info->Device));
    return true;
}

void ImGui_ImplVulkan_Shutdown(bool)
{
    // The real one reads bd->VulkanInitInfo through ImGui_ImplVulkan_DestroyDeviceObjects; with no
    // backend bd is null and only a Debug build's IM_ASSERT stops the access violation.
    if (ImGui::GetIO().BackendRendererUserData == nullptr)
        Fault("ImGui_ImplVulkan_Shutdown with no renderer backend (null bd in DestroyDeviceObjects)");
    ImGui::GetIO().BackendRendererUserData = nullptr;
    Ev("ImGui_ImplVulkan_Shutdown");
}

void ImGui_ImplVulkan_NewFrame() {}
void ImGui_ImplVulkan_RenderDrawData(ImDrawData*, VkCommandBuffer, VkPipeline) { Ev("ImGui_ImplVulkan_RenderDrawData"); }
void ImGui_ImplVulkan_SetQueue(VkQueue) {}

// ---- MenuOverlayBase (MenuCommon's ImGui context and window) ------------------------------------

// MenuCommon's own storage (menu_common.h): a plain HWND and bool, read and written through
// std::atomic_ref. run.py puts the production getters' expressions in place of the @...@ markers and
// checks that MenuCommon::Init and Shutdown store through std::atomic_ref, as the fake below does.
static HWND g_menuHandle = nullptr;
static bool g_menuInited = false;
static int g_menuInits = 0;
static int g_menuShutdowns = 0;

// Runs once, on the thread that set it, right after that thread's next IsInited() read is taken:
// the deterministic interleaving point between OnOtherWindow's read and the locks.
thread_local std::function<void()> t_afterIsInitedRead;

HWND MenuOverlayBase::Handle() { return @HANDLE_READ@; }

bool MenuOverlayBase::IsInited()
{
    bool value = @INITED_READ@;
    if (t_afterIsInitedRead)
    {
        auto hook = std::move(t_afterIsInitedRead);
        t_afterIsInitedRead = nullptr;
        hook();
    }
    return value;
}

void MenuOverlayBase::Init(HWND hwnd, bool, bool)
{
    if (@INITED_READ@)
        Fault("MenuOverlayBase::Init over a live context (ImGui::CreateContext leaks the old one)");
    Ev("MenuOverlayBase::Init " + N(hwnd));
    g_menuInits++;
    std::atomic_ref<HWND>(g_menuHandle).store(hwnd);
    std::atomic_ref<bool>(g_menuInited).store(true);
}

void MenuOverlayBase::Shutdown()
{
    if (!@INITED_READ@)
        return;
    // MenuCommon::Shutdown destroys the ImGui context; a renderer backend still attached leaks with it.
    if (ImGui::GetIO().BackendRendererUserData != nullptr)
        Fault("MenuOverlayBase::Shutdown with the Vulkan renderer backend still attached");
    Ev("MenuOverlayBase::Shutdown");
    g_menuShutdowns++;
    std::atomic_ref<HWND>(g_menuHandle).store(nullptr);
    std::atomic_ref<bool>(g_menuInited).store(false);
}

void MenuOverlayBase::VulkanReady() { Ev("MenuOverlayBase::VulkanReady"); }
bool MenuOverlayBase::RenderMenu() { return true; }
bool MenuOverlayBase::IsVisible() { return false; }

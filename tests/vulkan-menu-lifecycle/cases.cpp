// Cases for the Vulkan menu overlay's swapchain lifecycle. Appended after the production
// menu_overlay_vk.cpp, fakes.cpp and the sliced surface-hook tail, so the overlay's statics
// (_menuSwapchain, _vulkanObjectsCreated, _ImVulkan_Info) are visible here.

#include <condition_variable>
#include <iostream>

static int g_failures = 0;
static const char* g_case = "";

static void Check(bool ok, const std::string& what)
{
    if (!ok)
    {
        std::cout << "FAIL [" << g_case << "] " << what << "\n";
        g_failures++;
    }
}

static void PrintEvents(size_t mark)
{
    for (auto& e : Since(mark))
        std::cout << "    | " << e << "\n";
}

static int Count(const std::vector<std::string>& events, const std::string& prefix)
{
    int n = 0;
    for (auto& e : events)
        n += e.rfind(prefix, 0) == 0;
    return n;
}

static bool Contains(const std::vector<std::string>& events, const std::string& prefix)
{
    return Count(events, prefix) > 0;
}

// Anything that idles the device or releases an object the menu draws with.
static int Disturbances(const std::vector<std::string>& events)
{
    return Count(events, "vkDeviceWaitIdle") + Count(events, "destroy ") + Count(events, "ImGui_ImplVulkan_Shutdown") +
           Count(events, "MenuOverlayBase::Shutdown") + Count(events, "MenuOverlayBase::Init") +
           Count(events, "DeferredRelease::");
}

static const VkPhysicalDevice kPd = reinterpret_cast<VkPhysicalDevice>((uintptr_t) 0x10);
static const VkInstance kInstance = reinterpret_cast<VkInstance>((uintptr_t) 0x20);

static VkDevice MakeDevice(const std::string& name)
{
    auto device = Mint<VkDevice>(name);
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue {};
    queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue.queueFamilyIndex = 0;
    queue.queueCount = 1;
    queue.pQueuePriorities = &priority;
    VkDeviceCreateInfo info {};
    info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    info.queueCreateInfoCount = 1;
    info.pQueueCreateInfos = &queue;
    MenuOverlayVk::DeviceCreated(device, &info);
    return device;
}

// What hkvkCreateSwapchainKHR does around the overlay: the driver create, then -- only on success
// (run.py checks that guard in the production hook) -- MenuOverlayVk::CreateSwapchain with the
// surface's window.
static VkResult GameCreateSwapchain(VkDevice device, HWND hwnd, const std::string& name, uint32_t images,
                                    VkSwapchainKHR old, VkSwapchainKHR* out, VkResult driverResult = VK_SUCCESS)
{
    VkSwapchainCreateInfoKHR info {};
    info.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
    info.minImageCount = 2;
    info.imageFormat = VK_FORMAT_B8G8R8A8_UNORM;
    info.imageExtent = { 1280, 720 };
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    info.oldSwapchain = old;

    if (driverResult != VK_SUCCESS)
    {
        Ev("driver vkCreateSwapchainKHR " + name + " failed");
        *out = VK_NULL_HANDLE;
        return driverResult;
    }

    *out = Mint<VkSwapchainKHR>(name);
    {
        std::vector<VkImage> list;
        for (uint32_t i = 0; i < images; i++)
            list.push_back(Mint<VkImage>());
        std::scoped_lock lock(g_evMutex);
        g_swapchainImages[*out] = list;
    }
    Ev("driver vkCreateSwapchainKHR " + name + " on " + N(hwnd));
    MenuOverlayVk::CreateSwapchain(device, kPd, kInstance, hwnd, &info, nullptr, out);
    return VK_SUCCESS;
}

struct PresentOutcome
{
    bool result = false;
    bool untouched = false; // the present info the hook passes on is byte-identical to the game's
    int submits = 0;        // overlay submissions this present made
};

static PresentOutcome Present(VkQueue queue, VkSwapchainKHR swapchain, uint32_t image = 0)
{
    auto wait = reinterpret_cast<VkSemaphore>((uintptr_t) 0x30);
    VkPresentInfoKHR info {};
    info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores = &wait;
    info.swapchainCount = 1;
    info.pSwapchains = &swapchain;
    info.pImageIndices = &image;
    VkPresentInfoKHR before = info;
    auto mark = Mark();
    PresentOutcome outcome;
    outcome.result = MenuOverlayVk::QueuePresent(queue, &info);
    outcome.untouched = std::memcmp(&before, &info, sizeof(info)) == 0;
    outcome.submits = Count(Since(mark), "vkQueueSubmit");
    return outcome;
}

// One overlay generation for an N-image swapchain: descriptor pool, render pass, and per image a
// view, framebuffer, command pool, command buffer, fence and semaphore.
static size_t Generation(uint32_t images) { return 2 + 6 * (size_t) images; }

static void Begin(const char* name)
{
    g_case = name;
    std::scoped_lock lock(g_evMutex);
    g_events.clear();
    g_faults.clear();
    g_failAt.clear();
    g_calls.clear();
    g_live.clear();
    g_commandBufferPool.clear();
    g_imAllocations.clear();
    g_idleResult = VK_SUCCESS;
    g_menuInits = 0;
    g_menuShutdowns = 0;
}

// Production teardown, then the fake menu context. Leaves nothing alive for the next case.
static void End(int faultsExpected = 0)
{
    MenuOverlayVk::DestroyVulkanObjects(false);
    Check(LiveCount() == 0, "every overlay object is released at teardown (" + std::to_string(LiveCount()) + " left)");
    Check(ImLive() == 0, "every IM_ALLOC is freed at teardown");
    {
        std::scoped_lock lock(g_evMutex);
        if ((int) g_faults.size() != faultsExpected)
        {
            for (auto& f : g_faults)
                std::cout << "FAIL [" << g_case << "] fault: " << f << "\n";
            if (g_faults.empty())
                std::cout << "FAIL [" << g_case << "] expected " << faultsExpected << " fault(s)\n";
            g_failures++;
        }
    }
    MenuOverlayBase::Shutdown();
    g_menuHandle = nullptr;
    std::cout << "done [" << g_case << "]\n";
}

// ---- cases --------------------------------------------------------------------------------------

// A second window's swapchain is filtered before the locks: it must not idle the device, release
// anything the menu draws with, or move the ImGui context. The menu keeps drawing on its own chain
// and the other chain's presents pass through untouched.
static void SecondWindowLeavesMenuAlone()
{
    Begin("second window never touches the menu");
    auto device = MakeDevice("dev");
    auto queue = QueueOf(device, 0, 0);
    auto windowA = MakeWindow("windowA");
    auto windowB = MakeWindow("windowB");

    VkSwapchainKHR a = VK_NULL_HANDLE, b = VK_NULL_HANDLE;
    GameCreateSwapchain(device, windowA, "scA", 3, VK_NULL_HANDLE, &a);
    Check(_menuSwapchain == a && _vulkanObjectsCreated, "the first chain owns the menu");
    Check(LiveCount() == Generation(3), "one generation of objects for the menu's chain");

    auto mark = Mark();
    _hwnd = nullptr;
    FakeSurfaceInfo surface { windowB };
    SurfaceHookTail(&surface);
    GameCreateSwapchain(device, windowB, "scB", 2, VK_NULL_HANDLE, &b);
    auto events = Since(mark);
    Check(Disturbances(events) == 0, "window B's surface and swapchain cause no idle, release or menu re-init");
    if (Disturbances(events) != 0)
        PrintEvents(mark);
    Check(_hwnd == nullptr, "the surface hook keeps the fallback window on the menu's");
    Check(_menuSwapchain == a && MenuOverlayBase::Handle() == windowA, "the menu stays on window A's chain");
    Check(LiveCount() == Generation(3), "window B allocated no overlay objects");

    auto drawn = Present(queue, a);
    Check(drawn.result && drawn.submits == 1 && !drawn.untouched, "the menu's chain is still drawn on");
    auto passed = Present(queue, b);
    Check(passed.result && passed.submits == 0 && passed.untouched, "window B's present passes through untouched");
    End();
}

// Recreating on the same window with oldSwapchain hands the menu over: one drain, the old
// generation released before the new one is built, the ImGui context kept, and the retired chain's
// presents passed through.
static void SameWindowRecreateHandsOver()
{
    Begin("two chains on one window hand over");
    auto device = MakeDevice("dev");
    auto queue = QueueOf(device, 0, 0);
    auto window = MakeWindow("window");

    VkSwapchainKHR first = VK_NULL_HANDLE, second = VK_NULL_HANDLE;
    GameCreateSwapchain(device, window, "sc1", 3, VK_NULL_HANDLE, &first);
    Check(Present(queue, first).submits == 1, "the first chain is drawn on");

    auto mark = Mark();
    GameCreateSwapchain(device, window, "sc2", 4, first, &second);
    auto events = Since(mark);

    size_t idle = events.size(), firstDestroy = events.size(), firstCreate = events.size();
    for (size_t i = 0; i < events.size(); i++)
    {
        if (idle == events.size() && events[i].rfind("vkDeviceWaitIdle", 0) == 0)
            idle = i;
        if (firstDestroy == events.size() && events[i].rfind("destroy ", 0) == 0)
            firstDestroy = i;
        if (firstCreate == events.size() && events[i].rfind("create ", 0) == 0)
            firstCreate = i;
    }
    Check(idle < firstDestroy && firstDestroy < firstCreate,
          "drain, then release the old generation, then build the new one");
    Check(Count(events, "destroy ") == (int) Generation(3), "all of the old generation is released");
    Check(Count(events, "MenuOverlayBase::Shutdown") == 0 && Count(events, "MenuOverlayBase::Init") == 0,
          "the ImGui context stays: same window");
    Check(Count(events, "ImGui_ImplVulkan_Shutdown") == 1 && Count(events, "ImGui_ImplVulkan_Init") == 1,
          "the renderer backend is rebuilt once for the new render pass");
    Check(_menuSwapchain == second && LiveCount() == Generation(4), "the new chain owns one generation");

    auto retired = Present(queue, first);
    Check(retired.result && retired.untouched && retired.submits == 0, "the retired chain passes through");
    Check(Present(queue, second, 3).submits == 1, "the new chain is drawn on, every image index");
    End();
}

// The race in N-02: two first creates on two windows, from two threads, both read OnOtherWindow
// before either has the locks. Deterministic: thread B is held right after its IsInited() read until
// thread A has finished its whole create. One owner must come out, and the loser must not idle or
// tear down the winner's objects.
static void ConcurrentFirstAttach()
{
    Begin("concurrent first attach from two threads");
    auto device = MakeDevice("dev");
    auto queue = QueueOf(device, 0, 0);
    auto windowA = MakeWindow("windowA");
    auto windowB = MakeWindow("windowB");

    std::mutex m;
    std::condition_variable cv;
    bool aMayRun = false, aDone = false;
    VkSwapchainKHR a = VK_NULL_HANDLE, b = VK_NULL_HANDLE;
    size_t afterA = 0;

    std::thread threadA(
        [&]
        {
            std::unique_lock lock(m);
            cv.wait(lock, [&] { return aMayRun; });
            lock.unlock();
            GameCreateSwapchain(device, windowA, "scA", 3, VK_NULL_HANDLE, &a);
            afterA = Mark();
            lock.lock();
            aDone = true;
            cv.notify_all();
        });
    std::thread threadB(
        [&]
        {
            t_afterIsInitedRead = [&]
            {
                std::unique_lock lock(m);
                aMayRun = true;
                cv.notify_all();
                cv.wait(lock, [&] { return aDone; });
            };
            GameCreateSwapchain(device, windowB, "scB", 2, VK_NULL_HANDLE, &b);
        });
    threadA.join();
    threadB.join();

    auto loser = Since(afterA);
    Check(Disturbances(loser) == 0, "thread B, arriving second under the locks, leaves A's menu alone");
    if (Disturbances(loser) != 0)
        PrintEvents(afterA);
    Check(g_menuInits == 1 && g_menuShutdowns == 0, "exactly one ImGui context was made, none destroyed");
    Check(_menuSwapchain == a && MenuOverlayBase::Handle() == windowA, "exactly one owner: window A's chain");
    Check(LiveCount() == Generation(3), "one generation alive, nothing leaked by the loser");
    Check(Present(queue, a).submits == 1 && Present(queue, b).untouched, "A is drawn on, B passes through");
    End();
}

// The same two threads with no forced interleaving, many times. Whatever the order, one window owns
// the menu, one context is made, and nothing leaks or faults.
static void ConcurrentFirstAttachFree()
{
    Begin("free-running first attach, 200 rounds");
    auto device = MakeDevice("dev");
    int owners = 0;
    for (int round = 0; round < 200; round++)
    {
        auto windowA = MakeWindow("A" + std::to_string(round));
        auto windowB = MakeWindow("B" + std::to_string(round));
        int initsBefore = g_menuInits;
        VkSwapchainKHR a = VK_NULL_HANDLE, b = VK_NULL_HANDLE;
        std::thread ta([&] { GameCreateSwapchain(device, windowA, "a", 2, VK_NULL_HANDLE, &a); });
        std::thread tb([&] { GameCreateSwapchain(device, windowB, "b", 2, VK_NULL_HANDLE, &b); });
        ta.join();
        tb.join();
        bool ownerA = _menuSwapchain == a && MenuOverlayBase::Handle() == windowA;
        bool ownerB = _menuSwapchain == b && MenuOverlayBase::Handle() == windowB;
        owners += ownerA != ownerB;
        if (g_menuInits != initsBefore + 1 || LiveCount() != Generation(2))
        {
            Check(false, "round " + std::to_string(round) + ": one context and one generation per pair");
            break;
        }
        // The next pair replaces this one: the old windows go away, as a game's would.
        DestroyWindowFake(windowA);
        DestroyWindowFake(windowB);
    }
    Check(owners == 200, "every round ended with exactly one owner (" + std::to_string(owners) + "/200)");
    End();
}

// A swapchain create that fails leaves the menu consistent. In the driver: the hook never reaches
// the overlay, so the old chain keeps its menu (a retired chain can still present acquired images).
// In the overlay's own build: the partial generation is released and the menu is off until the next
// create, which recovers.
static void FailingCreateLeavesStateConsistent()
{
    Begin("failing vkCreateSwapchainKHR leaves the menu consistent");
    auto device = MakeDevice("dev");
    auto queue = QueueOf(device, 0, 0);
    auto window = MakeWindow("window");

    VkSwapchainKHR first = VK_NULL_HANDLE, failed = VK_NULL_HANDLE, second = VK_NULL_HANDLE,
                   third = VK_NULL_HANDLE;
    GameCreateSwapchain(device, window, "sc1", 3, VK_NULL_HANDLE, &first);

    auto mark = Mark();
    GameCreateSwapchain(device, window, "scFail", 3, first, &failed, VK_ERROR_OUT_OF_DEVICE_MEMORY);
    Check(Disturbances(Since(mark)) == 0, "a driver failure never reaches the overlay");
    Check(_menuSwapchain == first && _vulkanObjectsCreated && LiveCount() == Generation(3),
          "the menu keeps the old chain and its generation");
    Check(Present(queue, first).submits == 1, "and keeps drawing on it");

    // The overlay's second framebuffer fails: CreateVulkanObjects' cleanup releases what it built.
    {
        std::scoped_lock lock(g_evMutex);
        g_failAt["vkCreateFramebuffer"] = 2;
        g_calls["vkCreateFramebuffer"] = 0;
    }
    GameCreateSwapchain(device, window, "sc2", 3, first, &second);
    Check(!_vulkanObjectsCreated && _ImVulkan_Info.Device == VK_NULL_HANDLE, "no half-built generation is published");
    Check(LiveCount() == 0 && ImLive() == 0, "the partial generation and its arrays are released");
    Check(ImGui::GetIO().BackendRendererUserData == nullptr, "no renderer backend is left without objects");
    auto off = Present(queue, second);
    Check(off.result && off.untouched && off.submits == 0, "with no generation, presents pass through");

    GameCreateSwapchain(device, window, "sc3", 3, second, &third);
    Check(_menuSwapchain == third && _vulkanObjectsCreated && LiveCount() == Generation(3), "the next create recovers");
    Check(Present(queue, third).submits == 1, "and draws");
    End();
}

// The drain before a recreate fails (not device loss): the GPU may still use the old generation, so
// it is kept whole -- nothing released, nothing replaced -- and the menu stops drawing. The next
// create whose drain succeeds releases it and recovers.
static void FailedDrainKeepsOwnership()
{
    Begin("failed drain keeps the old generation");
    auto device = MakeDevice("dev");
    auto queue = QueueOf(device, 0, 0);
    auto window = MakeWindow("window");
    VkSwapchainKHR first = VK_NULL_HANDLE, second = VK_NULL_HANDLE, third = VK_NULL_HANDLE;
    GameCreateSwapchain(device, window, "sc1", 3, VK_NULL_HANDLE, &first);

    g_idleResult = VK_ERROR_OUT_OF_HOST_MEMORY;
    auto mark = Mark();
    GameCreateSwapchain(device, window, "sc2", 3, first, &second);
    auto events = Since(mark);
    g_idleResult = VK_SUCCESS;
    Check(Count(events, "vkDeviceWaitIdle") == 1 && Count(events, "destroy ") == 0 && Count(events, "create ") == 0,
          "one failed drain, then nothing released and nothing built");
    Check(LiveCount() == Generation(3) && _ImVulkan_Info.Device == device, "the old generation is kept whole");
    Check(!_vulkanObjectsCreated && Present(queue, first).untouched, "and no longer drawn with");

    GameCreateSwapchain(device, window, "sc3", 2, second, &third);
    Check(_menuSwapchain == third && _vulkanObjectsCreated && LiveCount() == Generation(2),
          "the next create releases it and recovers");
    End();
}

// The menu's window is destroyed and the game goes on with a window of its own (a launcher or splash
// replaced by the main window): the next create re-binds the menu to the new window, with one
// context handed over and no renderer backend shut down twice. With and without the surface hook
// having already released the menu's objects first.
static void DestroyedWindowRebinds(bool surfaceHookFirst)
{
    Begin(surfaceHookFirst ? "destroyed menu window, surface hook then create re-binds"
                           : "destroyed menu window, create re-binds");
    auto device = MakeDevice("dev");
    auto queue = QueueOf(device, 0, 0);
    auto windowA = MakeWindow("windowA");
    auto windowB = MakeWindow("windowB");

    VkSwapchainKHR a = VK_NULL_HANDLE, b = VK_NULL_HANDLE;
    GameCreateSwapchain(device, windowA, "scA", 3, VK_NULL_HANDLE, &a);
    DestroyWindowFake(windowA);

    auto mark = Mark();
    if (surfaceHookFirst)
    {
        FakeSurfaceInfo surface { windowB };
        SurfaceHookTail(&surface);
        Check(_hwnd == windowB, "the surface hook captures the new window");
        Check(!_vulkanObjectsCreated && LiveCount() == 0, "the surface hook released the dead window's objects");
    }
    GameCreateSwapchain(device, windowB, "scB", 2, VK_NULL_HANDLE, &b);
    auto events = Since(mark);
    Check(Count(events, "ImGui_ImplVulkan_Shutdown") == 1, "the renderer backend is shut down exactly once");
    Check(Count(events, "MenuOverlayBase::Shutdown") == 1 && Count(events, "MenuOverlayBase::Init windowB") == 1,
          "the ImGui context moves to window B once");
    Check(_menuSwapchain == b && MenuOverlayBase::Handle() == windowB && _vulkanObjectsCreated, "B owns the menu");
    Check(LiveCount() == Generation(2), "one generation, for B's chain");
    Check(Present(queue, b).submits == 1, "B is drawn on");
    Check(Present(queue, a).untouched, "A's chain, if it still presents, passes through");
    End();
}

// EXPECTATION, policy undecided: the menu was made on a splash window that the game hides but does
// not destroy, then the main window gets its own chain. OnOtherWindow only asks IsWindow(), and a
// hidden window still is one, so the menu stays on the splash and the main window's presents carry
// no menu. Whether visibility, size or recency should move it is not decided; this records what the
// code does so a change to it is a decision, not an accident.
static void HiddenSplashKeepsMenu()
{
    Begin("EXPECTATION: hidden splash keeps the menu");
    auto device = MakeDevice("dev");
    auto queue = QueueOf(device, 0, 0);
    auto splash = MakeWindow("splash");
    auto main = MakeWindow("main");

    VkSwapchainKHR s = VK_NULL_HANDLE, m = VK_NULL_HANDLE;
    GameCreateSwapchain(device, splash, "scSplash", 2, VK_NULL_HANDLE, &s);
    auto mark = Mark();
    GameCreateSwapchain(device, main, "scMain", 3, VK_NULL_HANDLE, &m);
    Check(Disturbances(Since(mark)) == 0, "the main window's chain is filtered like any other window's");
    Check(_menuSwapchain == s && MenuOverlayBase::Handle() == splash, "the menu stays on the (hidden) splash");
    Check(Present(queue, m).untouched, "the main window presents without the menu");
    std::cout << "EXPECTATION (undecided policy): a hidden-but-valid splash keeps the menu; the main window "
                 "has none until the splash window is destroyed\n";
    End();
}

// EXPECTATION, not fixed here: the surface hook in Vulkan_Hooks.cpp has the same read-then-act shape
// (OnOtherWindow, then DestroyVulkanObjects) without a re-check under the overlay's locks. A surface
// for window B whose check ran before window A's first create finished tears A's objects down; A's
// menu then stays dark until A's chain is recreated, and B's chain is filtered. No leak, no fault:
// the state is consistent, the menu is just off. Recorded, with the recovery asserted.
static void SurfaceHookRaceRecovers()
{
    Begin("EXPECTATION: surface hook race darkens the menu until A recreates");
    auto device = MakeDevice("dev");
    auto queue = QueueOf(device, 0, 0);
    auto windowA = MakeWindow("windowA");
    auto windowB = MakeWindow("windowB");

    std::mutex mtx;
    std::condition_variable cv;
    bool aMayRun = false, aDone = false;
    VkSwapchainKHR a = VK_NULL_HANDLE, a2 = VK_NULL_HANDLE, b = VK_NULL_HANDLE;
    std::thread threadA(
        [&]
        {
            std::unique_lock lock(mtx);
            cv.wait(lock, [&] { return aMayRun; });
            lock.unlock();
            GameCreateSwapchain(device, windowA, "scA", 2, VK_NULL_HANDLE, &a);
            lock.lock();
            aDone = true;
            cv.notify_all();
        });
    std::thread threadB(
        [&]
        {
            t_afterIsInitedRead = [&]
            {
                std::unique_lock lock(mtx);
                aMayRun = true;
                cv.notify_all();
                cv.wait(lock, [&] { return aDone; });
            };
            FakeSurfaceInfo surface { windowB };
            SurfaceHookTail(&surface);
        });
    threadA.join();
    threadB.join();

    bool dark = !_vulkanObjectsCreated;
    Check(LiveCount() == (dark ? 0 : Generation(2)), "objects are either all alive or all released");
    GameCreateSwapchain(device, windowB, "scB", 2, VK_NULL_HANDLE, &b);
    Check(MenuOverlayBase::Handle() == windowA, "B's chain is still filtered: the menu belongs to A");
    GameCreateSwapchain(device, windowA, "scA2", 2, a, &a2);
    Check(_menuSwapchain == a2 && _vulkanObjectsCreated && Present(queue, a2).submits == 1,
          "A's next chain brings the menu back");
    std::cout << "EXPECTATION (residual, not fixed): surface hook race " << (dark ? "darkened" : "did not darken")
              << " the menu until window A's chain was recreated\n";
    End();
}

// A present whose first swapchain is not the menu's passes through with the info untouched -- also
// when the menu's chain is second in the same present.
static void NonMenuPresentPassesThrough()
{
    Begin("present of a non-menu swapchain passes through");
    auto device = MakeDevice("dev");
    auto queue = QueueOf(device, 0, 0);
    auto windowA = MakeWindow("windowA");
    auto windowB = MakeWindow("windowB");
    VkSwapchainKHR a = VK_NULL_HANDLE, b = VK_NULL_HANDLE;
    GameCreateSwapchain(device, windowA, "scA", 2, VK_NULL_HANDLE, &a);
    GameCreateSwapchain(device, windowB, "scB", 2, VK_NULL_HANDLE, &b);

    VkSwapchainKHR both[2] = { b, a };
    uint32_t indices[2] = { 0, 0 };
    auto wait = reinterpret_cast<VkSemaphore>((uintptr_t) 0x30);
    VkPresentInfoKHR info {};
    info.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    info.waitSemaphoreCount = 1;
    info.pWaitSemaphores = &wait;
    info.swapchainCount = 2;
    info.pSwapchains = both;
    info.pImageIndices = indices;
    VkPresentInfoKHR before = info;
    auto mark = Mark();
    bool result = MenuOverlayVk::QueuePresent(queue, &info);
    auto events = Since(mark);
    Check(result && std::memcmp(&before, &info, sizeof(info)) == 0, "the present info is passed on unchanged");
    Check(events.empty(), "no Vulkan or ImGui call is made for it");
    End();
}

int main()
{
    SecondWindowLeavesMenuAlone();
    SameWindowRecreateHandsOver();
    ConcurrentFirstAttach();
    ConcurrentFirstAttachFree();
    FailingCreateLeavesStateConsistent();
    FailedDrainKeepsOwnership();
    DestroyedWindowRebinds(false);
    DestroyedWindowRebinds(true);
    HiddenSplashKeepsMenu();
    SurfaceHookRaceRecovers();
    NonMenuPresentPassesThrough();

    if (g_failures != 0)
    {
        std::cout << "FAILED: " << g_failures << " check(s)\n";
        return 1;
    }
    std::cout << "PASS: Vulkan menu swapchain lifecycle contract (host fakes, production menu_overlay_vk.cpp)\n";
    return 0;
}

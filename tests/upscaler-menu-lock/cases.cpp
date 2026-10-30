
// ---- cases ----

static int failures = 0;
static void check(bool passed, const std::string& message)
{
    if (!passed)
    {
        ++failures;
        std::fprintf(stderr, "FAIL: %s\n", message.c_str());
    }
}

struct Gate
{
    std::mutex m;
    std::condition_variable cv;
    bool open = false;
    void Open()
    {
        {
            std::lock_guard lock(m);
            open = true;
        }
        cv.notify_all();
    }
    bool Wait(std::chrono::milliseconds limit)
    {
        std::unique_lock lock(m);
        return cv.wait_for(lock, limit, [&] { return open; });
    }
};

static ID3D12Device device;
static ID3D12GraphicsCommandList commandList;
static ID3D12Resource output;
static NVSDK_NGX_Parameter parameters;

static IFeature_Dx12* NewFeature()
{
    auto* feature = new IFeature_Dx12();
    check(feature->Init(&device, &commandList, &parameters), "Init succeeds");
    return feature;
}

static void Clear()
{
    duringRender = [] {};
    duringDestroy = [] {};
    delete NewFeature(); // the destructor drops the shared menu
}

// RE2: a replaced feature is destroyed by Util::DelayedDestroy on its own thread two seconds later, while the running
// feature draws the shared menu on the render thread. The destroyed feature's reset must wait for the draw.
static void DestroyWhileDrawing()
{
    Clear();
    auto* running = NewFeature();
    auto* replaced = NewFeature();
    check(menusAlive == 1, "every feature shares one menu");

    Gate drawing, destroying;
    duringRender = [&]
    {
        drawing.Open();
        destroying.Wait(std::chrono::seconds(2));
        // The destroyer is now in the destructor (or waiting for this draw)
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    };
    std::thread render([&] { running->RenderLegacyMenu(&commandList, &output); });
    std::thread destroy(
        [&]
        {
            drawing.Wait(std::chrono::seconds(2));
            destroying.Open();
            delete replaced;
        });
    render.join();
    destroy.join();
    duringRender = [] {};
    check(menusAlive == 0, "the reset took effect after the draw");

    // The running feature builds it again on its next draw, as before
    running->RenderLegacyMenu(&commandList, &output);
    check(running->HasMenu() && menusAlive == 1, "the next draw recreates the menu");
    delete running;
}

// RE2: XeSS dropped together with the DLSS feature that failed to initialise; both destructors reset the menu at once.
static void TwoDestroyersAtOnce()
{
    for (int round = 0; round < 200; ++round)
    {
        Clear();
        auto* first = NewFeature();
        auto* second = NewFeature();
        std::atomic<int> ready { 0 };
        duringDestroy = [] { std::this_thread::sleep_for(std::chrono::microseconds(200)); };
        auto drop = [&](IFeature_Dx12* feature)
        {
            ++ready;
            while (ready.load() < 2)
            {
            }
            delete feature;
        };
        std::thread a(drop, first), b(drop, second);
        a.join();
        b.join();
        duringDestroy = [] {};
        if (menusAlive != 0)
        {
            check(false, "two destructors together leave no menu behind, round " + std::to_string(round));
            return;
        }
    }
}

// A feature initialising (creating the shared menu) while another is destroyed: the menu is either there and alive
// or gone, never leaked or freed twice.
static void CreateWhileDestroying()
{
    for (int round = 0; round < 200; ++round)
    {
        Clear();
        auto* doomed = NewFeature();
        // Init only creates the menu when there is none
        delete doomed;
        doomed = new IFeature_Dx12();
        IFeature_Dx12* created = nullptr;
        std::atomic<int> ready { 0 };
        std::thread a(
            [&]
            {
                ++ready;
                while (ready.load() < 2)
                {
                }
                created = NewFeature();
            });
        std::thread b(
            [&]
            {
                ++ready;
                while (ready.load() < 2)
                {
                }
                delete doomed;
            });
        a.join();
        b.join();
        if (menusAlive != (created->HasMenu() ? 1 : 0))
        {
            check(false, "the menu count matches the shared pointer, round " + std::to_string(round));
            return;
        }
        delete created;
    }
}

int main()
{
    DestroyWhileDrawing();
    TwoDestroyersAtOnce();
    CreateWhileDestroying();
    Clear();
    check(menusAlive == 0, "nothing leaked");
    if (failures == 0)
        std::printf("upscaler-menu-lock: all cases passed (%d menus created)\n", menusCreated.load());
    return failures == 0 ? 0 : 1;
}

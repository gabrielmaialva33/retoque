// The vkCreateDevice contract, case by case. Everything above this file is fakes.h and the production slices
// run.py cut out of Vulkan_Hooks.cpp, Vulkan_Spoofing.cpp and State.h; hkvkCreateDevice below is OptiScaler's.

namespace
{

const VkInstance kInstance = reinterpret_cast<VkInstance>(uintptr_t(0x1000));

// ---- The game's side ----------------------------------------------------------------------------------------

// A game's create info and everything it points at, owned here and never moved once built.
struct Request
{
    std::vector<std::unique_ptr<std::string>> names;
    std::vector<const char*> extensions;
    std::vector<std::shared_ptr<void>> structs;
    std::vector<VkBaseOutStructure*> order;
    std::vector<size_t> sizes; // of each struct in order
    VkPhysicalDeviceFeatures core {};
    bool useCore = false;
    float priority = 1.0f;
    VkDeviceQueueCreateInfo queue {};
    VkDeviceCreateInfo info {};

    Request() = default;
    Request(const Request&) = delete;
    Request& operator=(const Request&) = delete;

    Request& Ext(const char* name)
    {
        names.push_back(std::make_unique<std::string>(name));
        return *this;
    }

    template <typename T> T& Add(VkStructureType sType)
    {
        auto feature = std::make_shared<T>();
        feature->sType = sType;
        structs.push_back(feature);
        order.push_back(reinterpret_cast<VkBaseOutStructure*>(feature.get()));
        sizes.push_back(sizeof(T));
        return *feature;
    }

    // A structure the spoofing's feature copier does not know, ahead of the ones it does
    void Unknown() { Add<VkDevicePrivateDataCreateInfo>(VK_STRUCTURE_TYPE_DEVICE_PRIVATE_DATA_CREATE_INFO); }

    VkDeviceCreateInfo* Build()
    {
        for (const auto& name : names)
            extensions.push_back(name->c_str());
        for (size_t i = 0; i < order.size(); i++)
            order[i]->pNext = i + 1 < order.size() ? order[i + 1] : nullptr;

        queue.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
        queue.queueFamilyIndex = 0;
        queue.queueCount = 1;
        queue.pQueuePriorities = &priority;

        info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
        info.pNext = order.empty() ? nullptr : order.front();
        info.queueCreateInfoCount = 1;
        info.pQueueCreateInfos = &queue;
        info.enabledExtensionCount = (uint32_t) extensions.size();
        info.ppEnabledExtensionNames = extensions.empty() ? nullptr : extensions.data();
        info.pEnabledFeatures = useCore ? &core : nullptr;
        return &info;
    }

    // The built request copied into pages of its own that are then made read-only, as a game that keeps its create
    // info in constant data has it: any write to what the game handed over faults. Current() is then that copy.
    void Freeze()
    {
        size_t size = sizeof(VkDeviceCreateInfo) + sizeof(VkDeviceQueueCreateInfo) + sizeof(float) +
                      sizeof(VkPhysicalDeviceFeatures) + extensions.size() * sizeof(const char*) + 16 * 8;
        for (const auto& name : names)
            size += name->size() + 1 + 16;
        for (size_t bytes : sizes)
            size += bytes + 16;
        const size_t page = (size_t) sysconf(_SC_PAGESIZE);
        frozenSize = (size + page - 1) / page * page;
        frozenBase = mmap(nullptr, frozenSize, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        Check(frozenBase != MAP_FAILED, "mmap for a read-only request");

        char* next = static_cast<char*>(frozenBase);
        const auto take = [&](size_t bytes)
        {
            char* at = next;
            next += (bytes + 15) / 16 * 16;
            return at;
        };
        auto* frozen = reinterpret_cast<VkDeviceCreateInfo*>(take(sizeof(VkDeviceCreateInfo)));
        *frozen = info;
        auto* frozenQueue = reinterpret_cast<VkDeviceQueueCreateInfo*>(take(sizeof(VkDeviceQueueCreateInfo)));
        *frozenQueue = queue;
        auto* frozenPriority = reinterpret_cast<float*>(take(sizeof(float)));
        *frozenPriority = priority;
        frozenQueue->pQueuePriorities = frozenPriority;
        frozen->pQueueCreateInfos = frozenQueue;
        if (useCore)
        {
            auto* frozenCore = reinterpret_cast<VkPhysicalDeviceFeatures*>(take(sizeof(VkPhysicalDeviceFeatures)));
            *frozenCore = core;
            frozen->pEnabledFeatures = frozenCore;
        }
        if (!extensions.empty())
        {
            auto* array = reinterpret_cast<const char**>(take(extensions.size() * sizeof(const char*)));
            for (size_t i = 0; i < names.size(); i++)
            {
                char* name = take(names[i]->size() + 1);
                std::memcpy(name, names[i]->c_str(), names[i]->size() + 1);
                array[i] = name;
            }
            frozen->ppEnabledExtensionNames = array;
        }
        VkBaseOutStructure* previous = nullptr;
        for (size_t i = 0; i < order.size(); i++)
        {
            auto* node = reinterpret_cast<VkBaseOutStructure*>(take(sizes[i]));
            std::memcpy(node, order[i], sizes[i]);
            node->pNext = nullptr;
            if (previous != nullptr)
                previous->pNext = node;
            else
                frozen->pNext = node;
            previous = node;
        }
        Check(next <= static_cast<char*>(frozenBase) + frozenSize, "the read-only request fits its pages");
        Check(mprotect(frozenBase, frozenSize, PROT_READ) == 0, "mprotect the request read-only");
        frozenInfo = frozen;
    }

    const VkDeviceCreateInfo* Current() const { return frozenInfo != nullptr ? frozenInfo : &info; }

    ~Request()
    {
        if (frozenBase != nullptr)
            munmap(frozenBase, frozenSize);
    }

    void* frozenBase = nullptr;
    size_t frozenSize = 0;
    const VkDeviceCreateInfo* frozenInfo = nullptr;
};

// Every request built from here on is frozen read-only (game-chain-read-only).
bool gFreezeRequests = false;

// The shapes of request games make. Some are invalid at some API versions; those violations are the game's own,
// and the contract is only that OptiScaler adds none.
enum class Shape
{
    Bare,         // swapchain only
    CoreFeatures, // pEnabledFeatures
    Modern,       // Features2 -> Vulkan11 -> Vulkan12 -> Vulkan13, the usual chain of a 1.3 game
    Features2V12, // Features2 -> Vulkan12, bufferDeviceAddress on
    UnknownFirst, // an unknown struct, then Vulkan12: what follows it is the game's own memory
    Individual,   // Features2 -> timeline semaphore -> buffer device address, a 1.1 game with the extensions
    ExtBda,       // VK_EXT_buffer_device_address and its feature struct
    V14PushOff,   // Vulkan14 struct with pushDescriptor off, the game not asking for push_descriptor
    V14PushAsked, // Vulkan14 struct with pushDescriptor off, the game asking for push_descriptor
    Spoofed,      // names a spoofed enumeration showed: NVX and EXT buffer_device_address
    Count
};

const char* ShapeName(Shape shape)
{
    static const char* names[] = { "bare",       "core-features", "modern",       "features2-v12",  "unknown-first",
                                   "individual", "ext-bda",       "v14-push-off", "v14-push-asked", "spoofed" };
    return names[(int) shape];
}

void Fill(Request& r, Shape shape)
{
    r.Ext(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    switch (shape)
    {
    case Shape::Bare:
        break;
    case Shape::CoreFeatures:
        r.useCore = true;
        r.core.samplerAnisotropy = VK_TRUE;
        break;
    case Shape::Modern:
    {
        r.Add<VkPhysicalDeviceFeatures2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2).features.samplerAnisotropy =
            VK_TRUE;
        r.Add<VkPhysicalDeviceVulkan11Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES)
            .shaderDrawParameters = VK_TRUE;
        r.Add<VkPhysicalDeviceVulkan12Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES)
            .timelineSemaphore = VK_TRUE;
        r.Add<VkPhysicalDeviceVulkan13Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES)
            .dynamicRendering = VK_TRUE;
        break;
    }
    case Shape::Features2V12:
        r.Add<VkPhysicalDeviceFeatures2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
        r.Add<VkPhysicalDeviceVulkan12Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES)
            .bufferDeviceAddress = VK_TRUE;
        break;
    case Shape::UnknownFirst:
        r.Unknown();
        r.Add<VkPhysicalDeviceVulkan12Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES);
        break;
    case Shape::Individual:
        r.Ext(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME).Ext(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
        r.Add<VkPhysicalDeviceFeatures2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
        r.Add<VkPhysicalDeviceTimelineSemaphoreFeatures>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES)
            .timelineSemaphore = VK_TRUE;
        r.Add<VkPhysicalDeviceBufferDeviceAddressFeatures>(
             VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES)
            .bufferDeviceAddress = VK_TRUE;
        break;
    case Shape::ExtBda:
        r.Ext(VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
        r.Add<VkPhysicalDeviceBufferDeviceAddressFeaturesEXT>(
             VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES_EXT)
            .bufferDeviceAddress = VK_TRUE;
        break;
    case Shape::V14PushOff:
    case Shape::V14PushAsked:
        if (shape == Shape::V14PushAsked)
            r.Ext(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME);
        r.Add<VkPhysicalDeviceFeatures2>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2);
        r.Add<VkPhysicalDeviceVulkan14Features>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES).pushDescriptor =
            VK_FALSE;
        break;
    case Shape::Spoofed:
        r.Ext("VK_NVX_binary_import").Ext("VK_NVX_image_view_handle").Ext(VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
        break;
    case Shape::Count:
        break;
    }
}

// ---- The devices ----------------------------------------------------------------------------------------------

std::unique_ptr<FakeGpu> NvidiaGpu(uint32_t api = VK_API_VERSION_1_4)
{
    auto gpu = std::make_unique<FakeGpu>();
    gpu->name = "nvidia";
    gpu->apiVersion = api;
    gpu->extensions = { VK_KHR_SWAPCHAIN_EXTENSION_NAME,
                        "VK_NVX_binary_import",
                        "VK_NVX_image_view_handle",
                        VK_NVX_MULTIVIEW_PER_VIEW_ATTRIBUTES_EXTENSION_NAME,
                        VK_KHR_MULTIVIEW_EXTENSION_NAME,
                        VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
                        VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
                        VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
                        VK_NV_LOW_LATENCY_EXTENSION_NAME,
                        VK_NV_LOW_LATENCY_2_EXTENSION_NAME,
                        VK_KHR_PRESENT_ID_EXTENSION_NAME,
                        VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
                        VK_KHR_GET_MEMORY_REQUIREMENTS_2_EXTENSION_NAME,
                        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
                        VK_KHR_EXTERNAL_SEMAPHORE_EXTENSION_NAME,
                        VK_KHR_DEDICATED_ALLOCATION_EXTENSION_NAME,
                        VK_EXT_DESCRIPTOR_BUFFER_EXTENSION_NAME,
                        VK_KHR_COOPERATIVE_MATRIX_EXTENSION_NAME,
                        VK_NV_COOPERATIVE_MATRIX_2_EXTENSION_NAME,
                        VK_KHR_COMPUTE_SHADER_DERIVATIVES_EXTENSION_NAME,
                        VK_KHR_SHADER_FLOAT16_INT8_EXTENSION_NAME,
                        VK_KHR_8BIT_STORAGE_EXTENSION_NAME,
                        VK_EXT_DESCRIPTOR_INDEXING_EXTENSION_NAME,
                        VK_KHR_VULKAN_MEMORY_MODEL_EXTENSION_NAME,
                        VK_KHR_SHADER_FLOAT_CONTROLS_EXTENSION_NAME,
                        VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
                        VK_EXT_SUBGROUP_SIZE_CONTROL_EXTENSION_NAME,
                        VK_KHR_SHADER_INTEGER_DOT_PRODUCT_EXTENSION_NAME,
                        VK_EXT_MUTABLE_DESCRIPTOR_TYPE_EXTENSION_NAME,
                        VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME,
                        VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME };
    return gpu;
}

// No NVX, no push descriptors, no EXT buffer_device_address, and Device scope missing from its memory model.
std::unique_ptr<FakeGpu> MinimalGpu(uint32_t api = VK_API_VERSION_1_3)
{
    auto gpu = std::make_unique<FakeGpu>();
    gpu->name = "minimal";
    gpu->apiVersion = api;
    gpu->extensions = { VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
                        VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME, VK_KHR_VULKAN_MEMORY_MODEL_EXTENSION_NAME,
                        VK_KHR_MULTIVIEW_EXTENSION_NAME };
    gpu->Lacks(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
               &VkPhysicalDeviceVulkan12Features::vulkanMemoryModelDeviceScope);
    gpu->Lacks(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES,
               &VkPhysicalDeviceVulkanMemoryModelFeatures::vulkanMemoryModelDeviceScope);
    gpu->Lacks(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES,
               &VkPhysicalDeviceVulkan14Features::pushDescriptor);
    return gpu;
}

// Anti-Lag and push descriptors, no NVX; only the EXT buffer_device_address, and no bufferDeviceAddress feature.
std::unique_ptr<FakeGpu> AmdGpu(uint32_t api = VK_API_VERSION_1_3)
{
    auto gpu = std::make_unique<FakeGpu>();
    gpu->name = "amd";
    gpu->apiVersion = api;
    gpu->extensions = { VK_KHR_SWAPCHAIN_EXTENSION_NAME,
                        VK_AMD_ANTI_LAG_EXTENSION_NAME,
                        VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
                        VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME,
                        VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME,
                        VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME,
                        VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME,
                        VK_KHR_MULTIVIEW_EXTENSION_NAME,
                        VK_EXT_MUTABLE_DESCRIPTOR_TYPE_EXTENSION_NAME };
    gpu->Lacks(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
               &VkPhysicalDeviceVulkan12Features::bufferDeviceAddress);
    gpu->Lacks(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES,
               &VkPhysicalDeviceBufferDeviceAddressFeatures::bufferDeviceAddress);
    return gpu;
}

// ---- Running the production hook ------------------------------------------------------------------------------

struct Setup
{
    uint32_t instanceApi = VK_API_VERSION_1_3; // 0: the hook does not know it (the driver applies 1.3)
    bool nr = false;
    bool spoofing = false;
    bool skip = false; // inside ScopedSkipVulkanHooks
    bool d3d = false;  // inside ScopedCreatingD3DDevice, as dxvk and vkd3d-proton create
    bool fg = false;   // an FG output that wants the device

    std::string Label() const
    {
        return "api=" + std::to_string(VK_API_VERSION_MAJOR(instanceApi)) + "." +
               std::to_string(VK_API_VERSION_MINOR(instanceApi)) + (instanceApi == 0 ? "(unknown)" : "") +
               " nr=" + std::to_string(nr) + " spoofing=" + std::to_string(spoofing) + " skip=" + std::to_string(skip) +
               " d3d=" + std::to_string(d3d) + " fg=" + std::to_string(fg);
    }
};

void Install()
{
    o_vkCreateDevice = &FakeCreateDevice;
    o_vkGetPhysicalDeviceFeatures2 = &FakeGetPhysicalDeviceFeatures2;
    o_vkGetInstanceProcAddr = &FakeGetInstanceProcAddr;
    o_vkGetDeviceProcAddr = &FakeGetDeviceProcAddr;
    o_vkEnumerateDeviceExtensionProperties = &FakeEnumerateDeviceExtensionProperties;
    o_vkGetPhysicalDeviceProperties = &FakeGetPhysicalDeviceProperties;
    State::Instance().VulkanInstance = kInstance;
}

// Config and the instance version are process-wide in production too; cases set them before creating.
void Apply(const Setup& setup, std::initializer_list<FakeGpu*> gpus)
{
    Config::Instance()->DlssNrEnabled.value = setup.nr;
    Config::Instance()->VulkanExtensionSpoofing.value = setup.spoofing;
    {
        std::scoped_lock lock(_vkApiVersionMutex);
        _instanceApiVersions.clear();
        if (setup.instanceApi != 0)
            _instanceApiVersions[kInstance] = setup.instanceApi;
    }
    // The driver's version. hkvkCreateInstance raises it to 1.2 for an FG output and records the game's own.
    gInstanceApiVersion = setup.instanceApi != 0 ? setup.instanceApi : VK_API_VERSION_1_3;
    if (setup.fg)
        gInstanceApiVersion = std::max(gInstanceApiVersion, VK_API_VERSION_1_2);
    for (auto* gpu : gpus)
        TrackPhysicalDevice(kInstance, gpu->Handle());
}

struct Outcome
{
    Received got;
    Received game;
    std::vector<std::string> gameViolations;
    bool gameWrittenDuringCall = false;
};

std::string Join(const std::vector<std::string>& list)
{
    std::string out;
    for (const auto& item : list)
        out += (out.empty() ? "" : ", ") + item;
    return "[" + out + "]";
}

// One create through the production hook, with the checks every create must pass.
Outcome Create(const Setup& setup, FakeGpu& gpu, Request& request, const std::string& label)
{
    const auto* info = request.Current();
    Outcome outcome;
    outcome.game = Copy(info);
    outcome.gameViolations = Violations(gpu, outcome.game);
    VkDeviceCreateInfo topLevel = *info;
    const auto extensionArray = [&]
    {
        return std::vector<const char*>(info->ppEnabledExtensionNames,
                                        info->ppEnabledExtensionNames + info->enabledExtensionCount);
    };
    const std::vector<const char*> pointers = extensionArray();

    auto previous = gpu.inCreate;
    gpu.inCreate = [&]
    {
        outcome.gameWrittenDuringCall = !SameRequest(Copy(info), outcome.game);
        if (previous)
            previous();
    };

    IFGFeature_Vk fg;
    gFakeFG = setup.fg ? &fg : nullptr;
    std::optional<ScopedSkipVulkanHooks> skip;
    std::optional<ScopedCreatingD3DDevice> d3d;
    if (setup.skip)
        skip.emplace();
    if (setup.d3d)
        d3d.emplace();

    VkDevice device = VK_NULL_HANDLE;
    const VkResult result = hkvkCreateDevice(gpu.Handle(), info, nullptr, &device);
    gpu.inCreate = previous;
    gFakeFG = nullptr;

    const std::string where = label + " [" + gpu.name + " " + setup.Label() + "]";
    Check(result == VK_SUCCESS && device != VK_NULL_HANDLE, where + ": the create did not reach the driver");
    outcome.got = ReceivedBy(device);

    // The game's request: same struct, same pointers, same contents
    Check(std::memcmp(&topLevel, info, sizeof(topLevel)) == 0, where + ": the game's VkDeviceCreateInfo changed");
    Check(extensionArray() == pointers, where + ": the game's extension pointers changed");
    Check(SameRequest(Copy(info), outcome.game), where + ": the game's request is not what it was after the call");
    // ... and not even for the length of the call: another thread may be reading it, or it may be read-only
    Check(!outcome.gameWrittenDuringCall, where + ": the game's own request was written while the call ran");

    // The driver's request did not change under it
    Check(outcome.got.changedWhileRunning.empty(), where + ": the request changed while the driver ran, to " +
                                                       Join(outcome.got.changedWhileRunning) + " from " +
                                                       Join(outcome.got.extensions));

    // No rule broken that the game had not broken itself. A request with an aggregate features struct its version
    // does not have is invalid from the start, and what follows from it is the game's.
    const bool gameBelowItsVersion = std::ranges::any_of(outcome.gameViolations, [](const std::string& v)
                                                         { return v.ends_with("below its core version"); });
    for (const auto& violation : gameBelowItsVersion ? std::vector<std::string> {} : outcome.got.violations)
    {
        // A game that asked for an extension the device lacks (one a spoofed enumeration showed it) keeps its feature
        // struct when the extension is taken out: the struct is the game's, the invalid request was too.
        const auto without = violation.find(" enabled without ");
        if (without != std::string::npos &&
            std::ranges::find(outcome.gameViolations,
                              "extension the device does not offer: " + violation.substr(without + 17)) !=
                outcome.gameViolations.end())
            continue;
        Check(std::ranges::find(outcome.gameViolations, violation) != outcome.gameViolations.end(),
              where + ": OptiScaler's request breaks a rule the game's did not: " + violation + "; extensions " +
                  Join(outcome.got.extensions));
    }

    // Nothing the game asked for and the device has is dropped, save EXT buffer_device_address given up for the KHR
    for (const auto& extension : outcome.game.extensions)
    {
        if (!gpu.Has(extension) || outcome.got.Has(extension))
            continue;
        Check(extension == VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME &&
                  outcome.got.Has(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME),
              where + ": " + extension + ", asked for by the game and offered by the device, did not reach the driver");
    }

    // Under a skip scope or on dxvk/vkd3d-proton's create, the feature chain is the game's
    if (setup.skip || setup.d3d)
    {
        Check(outcome.got.chain == outcome.game.chain && outcome.got.chainTypes == outcome.game.chainTypes &&
                  outcome.got.enabledFeaturesSet == outcome.game.enabledFeaturesSet,
              where + ": features changed on a create the feature state must leave alone");
        Check(fg.prepared == 0, where + ": FG prepared a device it must not touch");
    }

    return outcome;
}

Outcome CreateShape(const Setup& setup, FakeGpu& gpu, Shape shape, const std::string& label)
{
    Request request;
    Fill(request, shape);
    request.Build();
    if (gFreezeRequests)
        request.Freeze();
    return Create(setup, gpu, request, label + " " + ShapeName(shape));
}

const uint32_t kApis[] = { VK_API_VERSION_1_0, VK_API_VERSION_1_1, VK_API_VERSION_1_2, VK_API_VERSION_1_3,
                           VK_API_VERSION_1_4 };

// Every shape on every device at every version, for the given toggles
int Matrix(Setup base, const std::string& label,
           const std::function<void(const Setup&, FakeGpu&, Shape, const Outcome&)>& extra = nullptr)
{
    int runs = 0;
    for (uint32_t instanceApi :
         { 0u, VK_API_VERSION_1_0, VK_API_VERSION_1_1, VK_API_VERSION_1_2, VK_API_VERSION_1_3, VK_API_VERSION_1_4 })
    {
        for (uint32_t deviceApi : kApis)
        {
            for (auto make : { &NvidiaGpu, &MinimalGpu, &AmdGpu })
            {
                auto gpu = make(deviceApi);
                for (int s = 0; s < (int) Shape::Count; s++)
                {
                    Setup setup = base;
                    setup.instanceApi = instanceApi;
                    Apply(setup, { gpu.get() });
                    const auto outcome = CreateShape(setup, *gpu, (Shape) s, label);
                    if (extra)
                        extra(setup, *gpu, (Shape) s, outcome);
                    ++runs;
                }
            }
        }
    }
    return runs;
}

// ---- Two threads -------------------------------------------------------------------------------------------------

struct Gate
{
    std::mutex mutex;
    std::condition_variable cv;
    bool open = false;
    void Open()
    {
        {
            std::scoped_lock lock(mutex);
            open = true;
        }
        cv.notify_all();
    }
    void Wait()
    {
        std::unique_lock lock(mutex);
        cv.wait(lock, [&] { return open; });
    }
};

enum class Hold
{
    BeforeRead, // between the spoofing's return and the driver reading the request: the A-01 window
    WhileReading,
};

// A's create held inside its driver call while B creates from start to finish on another thread.
void Interleave(const Setup& setupA, const Setup& setupB, Hold hold, const std::string& label)
{
    auto gpuA = NvidiaGpu(VK_API_VERSION_1_3);
    auto gpuB = AmdGpu(VK_API_VERSION_1_3);
    Apply(setupA, { gpuA.get(), gpuB.get() });

    Request requestA, requestB;
    Fill(requestA, Shape::Spoofed);
    requestA.Build();
    Fill(requestB, Shape::Individual);
    requestB.Ext(VK_AMD_ANTI_LAG_EXTENSION_NAME).Ext(VK_KHR_SYNCHRONIZATION_2_EXTENSION_NAME);
    requestB.Ext(VK_EXT_MUTABLE_DESCRIPTOR_TYPE_EXTENSION_NAME).Ext(VK_KHR_EXTERNAL_MEMORY_EXTENSION_NAME);
    requestB.Build();

    // Each alone first: what each device must receive
    const auto aloneA = Create(setupA, *gpuA, requestA, label + " A alone").got;
    const auto aloneB = Create(setupB, *gpuB, requestB, label + " B alone").got;
    Check(aloneA.extensions != aloneB.extensions, label + ": the two requests must differ to show cross-talk");

    Gate aInDriver, bDone;
    std::function<void()> holdA = [&]
    {
        aInDriver.Open();
        bDone.Wait();
    };
    (hold == Hold::BeforeRead ? gpuA->onEntry : gpuA->inCreate) = holdA;

    Outcome a, b;
    std::thread threadA([&] { a = Create(setupA, *gpuA, requestA, label + " A"); });
    std::thread threadB(
        [&]
        {
            aInDriver.Wait();
            b = Create(setupB, *gpuB, requestB, label + " B");
            bDone.Open();
        });
    threadA.join();
    threadB.join();
    gpuA->onEntry = nullptr;
    gpuA->inCreate = nullptr;

    Check(a.got.extensions == aloneA.extensions,
          label + ": A received " + Join(a.got.extensions) + ", alone it receives " + Join(aloneA.extensions));
    Check(a.got.chain == aloneA.chain && a.got.chainTypes == aloneA.chainTypes, label + ": A's features changed");
    Check(b.got.extensions == aloneB.extensions,
          label + ": B received " + Join(b.got.extensions) + ", alone it receives " + Join(aloneB.extensions));
    Check(b.got.chain == aloneB.chain, label + ": B's features changed");
    for (const auto& extension : a.got.extensions)
        Check(gpuA->Has(extension) || aloneA.Has(extension), label + ": A received B's " + extension);
}

// ---- Cases ------------------------------------------------------------------------------------------------------

void GameRequestUnchanged(bool fg)
{
    int written = 0, runs = 0;
    for (bool nr : { false, true })
    {
        for (bool spoofing : { false, true })
        {
            Setup setup;
            setup.nr = nr;
            setup.spoofing = spoofing;
            setup.fg = fg;
            runs += Matrix(setup, fg ? "unchanged+fg" : "unchanged",
                           [&](const Setup&, FakeGpu&, Shape, const Outcome& outcome)
                           { written += outcome.gameWrittenDuringCall; });
        }
    }
    // Create fails on any; counted again here so the log shows the matrix ran with writes checked.
    Check(written == 0, "the game's own feature structs were written during " + std::to_string(written) + " creates");
    std::cout << "  " << runs << " creates; the game's own request written during none of them\n";
}

void GameChainReadOnly()
{
    // The whole request -- create info, queue info, extension array and names, every chained struct -- in
    // read-only pages: a write to any of it, even one restored afterwards, faults.
    gFreezeRequests = true;
    int runs = 0;
    for (int bits = 0; bits < 8; bits++)
    {
        Setup setup;
        setup.nr = bits & 1;
        setup.spoofing = bits & 2;
        setup.fg = bits & 4;
        runs += Matrix(setup, "read-only");
    }
    for (bool d3d : { false, true })
    {
        Setup setup;
        setup.d3d = d3d;
        setup.skip = !d3d;
        setup.spoofing = true;
        setup.nr = true;
        runs += Matrix(setup, "read-only");
    }
    gFreezeRequests = false;
    std::cout << "  " << runs << " creates from a read-only request\n";
}

void PointersLiveUntilReturn()
{
    // Every toggle; Create re-reads every pointer of the request just before the driver returns (ASan on), and the
    // fake driver's deep copy at entry reads them all once already.
    int runs = 0;
    for (int bits = 0; bits < 32; bits++)
    {
        Setup setup;
        setup.nr = bits & 1;
        setup.spoofing = bits & 2;
        setup.skip = bits & 4;
        setup.d3d = bits & 8;
        setup.fg = bits & 16;
        runs += Matrix(setup, "live");
    }
    std::cout << "  " << runs << " creates\n";
}

void InterleavedNative()
{
    Setup setup;
    setup.spoofing = true;
    Interleave(setup, setup, Hold::BeforeRead, "native, held before reading");
    Interleave(setup, setup, Hold::WhileReading, "native, held while reading");
}

void InterleavedD3d()
{
    Setup setup;
    setup.spoofing = true;
    setup.d3d = true;
    Interleave(setup, setup, Hold::BeforeRead, "d3d, held before reading");
    Interleave(setup, setup, Hold::WhileReading, "d3d, held while reading");
}

void InterleavedMixed()
{
    Setup native, d3d, nr;
    native.spoofing = true;
    d3d.spoofing = true;
    d3d.d3d = true;
    nr.spoofing = true;
    nr.nr = true;
    Interleave(d3d, native, Hold::BeforeRead, "d3d then native");
    Interleave(native, d3d, Hold::BeforeRead, "native then d3d");
    Interleave(nr, nr, Hold::BeforeRead, "nr on both");
    Setup skip = native;
    skip.skip = true;
    Interleave(skip, native, Hold::BeforeRead, "skip scope on A only");
}

// A create made on the same thread while the driver is still reading the outer request. The spoofing's lists are
// per thread, not per call, so the hook hands the driver its own copy of the array; Create fails if the outer
// request changed under the driver. The nested device has Anti-Lag and the outer one not: the outer create must not
// clear the nested one's vkAntiLagUpdateAMD on the way out (the per-thread answer is the nested create's by then).
void NestedCreateInDriver()
{
    VulkanHooks::o_vkAntiLagUpdateAMD = nullptr;
    auto gpuA = NvidiaGpu(VK_API_VERSION_1_3);
    auto gpuB = AmdGpu(VK_API_VERSION_1_3);
    Setup setup;
    setup.spoofing = true;
    Apply(setup, { gpuA.get(), gpuB.get() });
    Request requestA, requestB;
    Fill(requestA, Shape::Spoofed);
    requestA.Build();
    Fill(requestB, Shape::Individual);
    requestB.Build();
    bool once = false;
    gpuA->inCreate = [&]
    {
        if (!std::exchange(once, true))
            Create(setup, *gpuB, requestB, "nested B");
    };
    Create(setup, *gpuA, requestA, "outer A");
    gpuA->inCreate = nullptr;
    Check(once, "the nested create ran");
    Check(VulkanHooks::o_vkAntiLagUpdateAMD == &FakeAntiLagUpdate,
          "the nested device's vkAntiLagUpdateAMD survives the outer create's return");
    VulkanHooks::o_vkAntiLagUpdateAMD = nullptr;
}

void AntiLagCrosstalk()
{
    // A lacks Anti-Lag, B has it. One is held once its Anti-Lag answer is recorded -- at its first extension
    // enumeration, which the spoofing makes before it reads that answer -- while the other creates from start to
    // finish. Each create must act on its own device's answer: A never asks for VK_AMD_anti_lag, B keeps it, and the
    // Anti-Lag entry point left behind is B's.
    auto gpuA = NvidiaGpu(VK_API_VERSION_1_3);
    auto gpuB = AmdGpu(VK_API_VERSION_1_3);
    Setup setup;
    Apply(setup, { gpuA.get(), gpuB.get() });

    const auto aloneB = CreateShape(setup, *gpuB, Shape::Bare, "B alone").got;
    Check(aloneB.Has(VK_AMD_ANTI_LAG_EXTENSION_NAME), "B alone asks for Anti-Lag");

    for (int order = 0; order < 2; order++)
    {
        VulkanHooks::o_vkAntiLagUpdateAMD = nullptr;
        FakeGpu& held = order == 0 ? *gpuA : *gpuB;
        FakeGpu& other = order == 0 ? *gpuB : *gpuA;
        Gate heldQueried, otherDone;
        bool once = false;
        held.inEnumerate = [&]
        {
            if (std::exchange(once, true))
                return;
            heldQueried.Open();
            otherDone.Wait();
        };
        Outcome a, b;
        std::thread first([&] { a = CreateShape(setup, held, Shape::Bare, "held"); });
        std::thread second(
            [&]
            {
                heldQueried.Wait();
                b = CreateShape(setup, other, Shape::Bare, "other");
                otherDone.Open();
            });
        first.join();
        second.join();
        held.inEnumerate = nullptr;
        const Received& gotA = order == 0 ? a.got : b.got;
        const Received& gotB = order == 0 ? b.got : a.got;
        const std::string which = order == 0 ? "A held, B in between" : "B held, A in between";
        Check(!gotA.Has(VK_AMD_ANTI_LAG_EXTENSION_NAME), which + ": A, which lacks it, never asks for VK_AMD_anti_lag");
        Check(gotB.Has(VK_AMD_ANTI_LAG_EXTENSION_NAME),
              which + ": B, which has Anti-Lag, keeps VK_AMD_anti_lag while A's create overlaps it");
        // The Anti-Lag output's entry point is B's: A's create, overlapping, neither skips nor clears it
        Check(VulkanHooks::o_vkAntiLagUpdateAMD == &FakeAntiLagUpdate,
              which + ": vkAntiLagUpdateAMD is B's after both creates");
        VulkanHooks::o_vkAntiLagUpdateAMD = nullptr;
    }
}

void CapabilitiesNative()
{
    int runs = 0;
    for (bool spoofing : { false, true })
    {
        Setup setup;
        setup.spoofing = spoofing;
        runs += Matrix(setup, "capabilities");
    }
    std::cout << "  " << runs << " creates\n";
}

void CapabilitiesNr()
{
    int runs = 0;
    for (bool spoofing : { false, true })
    {
        Setup setup;
        setup.nr = true;
        setup.spoofing = spoofing;
        runs += Matrix(setup, "capabilities+nr",
                       [](const Setup&, FakeGpu& gpu, Shape, const Outcome& outcome)
                       {
                           for (const char* want : DlssNr::VkExt::kDevice)
                           {
                               if (!gpu.Has(want))
                                   Check(!outcome.got.Has(want) || outcome.game.Has(want),
                                         std::string("NR asked for ") + want + ", which " + gpu.name + " lacks");
                           }
                       });
    }
    // On a device with both NVX extensions they reach the driver with NR on
    auto gpu = NvidiaGpu(VK_API_VERSION_1_3);
    Setup setup;
    setup.nr = true;
    Apply(setup, { gpu.get() });
    const auto outcome = CreateShape(setup, *gpu, Shape::Modern, "nr on nvidia");
    Check(outcome.got.Has("VK_NVX_binary_import") && outcome.got.Has("VK_NVX_image_view_handle"),
          "NR on: the NVX extensions reach the driver of a device that has them");
    std::cout << "  " << runs << " creates\n";
}

void PushDescriptor(bool supported)
{
    for (bool nr : { false, true })
    {
        for (bool skip : { false, true })
        {
            for (auto shape : { Shape::V14PushOff, Shape::V14PushAsked })
            {
                auto gpu = NvidiaGpu(VK_API_VERSION_1_4);
                if (!supported)
                    gpu->Lacks(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES,
                               &VkPhysicalDeviceVulkan14Features::pushDescriptor);
                Setup setup;
                setup.instanceApi = VK_API_VERSION_1_4;
                setup.nr = nr;
                setup.skip = skip;
                Apply(setup, { gpu.get() });
                const auto outcome = CreateShape(setup, *gpu, shape, "push");
                const auto push = outcome.got.Bit(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_4_FEATURES,
                                                  &VkPhysicalDeviceVulkan14Features::pushDescriptor);
                Check(push.has_value(), "the game's Vulkan14 struct reaches the driver");
                if (outcome.got.Has(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME) &&
                    !outcome.game.Has(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME))
                    Check(*push, "push_descriptor added with VkPhysicalDeviceVulkan14Features::pushDescriptor off");
                if (!supported)
                    Check(!*push, "pushDescriptor turned on where the device lacks it");
                if (supported && !skip && shape == Shape::V14PushOff)
                    Check(outcome.got.Has(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME) && *push,
                          "a device with pushDescriptor gets the extension and the feature on");
            }
        }
    }
}

void BdaNeverBoth()
{
    int runs = 0;
    for (bool nr : { false, true })
    {
        for (bool spoofing : { false, true })
        {
            for (bool d3d : { false, true })
            {
                Setup setup;
                setup.nr = nr;
                setup.spoofing = spoofing;
                setup.d3d = d3d;
                runs += Matrix(setup, "bda",
                               [](const Setup&, FakeGpu&, Shape, const Outcome& outcome)
                               {
                                   const bool both = outcome.got.Has(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME) &&
                                                     outcome.got.Has(VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME);
                                   Check(!both,
                                         "KHR and EXT buffer_device_address together: " + Join(outcome.got.extensions));
                               });
            }
        }
    }
    std::cout << "  " << runs << " creates\n";
}

void MemoryModelDeviceScope()
{
    const auto scopeWithModel = [](const Outcome& outcome, const std::string& where)
    {
        for (auto [sType, model, scope] :
             { std::tuple { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                            BitIndex(&VkPhysicalDeviceVulkan12Features::vulkanMemoryModel),
                            BitIndex(&VkPhysicalDeviceVulkan12Features::vulkanMemoryModelDeviceScope) },
               std::tuple { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES,
                            BitIndex(&VkPhysicalDeviceVulkanMemoryModelFeatures::vulkanMemoryModel),
                            BitIndex(&VkPhysicalDeviceVulkanMemoryModelFeatures::vulkanMemoryModelDeviceScope) } })
        {
            if (const auto* bits = outcome.got.Struct(sType))
                Check(!(*bits)[scope] || (*bits)[model], where + ": vulkanMemoryModelDeviceScope without the model");
        }
    };

    // A device with the memory model and without Device scope: scope never requested, the model is
    auto minimal = MinimalGpu(VK_API_VERSION_1_3);
    for (auto shape : { Shape::Modern, Shape::Bare, Shape::UnknownFirst })
    {
        Setup setup;
        Apply(setup, { minimal.get() });
        const auto outcome = CreateShape(setup, *minimal, shape, "scope");
        scopeWithModel(outcome, ShapeName(shape));
        for (auto sType : { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES,
                            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES })
        {
            if (const auto* bits = outcome.got.Struct(sType))
            {
                const size_t scope =
                    sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES
                        ? BitIndex(&VkPhysicalDeviceVulkan12Features::vulkanMemoryModelDeviceScope)
                        : BitIndex(&VkPhysicalDeviceVulkanMemoryModelFeatures::vulkanMemoryModelDeviceScope);
                Check(!(*bits)[scope], "Device scope requested on a device without it");
            }
        }
    }

    // A device with both: both on, together (e35f1c76)
    auto nvidia = NvidiaGpu(VK_API_VERSION_1_3);
    for (uint32_t api : { VK_API_VERSION_1_1, VK_API_VERSION_1_3 })
    {
        Setup setup;
        setup.instanceApi = api;
        Apply(setup, { nvidia.get() });
        Request request;
        Fill(request, api >= VK_API_VERSION_1_2 ? Shape::Modern : Shape::Bare);
        if (api < VK_API_VERSION_1_2)
            request.Ext(VK_KHR_VULKAN_MEMORY_MODEL_EXTENSION_NAME);
        request.Build();
        const auto outcome = Create(setup, *nvidia, request, "scope on nvidia");
        scopeWithModel(outcome, "nvidia");
        const auto sType = api >= VK_API_VERSION_1_2 ? VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES
                                                     : VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_MEMORY_MODEL_FEATURES;
        const auto* bits = outcome.got.Struct(sType);
        Check(bits != nullptr, "the memory model struct reaches the driver");
        const size_t scope = api >= VK_API_VERSION_1_2
                                 ? BitIndex(&VkPhysicalDeviceVulkan12Features::vulkanMemoryModelDeviceScope)
                                 : BitIndex(&VkPhysicalDeviceVulkanMemoryModelFeatures::vulkanMemoryModelDeviceScope);
        Check((*bits)[scope], "Device scope beside the memory model where the device has both");
    }
}

void ApiVersions()
{
    int runs = 0;
    for (bool fg : { false, true })
    {
        Setup setup;
        setup.spoofing = true;
        setup.nr = true;
        setup.fg = fg;
        runs +=
            Matrix(setup, "api",
                   [](const Setup&, FakeGpu& gpu, Shape, const Outcome& outcome)
                   {
                       // Below 1.2 at the driver, a promoted struct needs its extension
                       if (EffectiveApi(gpu) < VK_API_VERSION_1_2)
                       {
                           for (const auto& [sType, bits] : outcome.got.chain)
                           {
                               const auto* info = FindInfo(sType);
                               if (info->core >= VK_API_VERSION_1_2 && info->extension != nullptr &&
                                   std::ranges::find(outcome.game.chainTypes, sType) == outcome.game.chainTypes.end())
                                   Check(outcome.got.Has(info->extension),
                                         std::string(info->name) + " added below 1.2 without " + info->extension);
                           }
                       }
                   });
    }
    std::cout << "  " << runs << " creates\n";
}

void Enumeration(FakeGpu::Enumerate mode, bool d3d)
{
    for (auto make : { &NvidiaGpu, &AmdGpu, &MinimalGpu })
    {
        for (bool spoofing : { false, true })
        {
            for (bool nr : { false, true })
            {
                for (bool fg : { false, true })
                {
                    for (uint32_t api : { VK_API_VERSION_1_1, VK_API_VERSION_1_3 })
                    {
                        for (auto shape : { Shape::Bare, Shape::Spoofed, Shape::Individual, Shape::Modern })
                        {
                            auto gpu = make(VK_API_VERSION_1_4);
                            gpu->enumerate = mode;
                            // The one that appears late is one the game asks for
                            if (mode == FakeGpu::Enumerate::IncompleteOnce)
                                std::ranges::rotate(gpu->extensions, gpu->extensions.begin() + 1);
                            Setup setup;
                            setup.instanceApi = api;
                            setup.spoofing = spoofing;
                            setup.nr = nr;
                            setup.d3d = d3d;
                            setup.fg = fg;
                            Apply(setup, { gpu.get() });
                            const auto outcome = CreateShape(setup, *gpu, shape, "enumeration");
                            if (mode != FakeGpu::Enumerate::IncompleteOnce)
                            {
                                // Support unknown: nothing optional is added, the game's list goes through
                                for (const auto& extension : outcome.got.extensions)
                                    Check(outcome.game.Has(extension) ||
                                              (extension == VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME &&
                                               outcome.game.Has(VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME)),
                                          "an extension added with the device's list unknown: " + extension);
                            }
                        }
                    }
                }
            }
        }
    }
}

void NrOffOn()
{
    for (uint32_t api : kApis)
    {
        auto gpu = NvidiaGpu(VK_API_VERSION_1_4);
        Setup setup;
        setup.instanceApi = api;
        Apply(setup, { gpu.get() });
        const auto off = CreateShape(setup, *gpu, Shape::Bare, "nr off").got;
        setup.nr = true;
        Apply(setup, { gpu.get() });
        const auto on = CreateShape(setup, *gpu, Shape::Bare, "nr on").got;

        // Off, the KHR buffer_device_address the model names is not added where 1.2 made it core
        if (api >= VK_API_VERSION_1_2)
        {
            Check(!off.Has(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME), "NR off: no KHR buffer_device_address");
            Check(on.Has(VK_KHR_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME), "NR on: the model's KHR buffer_device_address");
        }
        // On adds only the model's names to what off sends
        for (const auto& extension : on.extensions)
            Check(off.Has(extension) ||
                      std::ranges::any_of(DlssNr::VkExt::kDevice, [&](const char* want) { return extension == want; }),
                  "NR on added something that is not the model's: " + extension);
        Check(on.chain == off.chain && on.chainTypes == off.chainTypes, "NR changes no feature");
    }

    // dxvk/vkd3d-proton's device: the spoofing is skipped, the NR block is not
    auto gpu = NvidiaGpu(VK_API_VERSION_1_3);
    Setup setup;
    setup.d3d = true;
    setup.nr = true;
    Apply(setup, { gpu.get() });
    const auto d3d = CreateShape(setup, *gpu, Shape::Bare, "nr on, d3d").got;
    Check(d3d.Has("VK_NVX_binary_import"), "NR on: the NVX extensions reach dxvk/vkd3d-proton's device too");
}

void SkipScopesNested()
{
    auto gpu = NvidiaGpu(VK_API_VERSION_1_3);
    Setup setup;
    setup.spoofing = true;
    Apply(setup, { gpu.get() });

    const auto changesFeatures = [&](const std::string& label)
    {
        Request request;
        Fill(request, Shape::Bare);
        request.Build();
        VkDevice device = VK_NULL_HANDLE;
        Check(hkvkCreateDevice(gpu->Handle(), &request.info, nullptr, &device) == VK_SUCCESS, label);
        const auto& got = ReceivedBy(device);
        for (const auto& extension : got.extensions)
            Check(gpu->Has(extension), label + ": requested what the device lacks");
        return !got.chainTypes.empty() || got.enabledFeaturesSet;
    };

    Check(!ScopedSkipVulkanHooks::Active() && changesFeatures("outside"), "outside any scope the features are added");
    {
        ScopedSkipVulkanHooks outer;
        Check(!changesFeatures("outer"), "inside a scope the features are left alone");
        {
            ScopedSkipVulkanHooks inner;
            Check(!changesFeatures("inner"), "inside two scopes the features are left alone");
        }
        Check(ScopedSkipVulkanHooks::Active() && !changesFeatures("outer again"),
              "the inner scope's end leaves the outer one active");

        bool other = false;
        std::thread thread([&] { other = changesFeatures("another thread"); });
        thread.join();
        Check(other, "a scope on one thread does not skip another thread's create");
    }
    Check(!ScopedSkipVulkanHooks::Active() && changesFeatures("after"), "after both scopes the features are added");

    // And the same through Create's checks, nested
    ScopedSkipVulkanHooks one;
    Setup skip = setup;
    skip.skip = true;
    Matrix(skip, "nested skip");
}

void ExtensionSpoofingOffOn()
{
    for (bool d3d : { false, true })
    {
        auto gpu = AmdGpu(VK_API_VERSION_1_3);
        Setup setup;
        setup.d3d = d3d;
        setup.spoofing = true;
        Apply(setup, { gpu.get() });
        const auto on = CreateShape(setup, *gpu, Shape::Spoofed, "spoofing on").got;
        for (const char* spoofed : { "VK_NVX_binary_import", "VK_NVX_image_view_handle" })
            Check(!on.Has(spoofed), std::string("spoofing on: ") + spoofed + " the device lacks is removed");
        Check(on.Has(VK_KHR_SWAPCHAIN_EXTENSION_NAME) && on.Has(VK_EXT_BUFFER_DEVICE_ADDRESS_EXTENSION_NAME),
              "spoofing on: what the device has stays");

        setup.spoofing = false;
        Apply(setup, { gpu.get() });
        const auto off = CreateShape(setup, *gpu, Shape::Spoofed, "spoofing off").got;
        Check(off.Has("VK_NVX_binary_import"), "spoofing off: the game's own list is not filtered");
    }
}

void D3dPath()
{
    // ScopedCreatingD3DDevice, the production class: per thread, restores the previous state
    Check(!State::Instance().creatingD3DDevice, "not creating a D3D device to start with");
    {
        ScopedCreatingD3DDevice outer;
        {
            ScopedCreatingD3DDevice inner;
        }
        Check(State::Instance().creatingD3DDevice, "the inner scope's end leaves the outer one set");
        bool other = true;
        std::thread thread([&] { other = State::Instance().creatingD3DDevice; });
        thread.join();
        Check(!other, "another thread is not creating a D3D device");
    }
    Check(!State::Instance().creatingD3DDevice, "cleared after the scope");

    for (bool nr : { false, true })
    {
        for (bool spoofing : { false, true })
        {
            Setup setup;
            setup.d3d = true;
            setup.nr = nr;
            setup.spoofing = spoofing;
            setup.fg = true;
            Matrix(setup, "d3d",
                   [](const Setup& setup, FakeGpu&, Shape, const Outcome& outcome)
                   {
                       // dxvk/vkd3d-proton's list goes through; only NR adds, only the model's names
                       for (const auto& extension : outcome.got.extensions)
                           Check(outcome.game.Has(extension) ||
                                     (setup.nr && std::ranges::any_of(DlssNr::VkExt::kDevice, [&](const char* want)
                                                                      { return extension == want; })),
                                 "dxvk/vkd3d-proton's device got " + extension);
                   });
        }
    }
}

} // namespace

int main(int argc, char** argv)
{
    if (argc != 2)
        Fail("usage: contract <case>");
    Install();
    const std::string name = argv[1];
    const std::map<std::string, std::function<void()>> cases = {
        { "game-request-unchanged", [] { GameRequestUnchanged(false); } },
        { "game-request-unchanged-fg", [] { GameRequestUnchanged(true); } },
        { "game-chain-read-only", GameChainReadOnly },
        { "pointers-live-until-return", PointersLiveUntilReturn },
        { "interleaved-native", InterleavedNative },
        { "interleaved-d3d", InterleavedD3d },
        { "interleaved-mixed", InterleavedMixed },
        { "nested-create-in-driver", NestedCreateInDriver },
        { "antilag-crosstalk", AntiLagCrosstalk },
        { "capabilities-native", CapabilitiesNative },
        { "capabilities-nr", CapabilitiesNr },
        { "push-descriptor-off", [] { PushDescriptor(false); } },
        { "push-descriptor-supported", [] { PushDescriptor(true); } },
        { "bda-never-both", BdaNeverBoth },
        { "memory-model-device-scope", MemoryModelDeviceScope },
        { "api-versions", ApiVersions },
        { "enumeration-fails-native",
          []
          {
              Enumeration(FakeGpu::Enumerate::CountFails, false);
              Enumeration(FakeGpu::Enumerate::DataFails, false);
          } },
        { "enumeration-incomplete-native", [] { Enumeration(FakeGpu::Enumerate::IncompleteOnce, false); } },
        { "enumeration-fails-d3d",
          []
          {
              Enumeration(FakeGpu::Enumerate::CountFails, true);
              Enumeration(FakeGpu::Enumerate::DataFails, true);
          } },
        { "enumeration-incomplete-d3d", [] { Enumeration(FakeGpu::Enumerate::IncompleteOnce, true); } },
        { "nr-off-on", NrOffOn },
        { "skip-scopes-nested", SkipScopesNested },
        { "extension-spoofing-off-on", ExtensionSpoofingOffOn },
        { "d3d-path", D3dPath },
    };
    const auto found = cases.find(name);
    if (found == cases.end())
        Fail("unknown case " + name);
    found->second();
    return 0;
}

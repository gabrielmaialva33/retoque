unsigned deviceCalls = 0, allCalls = 0;
NVSDK_NGX_Result result = 1;
ID3D12Device* expectedDevice = nullptr;
unsigned CoreDeviceShutdown(ID3D12Device* device, unsigned* remaining)
{
    assert(device == expectedDevice && device != nullptr);
    assert(remaining && *remaining == 0);
    *remaining = 7; // A real writable output, which the old one-argument call did not supply.
    ++deviceCalls;
    return result;
}
unsigned CoreAllShutdown()
{
    ++allCalls;
    return result;
}

unsigned sdkCalls = 0;
unsigned SdkDeviceShutdown(ID3D12Device* device)
{
    assert(device == expectedDevice);
    ++sdkCalls;
    return result;
}

unsigned dx11Calls = 0, dx11AllCalls = 0;
ID3D11Device* expectedDx11 = nullptr;
unsigned CoreDx11Shutdown(ID3D11Device* device, unsigned* remaining)
{
    assert(device == expectedDx11 && device != nullptr);
    assert(remaining != nullptr && *remaining == 0);
    *remaining = 9;
    ++dx11Calls;
    return result;
}
unsigned CoreDx11AllShutdown() { ++dx11AllCalls; return result; }

unsigned vkCalls = 0, vkAllCalls = 0;
VkDevice expectedVk = nullptr;
unsigned CoreVkShutdown(VkDevice device, unsigned* remaining)
{
    assert(device == expectedVk && device != nullptr);
    assert(remaining != nullptr && *remaining == 0);
    *remaining = 5;
    ++vkCalls;
    return result;
}
unsigned CoreVkAllShutdown() { ++vkAllCalls; return result; }

// The adapter's two debug lines for one device call: entry, then the core's result and remaining count.
void ExpectCoreCallAt(size_t at, VkDevice device, unsigned coreResult, unsigned remaining)
{
    assert(debugLog.size() >= at + 2);
    const auto pointer = static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(device));
    assert(debugLog[at].function == "ShutdownVulkanDevice" && debugLog[at].format == std::string("device 0x{0:X}"));
    assert(debugLog[at].values == std::vector<unsigned long long>({ pointer }));
    assert(debugLog[at + 1].format == std::string("device 0x{0:X}: core result 0x{1:X}, {2} remaining"));
    assert(debugLog[at + 1].values == std::vector<unsigned long long>({ pointer, coreResult, remaining }));
}

void ExpectCoreCallLogged(size_t at, VkDevice device, unsigned coreResult, unsigned remaining)
{
    assert(debugLog.size() == at + 2);
    ExpectCoreCallAt(at, device, coreResult, remaining);
}

// The SDK input's entry and return, around the adapter's pair when it called the core: the public consumer's own lines.
void ExpectPublicBracket(size_t at, VkDevice device, bool coreCalled, unsigned coreResult = 1)
{
    const size_t ret = at + (coreCalled ? 3 : 1);
    assert(debugLog.size() == ret + 1);
    const auto pointer = static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(device));
    assert(debugLog[at].function == "NVSDK_NGX_VULKAN_Shutdown1" && debugLog[at].format == std::string("device 0x{0:X}"));
    assert(debugLog[at].values == std::vector<unsigned long long>({ pointer }));
    if (coreCalled)
        ExpectCoreCallAt(at + 1, device, coreResult, 5);
    assert(debugLog[ret].function == "NVSDK_NGX_VULKAN_Shutdown1");
    assert(debugLog[ret].format == std::string("device 0x{0:X}: returning 0x{1:X}"));
    assert(debugLog[ret].values == std::vector<unsigned long long>({ pointer, NVSDK_NGX_Result_Success }));
}

// The Vulkan core's slot behind the public getter and both of its consumers. Run before the type checks, so a
// one-argument slot fails here on behaviour (the core's output is not supplied) and not only on its declaration.
void VulkanCases()
{
    KernelBaseProxy::address = reinterpret_cast<void*>(&CoreVkShutdown);
    NVNGXProxy::LoadVulkan();
    NVNGXProxy::_module.VULKAN_Shutdown = &CoreVkAllShutdown;
    assert(NVNGXProxy::VULKAN_Shutdown1() == nullptr);
    VkDevice_T device;
    expectedVk = &device;

    // Uninitialised: neither consumer reaches the core.
    ShutdownPresentCore(&device);
    assert(NVSDK_NGX_VULKAN_Shutdown1(&device) == NVSDK_NGX_Result_Success);
    assert(vkCalls == 0 && vkAllCalls == 0 && Nvngx_FG::shutdown1Calls == 1 && Nvngx_FG::shutdownCalls == 1);
    ExpectPublicBracket(0, &device, false); // the host's helper logs nothing of its own; the input brackets its call
    debugLog.clear();

    NVNGXProxy::_vulkanInited = true;
    auto vk = NVNGXProxy::VULKAN_Shutdown1();
    assert(vk != nullptr);
    // Nothing logged until a device call is made; then the entry and the core's own answer, failure included.
    assert(debugLog.empty());
    assert(vk(&device) == 1 && vkCalls == 1 && vkAllCalls == 0);
    ExpectCoreCallLogged(0, &device, 1, 5);
    result = 0xBAD00002;
    assert(vk(&device) == result && vkCalls == 2 && vkAllCalls == 0);
    ExpectCoreCallLogged(2, &device, 0xBAD00002, 5);
    assert(vk(nullptr) == result && vkAllCalls == 1 && vkCalls == 2);
    result = 1;
    NVNGXProxy::_module.VULKAN_Shutdown = nullptr;
    assert(vk(nullptr) == NVSDK_NGX_Result_FAIL_NotImplemented && vkAllCalls == 1 && vkCalls == 2);
    NVNGXProxy::_module.VULKAN_Shutdown = &CoreVkAllShutdown;
    assert(debugLog.size() == 4); // the all-devices routes log nothing: no device call was made

    // The NR present host: the device-specific entry, the runtime left initialised for its caller to clear.
    ShutdownPresentCore(&device);
    assert(vkCalls == 3 && vkAllCalls == 1 && NVNGXProxy::_vulkanInited);
    ExpectCoreCallLogged(4, &device, 1, 5);

    // The SDK input: the device entry once, the runtime cleared, so the all-devices shutdown it ends in is not repeated.
    assert(NVSDK_NGX_VULKAN_Shutdown1(&device) == NVSDK_NGX_Result_Success);
    assert(vkCalls == 4 && vkAllCalls == 1 && !NVNGXProxy::_vulkanInited);
    ExpectPublicBracket(6, &device, true);
    assert(Nvngx_FG::shutdown1Calls == 2 && Nvngx_FG::shutdownCalls == 2 && !State::Instance().nvngxVkInited);

    // A null device from the game selects the all-devices entry instead of dereferencing null inside the core.
    NVNGXProxy::_vulkanInited = true;
    assert(NVSDK_NGX_VULKAN_Shutdown1(nullptr) == NVSDK_NGX_Result_Success);
    assert(vkCalls == 4 && vkAllCalls == 2 && !NVNGXProxy::_vulkanInited);
    ExpectPublicBracket(10, nullptr, false);

    // A failing core result does not stop the input's cleanup.
    NVNGXProxy::_vulkanInited = true;
    result = 0xBAD00002;
    assert(NVSDK_NGX_VULKAN_Shutdown1(&device) == NVSDK_NGX_Result_Success);
    assert(vkCalls == 5 && vkAllCalls == 2 && !NVNGXProxy::_vulkanInited && Nvngx_FG::shutdown1Calls == 4);
    ExpectPublicBracket(12, &device, true, 0xBAD00002); // the core's failure logged; the public return is not it
    result = 1;

    // DLSS off: the input leaves the proxy's core alone.
    NVNGXProxy::_vulkanInited = true;
    Config::Instance()->DLSSEnabled.value = false;
    assert(NVSDK_NGX_VULKAN_Shutdown1(&device) == NVSDK_NGX_Result_Success);
    assert(vkCalls == 5 && vkAllCalls == 2 && NVNGXProxy::_vulkanInited);
    ExpectPublicBracket(16, &device, false);
    Config::Instance()->DLSSEnabled.value = true;

    // A core without the export: nothing exposed, neither consumer calls anything.
    KernelBaseProxy::address = nullptr;
    NVNGXProxy::LoadVulkan();
    assert(NVNGXProxy::VULKAN_Shutdown1() == nullptr);
    const auto logged = debugLog.size();
    ShutdownPresentCore(&device);
    assert(vkCalls == 5 && vkAllCalls == 2 && debugLog.size() == logged); // no call, so no evidence of one
    NVNGXProxy::_vulkanInited = false;
    NVNGXProxy::_module.VULKAN_Shutdown = nullptr;

    assert((std::is_same_v<decltype(NVNGXProxy::VULKAN_Shutdown1()), PFN_VULKAN_Shutdown1>));
    assert((std::is_same_v<decltype(NVNGXProxy::_module.VULKAN_Shutdown1),
                           NVSDK_NGX_Result (*)(VkDevice, unsigned*)>));
}

int main()
{
    VulkanCases();
    static_assert(std::is_same_v<decltype(NVNGXProxy::D3D12_Shutdown1()), PFN_D3D12_Shutdown1>);
    static_assert(std::is_same_v<decltype(NVNGXProxy::D3D11_Shutdown1()), PFN_D3D11_Shutdown1>);
    static_assert(std::is_same_v<decltype(NVNGXProxy::_module.D3D11_Shutdown1),
                                 NVSDK_NGX_Result (*)(ID3D11Device*, unsigned*)>);
    KernelBaseProxy::address = reinterpret_cast<void*>(&CoreDx11Shutdown);
    NVNGXProxy::LoadDx11();
    NVNGXProxy::_module.D3D11_Shutdown = &CoreDx11AllShutdown;
    assert(NVNGXProxy::D3D11_Shutdown1() == nullptr);
    NVNGXProxy::_dx11Inited = true;
    auto dx11 = NVNGXProxy::D3D11_Shutdown1();
    assert(dx11 != nullptr);
    ID3D11Device dx11Device;
    expectedDx11 = &dx11Device;
    assert(dx11(&dx11Device) == 1 && dx11Calls == 1 && dx11AllCalls == 0);
    result = 0xBAD00002;
    assert(dx11(&dx11Device) == result && dx11Calls == 2 && dx11AllCalls == 0);
    assert(dx11(nullptr) == result && dx11AllCalls == 1 && dx11Calls == 2);
    result = 1;
    assert(dx11(nullptr) == 1 && dx11AllCalls == 2 && dx11Calls == 2);
    NVNGXProxy::_module.D3D11_Shutdown = nullptr;
    assert(dx11(nullptr) == NVSDK_NGX_Result_FAIL_NotImplemented);
    assert(dx11AllCalls == 2 && dx11Calls == 2);
    KernelBaseProxy::address = nullptr;
    NVNGXProxy::LoadDx11();
    assert(NVNGXProxy::D3D11_Shutdown1() == nullptr);
    NVNGXProxy::_module.D3D12_Shutdown1 = &CoreDeviceShutdown;
    NVNGXProxy::_module.D3D12_Shutdown = &CoreAllShutdown;
    assert(NVNGXProxy::D3D12_Shutdown1() == nullptr);
    NVNGXProxy::_dx12Inited = true;
    auto call = NVNGXProxy::D3D12_Shutdown1();
    assert(call != nullptr);
    ID3D12Device device;
    expectedDevice = &device;
    assert(call(&device) == 1 && deviceCalls == 1 && allCalls == 0);
    result = 0xBAD00002;
    assert(call(&device) == result && deviceCalls == 2 && allCalls == 0);
    assert(call(nullptr) == result && allCalls == 1 && deviceCalls == 2);
    result = 1;
    assert(call(nullptr) == 1 && allCalls == 2 && deviceCalls == 2);
    NVNGXProxy::_module.D3D12_Shutdown = nullptr;
    assert(call(nullptr) == NVSDK_NGX_Result_FAIL_NotImplemented);
    assert(allCalls == 2 && deviceCalls == 2);
    NVNGXProxy::_module.D3D12_Shutdown1 = nullptr;
    assert(NVNGXProxy::D3D12_Shutdown1() == nullptr);
    // A custom runtime keeps the public SDK signature and must never use the private core slot.
    CustomNgxModule custom;
    custom.shutdown1 = &SdkDeviceShutdown;
    custom.module.D3D12_Shutdown1 = &CoreDeviceShutdown;
    NVNGXProxy::ShutdownCustomDx12(custom, &device);
    assert(sdkCalls == 0 && deviceCalls == 2);
    custom.dx12Inited = true;
    NVNGXProxy::ShutdownCustomDx12(custom, &device);
    assert(sdkCalls == 1 && deviceCalls == 2 && !custom.dx12Inited);
    custom.dx12Inited = true;
    custom.module.D3D12_Shutdown = &CoreAllShutdown;
    NVNGXProxy::ShutdownCustomDx12(custom, &device);
    assert(allCalls == 3 && sdkCalls == 1 && !custom.dx12Inited);
    custom.dx12Inited = true;
    custom.module.D3D12_Shutdown = nullptr;
    custom.shutdown1 = nullptr;
    NVNGXProxy::ShutdownCustomDx12(custom, &device);
    assert(allCalls == 3 && sdkCalls == 1 && deviceCalls == 2 && !custom.dx12Inited);
    std::cout << "PASS: production D3D11/D3D12/Vulkan core shutdown adapters, writable output, result propagation, "
                 "null-device routing, both Vulkan consumers and custom SDK shutdown\n";
}

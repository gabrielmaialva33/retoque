#include <dlssnr/DlssNr_VkCore.h>
#include <dlssnr/DlssNr_VkActivity.h>
#include <dlssnr/DlssNr_VkStorageRequest.h>
#include <cassert>
#include <thread>
#include <tuple>

int main()
{
    using namespace DlssNr;
    VkCore::State core;
    assert(!core.Current());
    const auto ours = core.Start(1);
    assert(core.Matches(ours));
    core.Stop(2); // an unrelated device cannot clear our record
    assert(core.Matches(ours));
    core.Stop(1); // game shutdown before host cleanup
    assert(!core.Matches(ours) && !core.StopIfCurrent(ours));
    const auto same = core.Start(1); // same handle, different core
    assert(same != ours && !core.StopIfCurrent(ours) && core.Matches(same));
    const auto other = core.Start(2);
    assert(!core.Matches(same) && !core.StopIfCurrent(same));
    assert(core.StopIfCurrent(other) && !core.StopIfCurrent(other));
    const auto game = core.Start(1); // game first: borrow; no ownership claim
    assert(core.Current() == game);
    assert(!core.StopIfCurrent({}));
    core.Stop();
    const auto before = core.Generation();
    const auto newerGame = core.Start(1); // game init racing a private init
    assert(!core.StartIfUnchanged(1, before) && core.Matches(newerGame));
    core.Stop();
    const auto privateStart = core.StartIfUnchanged(1, core.Generation());
    assert(privateStart && core.Matches(privateStart));
    const auto gameAfter = core.Start(1); // even a new start without a stop revokes ownership
    assert(!core.StopIfCurrent(privateStart) && core.Matches(gameAfter));
    std::thread reader(
        [&]
        {
            for (int i = 0; i < 1000; ++i)
                (void) core.Current();
        });
    for (int i = 0; i < 1000; ++i)
    {
        core.Stop();
        core.Start(1);
    }
    reader.join();

    VkActivity::State activity;
    assert(!activity.Running(100));
    activity.Recorded(100);
    assert(activity.Running(100) && activity.Running(1099));
    assert(!activity.Running(99) && !activity.Running(1100) && !activity.Running(5000));
    activity.Recorded(6000); // switch back to native
    assert(activity.Running(6001));
    activity.Clear();
    assert(!activity.Running(6001));

    VkDeviceCreateInfo info {};
    info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    VkPhysicalDeviceFeatures features {};
    info.pEnabledFeatures = &features;
    assert(!VkStorage::Enabled(nullptr) && !VkStorage::Enabled(&info));
    features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
    assert(VkStorage::Enabled(&info));
    VkPhysicalDeviceFeatures2 features2 {};
    features2.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
    info.pEnabledFeatures = nullptr;
    info.pNext = &features2;
    assert(!VkStorage::Enabled(&info));
    features2.features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
    assert(VkStorage::Enabled(&info));
    // Owner-authorized request: all writes land in copies; legacy and Features2 are exclusive.
    features2.features.shaderStorageImageWriteWithoutFormat = VK_FALSE;
    VkPhysicalDeviceVulkan11Features prefix {};
    prefix.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES;
    prefix.pNext = &features2;
    info.pNext = &prefix;
    VkStorage::Request chained(&info, true, false, true, VK_API_VERSION_1_2);
    assert(chained.Added() && VkStorage::Enabled(chained.Info(&info)));
    assert(chained.Info(&info)->pEnabledFeatures == nullptr);
    assert(chained.Info(&info)->pNext != &prefix && prefix.pNext == &features2);
    assert(features2.features.shaderStorageImageWriteWithoutFormat == VK_FALSE);
    assert(reinterpret_cast<const VkBaseInStructure*>(chained.Info(&info)->pNext)->pNext !=
           reinterpret_cast<const VkBaseInStructure*>(&features2));
    features.shaderStorageImageWriteWithoutFormat = VK_FALSE;
    features.robustBufferAccess = VK_TRUE;
    info.pNext = nullptr;
    info.pEnabledFeatures = &features;
    VkStorage::Request legacy(&info, true, false, true, VK_API_VERSION_1_2);
    assert(legacy.Added() && legacy.Info(&info)->pEnabledFeatures != &features);
    assert(legacy.Info(&info)->pEnabledFeatures->robustBufferAccess);
    assert(!features.shaderStorageImageWriteWithoutFormat);
    info.pEnabledFeatures = nullptr;
    VkStorage::Request none(&info, true, false, true, VK_API_VERSION_1_2);
    assert(none.Added() && VkStorage::Enabled(none.Info(&info)) && !info.pEnabledFeatures);
    for (auto [nr, internal, supported] :
         { std::tuple { false, false, true }, std::tuple { true, true, true }, std::tuple { true, false, false } })
    {
        VkStorage::Request unchanged(&info, nr, internal, supported, VK_API_VERSION_1_2);
        assert(!unchanged.Added() && unchanged.Info(&info) == &info);
        assert((unchanged.Reason() != nullptr) == (nr && !internal && !supported));
    }
    VkStorage::Request core13(&info, true, false, false, VK_API_VERSION_1_3);
    assert(!core13.Added() && core13.Info(&info) == &info && !core13.Reason());
    const char* ext = VK_KHR_FORMAT_FEATURE_FLAGS_2_EXTENSION_NAME;
    info.enabledExtensionCount = 1;
    info.ppEnabledExtensionNames = &ext;
    VkStorage::Request flags2(&info, true, false, false, VK_API_VERSION_1_2);
    assert(!flags2.Added() && flags2.Info(&info) == &info && VkStorage::Flags2(&info));
    info.enabledExtensionCount = 0;
    info.pEnabledFeatures = &features;
    features.shaderStorageImageWriteWithoutFormat = VK_TRUE;
    VkStorage::Request app(&info, true, false, false, VK_API_VERSION_1_2);
    assert(!app.Added() && app.Info(&info) == &info && !app.Reason());
    info.pEnabledFeatures = nullptr;
    VkBaseInStructure unknown { static_cast<VkStructureType>(123456789),
                                reinterpret_cast<const VkBaseInStructure*>(&features2) };
    info.pNext = &unknown;
    VkStorage::Request unsafe(&info, true, false, true, VK_API_VERSION_1_2);
    assert(!unsafe.Added() && unsafe.Info(&info) == &info && unsafe.Reason());
    assert(!features2.features.shaderStorageImageWriteWithoutFormat);

    VkStorage::Devices devices;
    const auto d = reinterpret_cast<VkDevice>(1);
    const auto pd = reinterpret_cast<VkPhysicalDevice>(2);
    assert(!devices.Destroyed(d)); // empty/off path has no registry mutation
    int logs = 0;
    const auto log = [&](VkDevice device, VkFormat, const char* reason)
    {
        assert(device == d && reason && *reason);
        ++logs;
    };
    const auto good = VK_FORMAT_R16G16B16A16_SFLOAT;
    const auto bad = VK_FORMAT_B10G11R11_UFLOAT_PACK32;
    assert(!devices.Allows(d, good, log) && logs == 1);
    assert(!devices.Allows(d, good, log) && logs == 1);
    const auto query = +[](VkPhysicalDevice physical, VkFormat format, VkFormatProperties2* props)
    {
        assert(physical == reinterpret_cast<VkPhysicalDevice>(2));
        assert(props->sType == VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2);
        auto& flags = *static_cast<VkFormatProperties3*>(props->pNext);
        assert(flags.sType == VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_3);
        flags.optimalTilingFeatures =
            format == VK_FORMAT_R16G16B16A16_SFLOAT ? VK_FORMAT_FEATURE_2_STORAGE_WRITE_WITHOUT_FORMAT_BIT : 0;
    };
    // All three enabling paths; only the 1.3/flags2 paths require per-format query support.
    devices.Created(d, { true, VK_API_VERSION_1_0, false }, pd, nullptr);
    assert(devices.Allows(d, good, log) && devices.Allows(d, bad, log));
    for (const auto facts : { VkStorage::Facts { false, VK_API_VERSION_1_3, false },
                              VkStorage::Facts { false, VK_API_VERSION_1_2, true } })
    {
        devices.Created(d, facts, pd, query);
        const auto before = logs;
        assert(devices.Allows(d, good, log));
        assert(!devices.Allows(d, bad, log) && logs == before + 1);
        assert(!devices.Allows(d, bad, log) && logs == before + 1);
        assert(devices.Allows(d, good, log)); // refusal is format-local, never device-wide
    }
    devices.Created(d, { false, VK_API_VERSION_1_3, false }, pd, nullptr);
    assert(!devices.Allows(d, good, log)); // cannot prove per-format support
    devices.Destroyed(d);
    assert(!devices.Allows(d, good, log)); // reused handle cannot inherit support
    devices.Created(d, { false, VK_API_VERSION_1_2, false }, pd, query);
    assert(!devices.Allows(d, good, log)); // supported format does not enable the capability
}

"""Execute the actual hook call sites with counted Vulkan queries and registry writes."""
from pathlib import Path
import subprocess

ROOT = Path(__file__).resolve().parents[2]


def run(folder):
    native = (ROOT/'OptiScaler/hooks/Vulkan_Hooks.cpp').read_text()
    host = (ROOT/'retoque/dlssnr/DlssNr_VkPresentHost.cpp').read_text()
    native = native[native.index('static VkResult hkvkCreateDevice('):]
    assert 'const bool nr = Config::Instance()->DlssNrEnabled.value_or_default();' in native
    nstart = native.index('        const bool storageInternal =')
    nend = native.index('\n        {\n            // With NR on', nstart)
    observe = native.index('        if (result == VK_SUCCESS && storageEnabled)', nend)
    native_part = native[nstart:nend] + native[observe:native.index('        // Observe while', observe)]
    host = host[host.index('VkResult VKAPI_CALL hkCreateDevice('):]
    assert 'const bool nr = Config::Instance()->DlssNrEnabled.value_or_default();' in host
    hstart = host.index('        if (storageEnabled)')
    hend = host.index('        DeviceCreateFacts facts;', hstart)
    observe = host.index('    if (storageEnabled)\n        DlssNr::VkStorage::Registry().Created')
    host_part = ('const bool storageEnabled = nr && !internal;\n' + host[hstart:hend] +
                 'const auto* effectiveInfo = info;\n' + host[observe:host.index('    DeferredRelease::RegisterOwner', observe)])
    # The off extension plan must retain its original (instance API / enabled feature) decision.
    assert ': atLeast(3) || StorageWithoutFormatEnabled(info);' in host
    source = r'''
#include <cassert>
#include <cstring>
static int logs = 0, notes = 0, driverCalls = 0, lookups = 0, mutations = 0, creates = 0;
template<class... Args> void Log(Args&&...) { ++logs; }
#define LOG_WARN(...) Log(__VA_ARGS__)
#define LOG_INFO(...) Log(__VA_ARGS__)
#define Registry RealRegistry
#define Note RealNote
#include <dlssnr/DlssNr_VkStorageRuntime.h>
#undef Registry
#undef Note
namespace DlssNr::VkStorage {
struct CountedRegistry {
    void Created(VkDevice d, Facts f, VkPhysicalDevice p, PFN_vkGetPhysicalDeviceFormatProperties2 q, bool replace = true) {
        ++creates; ++mutations; RealRegistry().Created(d,f,p,q,replace);
    }
    void Destroyed(VkDevice d) { if (RealRegistry().Destroyed(d)) ++mutations; }
};
CountedRegistry& Registry() { static CountedRegistry registry; return registry; }
void Note(const Request& request) { ++notes; RealNote(request); }
}
static void Properties(VkPhysicalDevice, VkPhysicalDeviceProperties* p) {
    ++driverCalls; p->apiVersion = VK_API_VERSION_1_2;
}
static void Features(VkPhysicalDevice, VkPhysicalDeviceFeatures* p) {
    ++driverCalls; p->shaderStorageImageWriteWithoutFormat = VK_TRUE;
}
static void Formats(VkPhysicalDevice, VkFormat, VkFormatProperties2*) { ++driverCalls; }
static PFN_vkVoidFunction Get(VkInstance, const char* name) {
    ++lookups;
    if (std::strcmp(name,"vkGetPhysicalDeviceProperties") == 0) return reinterpret_cast<PFN_vkVoidFunction>(Properties);
    if (std::strcmp(name,"vkGetPhysicalDeviceFeatures") == 0) return reinterpret_cast<PFN_vkVoidFunction>(Features);
    if (std::strcmp(name,"vkGetPhysicalDeviceFormatProperties2") == 0) return reinterpret_cast<PFN_vkVoidFunction>(Formats);
    return nullptr;
}
struct ScopedSkipVulkanHooks {
    inline static bool active = false;
    static bool Active() { return active; }
};
template<class T> T As(uintptr_t value) { return reinterpret_cast<T>(value); }
static void Native(bool nr, bool internal, VkDevice d) {
    ScopedSkipVulkanHooks::active = internal;
    VkDeviceCreateInfo localCreateInfo {};
    localCreateInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    const auto original = localCreateInfo;
    const auto physicalDevice = reinterpret_cast<VkPhysicalDevice>(2);
    const auto physicalDeviceInstance = reinterpret_cast<VkInstance>(1);
    const auto o_vkGetInstanceProcAddr = Get;
    const auto requestedApiVersion = VK_API_VERSION_1_2;
    const auto result = VK_SUCCESS;
    const auto pDevice = &d;
    // NATIVE
    if (!nr || internal) assert(std::memcmp(&original, &localCreateInfo, sizeof(original)) == 0);
    else assert(DlssNr::VkStorage::Enabled(&localCreateInfo));
}
static void Host(bool nr, bool internal, VkDevice d) {
    VkDeviceCreateInfo original {};
    original.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    const auto* info = &original;
    const auto physical = reinterpret_cast<VkPhysicalDevice>(2);
    const uintptr_t instanceOfPhysical = 1;
    const auto g_getInstanceProcAddr = Get;
    const auto apiVersion = VK_API_VERSION_1_2;
    const auto device = &d;
    DlssNr::VkStorage::PhysicalFacts storagePhysical;
    std::unique_ptr<DlssNr::VkStorage::Request> storageRequest;
    // HOST
    if (!nr || internal) assert(info == &original && !storageRequest);
    else assert(DlssNr::VkStorage::Enabled(info));
    assert(original.pEnabledFeatures == nullptr && original.pNext == nullptr);
}
int main() {
    for (auto create : {Native,Host}) {
      for (bool populated : {false,true}) {
        for (auto [nr,internal] : {std::pair{false,false},std::pair{false,true},std::pair{true,true},std::pair{true,false}}) {
            const auto d = reinterpret_cast<VkDevice>(3);
            const auto other = reinterpret_cast<VkDevice>(4);
            if (populated) DlssNr::VkStorage::RealRegistry().Created(other, {true, VK_API_VERSION_1_0, false}, reinterpret_cast<VkPhysicalDevice>(2), nullptr);
            logs = notes = driverCalls = lookups = mutations = creates = 0;
            create(nr,internal,d);
            if (!nr || internal) assert(logs == 0 && notes == 0 && driverCalls == 0 && lookups == 0 && mutations == 0 && creates == 0);
            else assert(driverCalls == 2 && lookups > 0 && mutations == 1 && creates == 1 && notes == 1 && logs == 1);
            // Actual registry erasure returns false without mutation for an unregistered device.
            DlssNr::VkStorage::Registry().Destroyed(d);
            assert(mutations == (nr && !internal ? 2 : 0));
            DlssNr::VkStorage::Registry().Destroyed(d);
            assert(mutations == (nr && !internal ? 2 : 0));
            if (populated) assert(DlssNr::VkStorage::RealRegistry().Destroyed(other));
        }
      }
    }
}
'''.replace('// NATIVE', native_part).replace('// HOST', host_part)
    variants = [('normal', source, True),
                ('native-off-guard', source.replace('const bool storageEnabled = nr && !storageInternal;',
                                                    'const bool storageEnabled = true;', 1), False),
                ('host-off-guard', source.replace('const bool storageEnabled = nr && !internal;',
                                                  'const bool storageEnabled = true;', 1), False)]
    for name, code, success in variants:
        cpp, exe = folder/(name+'.cpp'), folder/name
        cpp.write_text(code)
        subprocess.run(['g++','-std=c++20','-O1','-g','-Wall','-Wextra','-Werror','-pthread',
                        '-fsanitize=address,undefined','-fno-pie','-no-pie',
                        '-I'+str(ROOT/'retoque'),'-I'+str(ROOT/'external/vulkan/include'),str(cpp),'-o',str(exe)],check=True)
        result = subprocess.run([str(exe)],capture_output=True,text=True)
        assert (result.returncode == 0) == success, (name,result.stdout,result.stderr)
    print('PASS: real native/present hook slices: NR off/internal = zero queries, lookups, notes, logs or registry writes; 2 guard mutants rejected')

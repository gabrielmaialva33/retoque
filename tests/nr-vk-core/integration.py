"""Execute the production proxy adapters against a counted fake core, plus call-site contracts."""
from pathlib import Path
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]

def function(text, signature):
    start = text.index(signature)
    opening = text.index('{', start)
    depth, i = 1, opening + 1
    while depth:
        depth += (text[i] == '{') - (text[i] == '}')
        i += 1
    return text[start:i]


def run(folder):
    proxy = (ROOT/'OptiScaler/proxies/Nvngx_Proxy.h').read_text()
    funcs = '\n'.join(function(proxy, signature) for signature in (
        'static bool InitVulkan(', 'static void SetVulkanInited(',
        'static DlssNr::VkCore::Token VulkanCore()', 'static bool VulkanCoreMatches(',
        'static bool IsVulkanInited()', 'static bool ShutdownOwnedVulkan('))
    source = r'''
#include <dlssnr/DlssNr_VkCore.h>
#include <cassert>
using VkDevice = void*;
using VkInstance = void*;
using VkPhysicalDevice = void*;
using PFN_vkGetInstanceProcAddr = void*;
using PFN_vkGetDeviceProcAddr = void*;
constexpr VkDevice VK_NULL_HANDLE = nullptr;
constexpr int NVSDK_NGX_Result_Success = 1;
struct Module { void* dll = reinterpret_cast<void*>(1); void* VULKAN_Shutdown1 = reinterpret_cast<void*>(1); };
class NVNGXProxy {
public:
inline static DlssNr::VkCore::State _vulkanCore;
inline static Module _module;
inline static int starts = 0, stops = 0;
inline static bool succeed = true;
inline static void (*duringInit)() = nullptr;
static void InitNVNGX() {}
static int InitVulkanModule(Module&, void*, void*, void*, void*, void*) {
    ++starts;
    if (duringInit) duringInit();
    return succeed ? 1 : 0;
}
static void ShutdownVulkanDevice(VkDevice) { ++stops; }
// FUNCTIONS
};
int main() {
    using P = NVNGXProxy;
    const auto d1 = reinterpret_cast<void*>(1), d2 = reinterpret_cast<void*>(2);
    DlssNr::VkCore::Token owned;
    auto init = [&](void* d) { return P::InitVulkan(nullptr,nullptr,d,nullptr,nullptr,&owned); };
    P::succeed = false;
    assert(!init(d1) && !owned && !P::IsVulkanInited());
    P::succeed = true;
    assert(init(d1) && owned && P::VulkanCoreMatches(owned));
    const auto first = owned;
    const auto count = P::starts;
    assert(init(d1) && !owned && P::starts == count); // reused, never privately owned
    assert(!init(d2) && !owned && P::starts == count);
    // Non-owner callers retain the pre-existing initialized-core return behavior.
    assert(P::InitVulkan(nullptr,nullptr,d2,nullptr,nullptr) && P::starts == count);
    P::SetVulkanInited(false,d1); // game shutdown comes first
    assert(!P::ShutdownOwnedVulkan(first,false) && P::stops == 0);
    assert(init(d1) && owned);
    const auto second = owned;
    P::SetVulkanInited(true,d1); // game restarts on the same device
    assert(!P::VulkanCoreMatches(second));
    assert(!P::ShutdownOwnedVulkan(second,false) && P::stops == 0);
    assert(init(d1) && !owned); // game first, host borrows
    P::SetVulkanInited(false,d2); // unrelated shutdown does not affect this device
    assert(P::IsVulkanInited());
    P::SetVulkanInited(false);
    assert(init(d2) && owned);
    const auto third = owned;
    assert(P::ShutdownOwnedVulkan(third,true) && P::stops == 0); // lost: metadata only
    assert(!P::ShutdownOwnedVulkan(third,false));
    assert(init(d1) && owned);
    assert(P::ShutdownOwnedVulkan(owned,false) && P::stops == 1);
    assert(!P::ShutdownOwnedVulkan(owned,false) && P::stops == 1);
    P::duringInit = [] { P::SetVulkanInited(true,reinterpret_cast<void*>(2)); };
    assert(!init(d1) && !owned && P::VulkanCore().device == 2);
}
'''.replace('// FUNCTIONS', funcs)
    cpp, exe = folder/'proxy.cpp', folder/'proxy'
    cpp.write_text(source)
    subprocess.run(['g++','-std=c++20','-O1','-g','-Wall','-Wextra','-Werror',
                    '-fsanitize=address,undefined','-fno-pie','-no-pie','-pthread',
                    '-I'+str(ROOT/'retoque'),str(cpp),'-o',str(exe)], check=True)
    subprocess.run([str(exe)],check=True)
    inputs = (ROOT/'OptiScaler/inputs/NVNGX_DLSS_Vk.cpp').read_text()
    assert inputs.count('SetVulkanInited(true, InDevice)') == 5
    assert 'SetVulkanInited(true);' not in inputs
    feature = (ROOT/'retoque/dlssnr/DlssNrFeature_Vk.cpp').read_text()
    for signature, token in [('void FreeBuild(', 'job.coreToken'),
                             ('void VkPresentModel::Release()', 'm.coreToken'),
                             ('void Free(RetiredSet&', 'set.coreToken'),
                             ('bool VkPresentModel::Record(', 'm.coreToken')]:
        assert f'VulkanCoreMatches({token})' in function(feature,signature)
    assert 'next->coreToken = coreToken;' in feature and 'coreToken = over->coreToken;' in feature
    assert 'old.coreToken = coreToken;' in feature
    assert feature.count('g_nativeActivity.Recorded(OwnerNowMs());') == 2
    assert 'g_vk.feature' not in function(feature,'bool IsRunningVk()')
    host = (ROOT/'retoque/dlssnr/DlssNr_VkPresentHost.cpp').read_text()
    assert 'VkStorage::Describe(effectiveInfo, storagePhysical.apiVersion)' in host
    assert 'return DlssNr::VkStorage::Enabled(info);' in host
    assert 'storageRequest->Info(info)' in host
    hooks = (ROOT/'OptiScaler/hooks/Vulkan_Hooks.cpp').read_text()
    assert 'VkStorage::Request storageRequest(' in hooks and 'storageRequest.Info(&localCreateInfo)' in hooks
    assert 'VkStorage::Registry().Destroyed(device);' in host
    shader = (ROOT/'retoque/shaders/dlssnr/DlssNr_Vk.cpp').read_text()
    assert shader.index('VkStorage::Allows(InDevice,') < shader.index('CreateSampler(')
    hdr = (ROOT/'retoque/shaders/dlssnr/DlssNr_PresentHdr_Vk.h').read_text()
    assert 'if (!VkStorage::Allows(device,' in hdr
    assert 'EncodedFormat(raw)' in function(hdr, 'bool Create(')
    after = function(feature, 'void EvaluateAfterUpscaleVk(')
    assert after.index('if (!VkStorage::Allows(') < after.index('make_unique<DlssNr_Vk>')
    pre = function(feature, 'bool RecordPreUpscaleVk(')
    assert pre.index('if (!VkStorage::Allows(') < pre.index('make_unique<DlssNr_Vk>')
    assert 'colour->Resource.ImageViewInfo.Format' in pre[pre.index('if (!VkStorage::Allows('):pre.index('make_unique<DlssNr_Vk>')]
    assert 'VkStorage::Allows' in function(feature, 'VkPresentAdmission VkPresentModel::Admit(')
    print('PASS: production NGX proxy adapter interleavings and consumer wiring')

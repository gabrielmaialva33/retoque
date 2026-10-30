#!/usr/bin/env python3
"""Compile the actual NGX core shutdown adapter and getter with strict host fakes."""
from pathlib import Path
import subprocess
import sys
import tempfile

here = Path(__file__).resolve().parent
root = here.parents[1]
source = (root / 'OptiScaler/proxies/NVNGX_Proxy.h').read_text()
vk_input = (root / 'OptiScaler/inputs/NVNGX_DLSS_Vk.cpp').read_text()
feature_vk = (root / 'retoque/dlssnr/DlssNrFeature_Vk.cpp').read_text()


def between(start, end, text=source):
    assert text.count(start) == 1, f'production boundary changed: {start}'
    begin = text.index(start)
    return text[begin:text.index(end, begin)]


def definition(text, signature):
    """The one definition that starts with `signature`, braces matched outside comments."""
    assert text.count(signature) == 1, f'production boundary changed: {signature}'
    begin = text.index(signature)
    depth, j = 0, text.index('{', begin)
    while True:
        if text.startswith('//', j):
            j = text.index('\n', j)
            continue
        depth += {'{': 1, '}': -1}.get(text[j], 0)
        if depth == 0:
            return text[begin:j + 1] + '\n'
        j += 1


dx11_types = between('typedef NVSDK_NGX_Result (*PFN_D3D11_Shutdown)',
                     'typedef NVSDK_NGX_Result (*PFN_D3D11_GetParameters)')
dx11_slots = between('    PFN_D3D11_Shutdown D3D11_Shutdown = nullptr;',
                     '    PFN_D3D11_GetParameters D3D11_GetParameters = nullptr;')
dx11_load = between('            _module.D3D11_Shutdown1 =',
                    '            _module.D3D11_GetParameters =')
dx11_getter = between('    static PFN_D3D11_Shutdown1 D3D11_Shutdown1()', '    // DirectX12')
types = between('typedef NVSDK_NGX_Result (*PFN_D3D12_Shutdown)',
                'typedef NVSDK_NGX_Result (*PFN_D3D12_GetParameters)')
adapter = between('    static NVSDK_NGX_Result ShutdownDx12Device(', '    inline static void LogCallback(')
custom = between('struct CustomNgxModule', 'class NVNGXProxy')
custom_shutdown = between('    static void ShutdownCustomDx12(', '    static void SetDx12Inited(')
getter = between('    static PFN_D3D12_Shutdown1 D3D12_Shutdown1()', '    // Vulkan')
vk_types = between('typedef NVSDK_NGX_Result (*PFN_VULKAN_Shutdown)',
                   'typedef NVSDK_NGX_Result (*PFN_VULKAN_GetParameters)')
vk_slots = between('    PFN_VULKAN_Shutdown VULKAN_Shutdown = nullptr;',
                   '    PFN_VULKAN_GetParameters VULKAN_GetParameters = nullptr;')
vk_load = between('            _module.VULKAN_Shutdown1 =', '            _module.VULKAN_GetParameters =')
vk_getter = between('    static PFN_VULKAN_Shutdown1 VULKAN_Shutdown1()', '    static PFN_UpdateFeature UpdateFeature()')
# Both consumers of the public getter: the SDK input (with the all-devices shutdown it ends in) and the NR present host.
vk_public = (definition(vk_input, 'NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_VULKAN_Shutdown(void)') +
             definition(vk_input, 'NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_VULKAN_Shutdown1(VkDevice InDevice)'))
vk_host = definition(feature_vk, 'void ShutdownPresentCore(VkDevice device)')
unit = """
#include <cassert>
#include <cstdint>
#include <string>
#include <type_traits>
#include <iostream>
#include <vector>
using NVSDK_NGX_Result = unsigned;
using UINT = unsigned;
// LOG_DEBUG as SysUtils.h has it, recording the function, the format and the values instead of formatting them.
struct LogRecord
{
    std::string function, format;
    std::vector<unsigned long long> values;
};
inline std::vector<LogRecord> debugLog;
template <typename... Values> void RecordDebug(const char* function, const char* format, Values... values)
{
    debugLog.push_back({ function, format, { static_cast<unsigned long long>(values)... } });
}
#define LOG_DEBUG(msg, ...) RecordDebug(__FUNCTION__, msg, ##__VA_ARGS__)
constexpr NVSDK_NGX_Result NVSDK_NGX_Result_Success = 1;
constexpr NVSDK_NGX_Result NVSDK_NGX_Result_FAIL_NotImplemented = 0xBAD00012;
struct ID3D12Device {};
struct ID3D11Device {};
struct VkDevice_T {};
using VkDevice = VkDevice_T*;
#define VK_NULL_HANDLE nullptr
""" + dx11_types + types + vk_types + """
struct Module
{
    void* dll = nullptr;
""" + dx11_slots + vk_slots + """
    PFN_D3D12_Shutdown D3D12_Shutdown = nullptr;
    PFN_D3D12_CoreShutdown1 D3D12_Shutdown1 = nullptr;
};
using NvngxModule = Module;
struct KernelBaseProxy
{
    inline static void* address = nullptr;
    static void* Resolve(void*, const char*) { return address; }
    static auto GetProcAddress_() { return &Resolve; }
};
""" + custom + """
class NVNGXProxy
{
  public:
    inline static Module _module;
    inline static bool _dx12Inited = false;
    inline static bool _dx11Inited = false;
    inline static bool _vulkanInited = false;
    static void SetVulkanInited(bool value) { _vulkanInited = value; }
    static bool IsVulkanInited() { return _vulkanInited; }
    static PFN_VULKAN_Shutdown VULKAN_Shutdown() { return _vulkanInited ? _module.VULKAN_Shutdown : nullptr; }
    static void LoadDx11() {
""" + dx11_load + """    }
    static void LoadVulkan() {
""" + vk_load + """    }
""" + adapter + dx11_getter + getter + vk_getter + custom_shutdown + """};

// What the SDK input around the Vulkan shutdowns touches, faked.
#define NVSDK_NGX_API
static bool shutdown = false;
void* vkInstance = nullptr;
void* vkPD = nullptr;
void* vkDevice = nullptr;
struct Enabled
{
    bool value = true;
    bool value_or_default() const { return value; }
};
struct Config
{
    Enabled DLSSEnabled;
    static Config* Instance()
    {
        static Config config;
        return &config;
    }
};
struct State
{
    void* currentFeature = nullptr;
    bool nvngxVkInited = true;
    static State& Instance()
    {
        static State state;
        return state;
    }
};
struct Nvngx_FG
{
    inline static unsigned shutdownCalls = 0, shutdown1Calls = 0;
    static NVSDK_NGX_Result VULKAN_Shutdown() { ++shutdownCalls; return NVSDK_NGX_Result_Success; }
    static NVSDK_NGX_Result VULKAN_Shutdown1(VkDevice) { ++shutdown1Calls; return NVSDK_NGX_Result_Success; }
};
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-variable" // production keeps the core's result in an unread local
""" + vk_public + """#pragma GCC diagnostic pop
""" + vk_host + (here / 'cases.h').read_text()
with tempfile.TemporaryDirectory(prefix='optiscaler-ngx-shutdown-') as directory:
    cpp = Path(directory) / 'shutdown.cpp'
    binary = Path(directory) / 'shutdown'
    cpp.write_text(unit)
    subprocess.run(['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
# The runtime evidence judge for the existing Vulkan cases, against synthetic runs (no Wine or GPU here).
subprocess.run([sys.executable, str(here / 'test_vk_shutdown_evidence.py')], check=True)

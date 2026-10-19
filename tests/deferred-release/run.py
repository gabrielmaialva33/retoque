#!/usr/bin/env python3
"""Production deferred queue against a fake clock, including a barrier-controlled collector race."""
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def body(path):
    return '\n'.join(line for line in path.read_text().splitlines()
                     if not line.startswith(('#include ', '#pragma once'))) + '\n'


unit = '''#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <unordered_set>
#include <vector>
struct State { bool isShuttingDown=false; static State& Instance(){static State s; return s;} };
#define LOG_WARN(...) ((void)0)
#define LOG_DEBUG(...) ((void)0)
struct Clock {
    using time_point = std::chrono::steady_clock::time_point;
    static time_point now(){return time_point{} + std::chrono::seconds(tick);}
    inline static int tick=0;
};
'''
unit += body(ROOT / 'OptiScaler/misc/DeferredRelease.h')
# Only the wall clock is replaced; queue extraction, callback execution and synchronization stay verbatim.
unit += body(ROOT / 'OptiScaler/misc/DeferredRelease.cpp').replace('std::chrono::steady_clock', 'Clock')
unit += (HERE / 'cases.cpp').read_text()
with tempfile.TemporaryDirectory(prefix='retoque-deferred-release-') as tmp:
    cpp, exe = Path(tmp) / 'test.cpp', Path(tmp) / 'test'
    cpp.write_text(unit)
    subprocess.run(['g++', '-std=c++20', '-pthread', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer', str(cpp), '-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True, timeout=30)

host = (ROOT / 'retoque/dlssnr/DlssNr_VkPresentHost.cpp').read_text()
destroy = host[host.index('void OnDestroyDevice('):host.index('\nvoid OnGetQueue(')]
assert destroy.index('TeardownDevice(*dev, destroyDeferred)') < destroy.index('DeferredRelease::RetireOwner(H(device), destroyDeferred);')
assert destroy.index('DeferredRelease::RetireOwner(H(device), destroyDeferred);') < destroy.index('next(device, allocator);')
assert 'bool destroyDeferred = false;' in destroy
assert 'destroyDeferred = false;' in host[host.index('std::string TeardownDevice('):]
assert 'destroyDeferred = !unproven && !dev.leaked;' in host
created = host[host.index('VkResult VKAPI_CALL hkCreateDevice('):]
assert created.index('DeferredRelease::RegisterOwner(H(*device));') < created.index('HookDevice(*device);')
print('PASS: NR teardown retires the owner before destroying its device; creation reopens reused handles')

menu = (ROOT / 'OptiScaler/menu/menu_overlay_vk.cpp').read_text()
cleanup = menu[menu.index('static bool DestroyVulkanObjectsLocked(bool shutdown)\n{'):menu.index('void MenuOverlayVk::DestroyVulkanObjects(')]
assert cleanup.index('vkDeviceWaitIdle(') < cleanup.index('DeferredRelease::FlushOwner(') < cleanup.index('_ImVulkan_Info = {};')
assert 'if (!_vulkanDeviceLost && !shutdown)' in cleanup[:cleanup.index('DeferredRelease::FlushOwner(')]
assert 'DeferredRelease::RetireOwner((uint64_t) _ImVulkan_Info.Device, false);' in cleanup
print('PASS: drained menu cleanup flushes deferred resources; device loss abandons them')

hooks = (ROOT / 'OptiScaler/hooks/Vulkan_Hooks.cpp').read_text()
assert 'DeferredRelease::RegisterOwner((uint64_t) *pDevice);' in hooks
print('PASS: the Vulkan bundle also reopens handles when the NR host is disabled')

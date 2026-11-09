#!/usr/bin/env python3
"""Bundle device-destroy hook and real deferred queue under scripted NR/device drain outcomes."""
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[2]
def content(p): return '\n'.join(l for l in p.read_text().splitlines() if not l.startswith(('#include','#pragma once')))
s=(ROOT/'OptiScaler/hooks/Vulkan_Hooks.cpp').read_text();a=s.index('static void hkvkDestroyDevice(');b=s.index('\nVALIDATE_HOOK(',a);hook=s[a:b]
pre=r'''
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <unordered_set>
#include <vector>
#include <optional>
#include <cassert>
#define LOG_FUNC(...) ((void)0)
#define LOG_WARN(...) ((void)0)
#define LOG_DEBUG(...) ((void)0)
struct State { bool isShuttingDown=false; static State& Instance(){static State s;return s;} };
'''
queue=content(ROOT/'OptiScaler/misc/DeferredRelease.h')+'\n'+content(ROOT/'OptiScaler/misc/DeferredRelease.cpp')
fakes=r'''
using VkDevice=void*;struct VkAllocationCallbacks{};
constexpr auto VK_NULL_HANDLE=nullptr;constexpr int VK_SUCCESS=0;
using PFN_vkDeviceWaitIdle=int(*)(VkDevice);
static std::optional<bool> nr;static int waitResult=0,waitCalls=0,callbacks=0;
static std::vector<int> events;
namespace DlssNr::VkPresentHost { std::optional<bool> PrepareForDeviceDestroy(VkDevice){events.push_back(1);return nr;} }
namespace MenuOverlayVk { void ReleaseDevice(VkDevice d){events.push_back(2); DeferredRelease::Enqueue([]{++callbacks;events.push_back(3);},(uint64_t)d);} }
static int idle(VkDevice){++waitCalls;return waitResult;}
static PFN_vkDeviceWaitIdle lookup(VkDevice,const char*){return idle;}
static auto o_vkGetDeviceProcAddr=&lookup;
static void destroyed(VkDevice,const VkAllocationCallbacks*){events.push_back(4);}
static auto o_vkDestroyDevice=&destroyed;
static std::mutex _hookedDevicesMutex,_passThroughSwapchainsMutex;
static std::unordered_set<uint64_t> _hookedDevices;
static std::vector<std::pair<VkDevice,int>> _passThroughSwapchains;
static VkDevice _lowLatency2Device=nullptr,_fifoLatestReadyDevice=nullptr;
namespace VulkanHooks { void* LowLatencySwapchain(){return nullptr;} void ForgetLowLatencySwapchain(void*){} }
namespace ReflexHooks { void deviceDestroyed(void*){} }
namespace InputCommon { void vk_device_destroyed(void*){} }
void NVNGXVkDeviceDestroyed(void*){} void FfxApiVkDeviceDestroyed(void*){}
void FSR2VkDeviceDestroyed(void*){} void XeSSVkDeviceDestroyed(void*){}
'''
cases=r'''
int main(){
 VkDevice device=(void*)42;
 for(int host=0;host<3;++host) for(int driver:{0,-4,-1}) {
  nr=host==0?std::nullopt:std::optional<bool>(host==2);waitResult=driver;waitCalls=callbacks=0;events.clear();
  DeferredRelease::RegisterOwner((uint64_t)device);
  _hookedDevices.insert((uint64_t)device);_passThroughSwapchains={{device,1},{(void*)43,2}};
  hkvkDestroyDevice(device,nullptr);
  const bool expected=nr.value_or(driver==0);
  assert(callbacks==(expected?1:0));assert(waitCalls==(host==0?1:0));
  assert(events.front()==1 && events[1]==2 && events.back()==4);
  assert(!_hookedDevices.contains((uint64_t)device) && _passThroughSwapchains.size()==1);
  // A nested second layer may see an idle device, but cannot resurrect an abandoned owner's callbacks.
  nr=std::nullopt;waitResult=0;hkvkDestroyDevice(device,nullptr);
  assert(callbacks==(expected?1:0));
 }
}
'''
with tempfile.TemporaryDirectory(prefix='retoque-vk-destroy-') as folder:
 p=Path(folder)
 for name,h in [('baseline',hook),('promote-refusal',hook.replace('if (!nrDrained.has_value())','if (true)'))]:
  cpp=p/(name+'.cpp');exe=p/name;cpp.write_text(pre+queue+fakes+h+cases)
  subprocess.run(['g++','-std=c++20','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(cpp),'-o',str(exe)],check=True)
  result=subprocess.run([str(exe)],capture_output=True)
  assert (result.returncode==0)==(name=='baseline'),(name,result.stderr.decode())
print('PASS: NR refusal never promoted; no-host idle/loss/error, producer stop before menu and retire, nested hook tombstone; mutant rejected')

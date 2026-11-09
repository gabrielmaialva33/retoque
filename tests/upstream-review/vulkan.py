from support import source, function, stripped, execute
hooks=source('OptiScaler/hooks/Vulkan_Hooks.cpp')
host=source('retoque/dlssnr/DlssNr_VkPresentHost.cpp')
pre=r'''
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#define LOG_FUNC(...) ((void)0)
#define LOG_WARN(...) ((void)0)
#define LOG_DEBUG(...) ((void)0)
#define LOG_INFO(...) ((void)0)
struct State { bool isShuttingDown=false; static State& Instance(){static State s;return s;} };
'''
queue=stripped(source('OptiScaler/misc/DeferredRelease.h'))+stripped(source('OptiScaler/misc/DeferredRelease.cpp'))
fakes=r'''
using VkDevice=void*;struct VkAllocationCallbacks{};using PFN_vkDestroyDevice=void(*)(VkDevice,const VkAllocationCallbacks*);
constexpr auto VK_NULL_HANDLE=nullptr;constexpr int VK_SUCCESS=0;
using PFN_vkDeviceWaitIdle=int(*)(VkDevice);
static int waitCalls=0,prepareCalls=0,callbacks=0,realDestroy=0,lockDepth=0;
static bool tracked=true,drain=true,queuesAlive=true;
static std::vector<int> events;
static uint64_t H(VkDevice d){return (uint64_t)d;} static std::string Hex(uint64_t){return "device";}
struct HostActivity { HostActivity(const char*,uint64_t){} };
struct TransportLock { TransportLock(){++lockDepth;}~TransportLock(){--lockDepth;} };
struct DeviceTransport { int id=1;std::vector<std::string> lines; };
static DeviceTransport deviceRecord;static std::unordered_map<VkDevice,int> g_transport;
static DeviceTransport* FindDevice(VkDevice){++prepareCalls;return tracked?&deviceRecord:nullptr;}
static std::string TeardownDevice(DeviceTransport&,bool& out){assert(lockDepth);out=drain;tracked=false;DeferredRelease::Enqueue([]{++callbacks;},42);return "";}
static bool DrainUntrackedDevice(VkDevice){assert(lockDepth);++waitCalls;return drain;}
struct StreamlineCounts{};static StreamlineCounts StreamlineNow(){return {};}
static bool g_traceOnValue=false;struct Atomic { bool load(int){return g_traceOnValue;} } g_traceOn;
namespace std { constexpr int memory_order_relaxed=0; }
static std::mutex g_traceMutex,g_registryMutex;
struct Trace { std::vector<std::string> FlushDevice(uint64_t,StreamlineCounts){return {};} } g_trace;
struct Registry { void OnDeviceDestroyed(uint64_t){} } g_registry;
static void Log(const std::vector<std::string>&){}
static void EraseQueueLocks(VkDevice){queuesAlive=false;}
namespace DlssNr::VkPresentHost { std::optional<bool> PrepareForDeviceDestroy(VkDevice);void FinishDeviceDestroy(VkDevice); }
namespace MenuOverlayVk { void ReleaseDevice(VkDevice d,std::optional<bool> =std::nullopt){DeferredRelease::Enqueue([]{++callbacks;},H(d));} }
static int idle(VkDevice){++waitCalls;assert(lockDepth && queuesAlive);return drain?VK_SUCCESS:-1;}
static PFN_vkDeviceWaitIdle lookup(VkDevice,const char*){return idle;}
[[maybe_unused]] static auto o_vkGetDeviceProcAddr=&lookup;
static void destroyed(VkDevice,const VkAllocationCallbacks*){++realDestroy;events.push_back(callbacks);}
static auto o_vkDestroyDevice=&destroyed;
static std::mutex _hookedDevicesMutex,_passThroughSwapchainsMutex;
static std::unordered_set<uint64_t> _hookedDevices;
static std::vector<std::pair<VkDevice,int>> _passThroughSwapchains;
static VkDevice _lowLatency2Device=nullptr,_fifoLatestReadyDevice=nullptr;
struct VulkanHooks {static void* LowLatencySwapchain(){return nullptr;}static void ForgetLowLatencySwapchain(void*){}static void DestroyDevice(PFN_vkDestroyDevice,VkDevice,const VkAllocationCallbacks*);};
namespace ReflexHooks { void deviceDestroyed(void*){} }
namespace InputCommon { void vk_device_destroyed(void*){} }
void NVNGXVkDeviceDestroyed(void* d){DeferredRelease::Enqueue([]{++callbacks;},H(d));}
void FfxApiVkDeviceDestroyed(void*){} void FSR2VkDeviceDestroyed(void*){} void XeSSVkDeviceDestroyed(void*){}
'''
prepare=function(host,'std::optional<bool> PrepareDeviceDestroy(')
exports='std::optional<bool> DlssNr::VkPresentHost::PrepareForDeviceDestroy(VkDevice d){return PrepareDeviceDestroy(d);}\n'
if 'void DlssNr::VkPresentHost::FinishDeviceDestroy(' in host:
    exports+=function(host,'void DlssNr::VkPresentHost::FinishDeviceDestroy(')
else:
    exports+='void DlssNr::VkPresentHost::FinishDeviceDestroy(VkDevice){}\n'
central=function(hooks,'void VulkanHooks::DestroyDevice(') if 'void VulkanHooks::DestroyDevice(' in hooks else ''
hook=function(hooks,'static void hkvkDestroyDevice(')
outer=function(host,'void OnDestroyDevice(')
main=r'''
int main(int argc,char**argv){
 assert(argc==2);const int mode=std::atoi(argv[1]);VkDevice d=(void*)42;
 tracked=(mode==1 || mode==2);drain=mode<2;DeferredRelease::RegisterOwner(H(d));
 if(mode==0 || mode==3)hkvkDestroyDevice(d,nullptr);else OnDestroyDevice(hkvkDestroyDevice,d,nullptr);
 assert(realDestroy==1);
 assert(prepareCalls==1); // Both actual hook bodies must share one prepare.
 assert(waitCalls==((mode==0 || mode==3)?1:0)); // Never retry a tracked failed drain after the first hook forgot it.
 assert(callbacks==(drain?(mode==0?2:3):0)); // HudCopy and menu children enqueue before retirement.
 assert(events.size()==1 && events[0]==callbacks); // Every proven child destroyed before the real device.
 DeferredRelease::Enqueue([]{assert(false);},H(d));DeferredRelease::FlushOwner(H(d));
 assert(!queuesAlive);
}
'''
# The unused fallback only exists in the old version; keep the same boundary fake for both revisions.
fakes=fakes.replace('static bool DrainUntrackedDevice','[[maybe_unused]] static bool DrainUntrackedDevice')
execute('1-vulkan-destroy-protocol',pre+queue+fakes+prepare+exports+central+hook+outer+main,('0','1','2','3'))

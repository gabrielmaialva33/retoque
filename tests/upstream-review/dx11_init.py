from support import source,function,execute
s=source('OptiScaler/inputs/NVNGX_DLSS_Dx11.cpp');a=s.index('        Upscaler upscalerChoice = Upscaler::FSR22;');b=s.index('        LOG_INFO("Creating new {} feature"',a);selection=s[a:b]
main=source('OptiScaler/dllmain.cpp');block=function(main,'if (GetDllNameWModule(&nvapiNamesW)')
pre=r'''
#include <cassert>
#include <cstdlib>
#include <optional>
#include <atomic>
#define LOG_INFO(...) ((void)0)
enum class Upscaler{FSR22,DLSS,DLSS_on12};enum class VendorId{Invalid,Nvidia};
struct Option{bool v=true;bool value_or_default()const{return v;}};
struct Config{Option DLSSEnabled,DlssNrEnabled,DisableFlipMetering;std::optional<Upscaler> Dx11Upscaler;static Config* Instance(){static Config c;return &c;}};
static int initCalls=0,enumerations=0,loads=0;static bool initResult=true,hint=true;static void* module=nullptr;
static void* D3D11Device=(void*)1;static int nvapiNamesW=0;
struct Gpu{bool dlssCapable=true;VendorId vendorId=VendorId::Nvidia;};
struct IdentifyGpu{static Gpu getPrimaryGpu(){++enumerations;return {};}static bool hasNvidiaHint(){return hint;}};
struct NVNGXProxy{static bool InitDx11(void*){++initCalls;return initResult;}};
namespace DlssNr::Dx11Route{enum class Kind{Other,NativeDlss,BridgedDlss};constexpr char kAutoChoiceText[]="bridge";
'''+function(source('retoque/dlssnr/DlssNr_Dx11Route.h'),'inline bool PreferBridge(')+r'''
}
namespace DlssNr::Dx11RouteDetail{static std::atomic<bool> g_autoChoiceLogged{false};
'''+function(source('retoque/dlssnr/DlssNr_Dx11Upscaler.h'),'inline Dx11Route::Kind KindOf(')+r'''
}
namespace DlssNr{
'''+function(source('retoque/dlssnr/DlssNr_Dx11Upscaler.h'),'inline ::Upscaler Dx11DefaultUpscaler(')+r'''
}
static void* GetDllNameWModule(int*){return module;}
namespace LibraryLoadHooks{struct Loaded{~Loaded(){}};Loaded LoadNvApi(){++loads;return {};}}
static Upscaler Choose(){
'''+selection+'''return upscalerChoice;}\nstatic void UnderLoaderLock(){'''+block+'''}\n'''
post=r'''
int main(int argc,char**argv){assert(argc==2);auto& c=*Config::Instance();
 if(std::atoi(argv[1])==0){
  assert(Choose()==Upscaler::DLSS_on12 && initCalls==0);
  c.DlssNrEnabled.v=false;assert(Choose()==Upscaler::DLSS && initCalls==1);
  initResult=false;assert(Choose()==Upscaler::FSR22 && initCalls==2);
  c.DlssNrEnabled.v=true;c.Dx11Upscaler=Upscaler::FSR22;
  assert(Choose()==Upscaler::FSR22 && initCalls==2);
 }else{
  UnderLoaderLock();assert(loads==1 && enumerations==0);
  hint=false;UnderLoaderLock();assert(loads==1 && enumerations==0);
  module=(void*)1;UnderLoaderLock();assert(loads==2 && enumerations==0);
 }
}
'''
execute('8-DX11-lazy-and-loader',pre+post,('0','1'))

from support import source,function,execute
s=source('OptiScaler/proxies/NVNGX_Proxy.h')
types=s[s.index('typedef NVSDK_NGX_Result (*PFN_D3D12_Shutdown)'):s.index('typedef NVSDK_NGX_Result (*PFN_D3D12_GetParameters)')]
custom=function(s,'struct CustomNgxModule')+';\n'
shutdown=function(s,'    static void ShutdownCustomDx12(')
pre=r'''
#include <cassert>
#include <type_traits>
#include <cstdlib>
using NVSDK_NGX_Result=unsigned;constexpr unsigned NVSDK_NGX_Result_Success=1;struct ID3D12Device{};
#define LOG_DEBUG(...) ((void)0)
'''+types+r'''
struct NvngxModule{PFN_D3D12_Shutdown D3D12_Shutdown=nullptr;PFN_D3D12_CoreShutdown1 D3D12_Shutdown1=nullptr;};
'''+custom+'struct NVNGXProxy{'+shutdown+'};\n'+r'''
static int argsSeen=0,allCalls=0;static ID3D12Device* expected=nullptr;
[[maybe_unused]] static unsigned one(ID3D12Device* d){assert(d==expected);argsSeen=1;return 1;}
[[maybe_unused]] static unsigned two(ID3D12Device* d,unsigned* remaining){assert(d==expected && remaining && *remaining==0);*remaining=3;argsSeen=2;return 1;}
static unsigned all(){++allCalls;return 1;}
template<class T>void bind(T& slot){if constexpr(std::is_same_v<T,PFN_D3D12_Shutdown1>)slot=one;else slot=two;}
'''
slot='custom.shutdown1' if 'shutdown1 = nullptr' in custom else 'custom.module.D3D12_Shutdown1'
post=r'''
int main(int argc,char**argv){assert(argc==2);ID3D12Device device;expected=&device;CustomNgxModule custom;bind(SLOT);
 NVNGXProxy::ShutdownCustomDx12(custom,&device);assert(argsSeen==0);
 custom.dx12Inited=true;
 if(std::atoi(argv[1])==0){
  NVNGXProxy::ShutdownCustomDx12(custom,&device);assert(argsSeen==2 && !custom.dx12Inited);
  custom.dx12Inited=true;custom.module.D3D12_Shutdown=all;
  NVNGXProxy::ShutdownCustomDx12(custom,&device);assert(allCalls==1 && argsSeen==2);
 }else{
  expected=nullptr;NVNGXProxy::ShutdownCustomDx12(custom,nullptr);assert(argsSeen==0);
 }
}
'''.replace('SLOT',slot)
execute('7-custom-raw-core-ABI',pre+post,('0','1'))

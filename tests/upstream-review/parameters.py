from support import source,function,execute
proxy=source('OptiScaler/proxies/NVNGX_Proxy.h')
pre=r'''
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <string_view>
#define NVSDK_NGX_API
#define LOG_FUNC(...) ((void)0)
#define LOG_INFO(...) ((void)0)
#define LOG_DEBUG(...) ((void)0)
#define LOG_WARN(...) ((void)0)
using NVSDK_NGX_Result=int;using UINT=unsigned;
constexpr int NVSDK_NGX_Result_Success=1,NVSDK_NGX_Result_Fail=0,NVSDK_NGX_Result_FAIL_InvalidParameter=-1;
enum class API{DX11,DX12,Vulkan};
namespace NGX_AllocTypes {constexpr uint32_t Unknown=0,NVDynamic=1,InternDynamic=2,NVPersistent=3;constexpr std::string_view AllocKey="alloc";}
struct NVSDK_NGX_Parameter{uint32_t kind=0;virtual ~NVSDK_NGX_Parameter()=default;int Get(const char*,uint32_t* out){*out=kind;return 1;}};
struct NVNGX_Parameters:NVSDK_NGX_Parameter{NVNGX_Parameters(API,bool){kind=2;}};
static void InitNGXParameters(NVSDK_NGX_Parameter*,API){}
static void SetNGXParamAllocType(NVSDK_NGX_Parameter& p,uint32_t k){p.kind=k;}
static int frees=0,gets=0;
static NVSDK_NGX_Parameter persistent;
static int nativeGet(NVSDK_NGX_Parameter** p){++gets;*p=&persistent;return 1;}
static int nativeAlloc(NVSDK_NGX_Parameter** p){*p=new NVSDK_NGX_Parameter;return 1;}
static int nativeFree(NVSDK_NGX_Parameter* p){++frees;delete p;return 1;}
struct Option{bool enabled=true;bool value_or_default(){return enabled;}};
struct Config{Option DLSSEnabled;static Config* Instance(){static Config c;return &c;}};
namespace UpscalerInputsDx11wDx12{void Reset(){}}
namespace UpscalerInputsDx12{void Reset(){}}
using Free=int(*)(NVSDK_NGX_Parameter*);
using PFN_D3D11_DestroyParameters=Free;using PFN_D3D12_DestroyParameters=Free;using PFN_VULKAN_DestroyParameters=Free;
struct Module{Free D3D11_DestroyParameters=nativeFree,D3D12_DestroyParameters=nativeFree,VULKAN_DestroyParameters=nativeFree;};
struct NVNGXProxy{
 static inline bool _dx11Inited=false,_dx12Inited=false,_vulkanInited=false;
 static inline Module _module;
 static void* NVNGXModule(){return (void*)1;}
 static bool IsDx11Inited(){return _dx11Inited;}static bool IsDx12Inited(){return _dx12Inited;}static bool IsVulkanInited(){return _vulkanInited;}
 static auto D3D11_GetParameters(){return nativeGet;}static auto D3D12_GetParameters(){return nativeGet;}static auto VULKAN_GetParameters(){return nativeGet;}
 static auto D3D11_AllocateParameters(){return nativeAlloc;}static auto D3D12_AllocateParameters(){return nativeAlloc;}static auto VULKAN_AllocateParameters(){return nativeAlloc;}
'''
free=function(source('OptiScaler/NVNGX_Parameter.h'),'template <typename PFN_DestroyNGXParameters>')
failed=[]
for api,file in (('D3D11','Dx11'),('D3D12','Dx12'),('VULKAN','Vk')):
    s=source(f'OptiScaler/inputs/NVNGX_DLSS_{file}.cpp')
    getters=function(proxy,f'    static PFN_{api}_DestroyParameters {api}_DestroyParameters()')
    public=''.join(function(s,f'NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_{api}_{op}(') for op in ('GetParameters','AllocateParameters','DestroyParameters'))
    post=r'''
int main(int argc,char** argv){
 assert(argc==2);NVSDK_NGX_Parameter* p=nullptr;
 if(std::atoi(argv[1])==0){
  assert(GET(nullptr)==-1);assert(GET(&p)==1);
  assert(gets==1 && p==&persistent && p->kind==NGX_AllocTypes::NVPersistent);
  // SDK-owned persistent maps must never be deleted by DestroyParameters.
  assert(DESTROY(p)==NVSDK_NGX_Result_Fail && frees==0);
 }else{
  assert(ALLOC(&p)==1 && p->kind==NGX_AllocTypes::NVDynamic);
  assert(DESTROY(p)==1 && frees==1); // init was skipped, failed, or already shut down.
 }
}
'''.replace('GET(',f'NVSDK_NGX_{api}_GetParameters(').replace('ALLOC(',f'NVSDK_NGX_{api}_AllocateParameters(').replace('DESTROY(',f'NVSDK_NGX_{api}_DestroyParameters(')
    try:execute('2-parameters-'+api,pre+getters+'};\n'+free+public+post,('0','1'))
    except SystemExit:failed.append(api)
if failed:raise SystemExit('FAIL: '+', '.join(failed))

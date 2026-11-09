#!/usr/bin/env python3
"""A loaded core may allocate parameters before successful init; later lazy DLSS must receive its table."""
from pathlib import Path
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[2]

def function(source, anchor):
    a = source.index(anchor); b = source.index('{', a); end = b + 1; depth = 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}'); end += 1
    return source[a:end]

pre = r'''
#include <cassert>
#include <cstdio>
#define NVSDK_NGX_API
#define LOG_FUNC(...) ((void)0)
#define LOG_INFO(...) ((void)0)
using NVSDK_NGX_Result = int;
constexpr int NVSDK_NGX_Result_Success=1, NVSDK_NGX_Result_FAIL_InvalidParameter=-1;
enum class API {DX11, DX12, Vulkan};
struct NVSDK_NGX_Parameter { int kind=0; virtual ~NVSDK_NGX_Parameter()=default; };
struct NVNGX_Parameters:NVSDK_NGX_Parameter { NVNGX_Parameters(API,bool){kind=2;} };
namespace NGX_AllocTypes { constexpr int NVDynamic=1; }
static void SetNGXParamAllocType(NVSDK_NGX_Parameter& p,int tag){p.kind=tag;}
struct Option { bool enabled=true; bool value_or_default(){return enabled;} };
struct Config { Option DLSSEnabled; static Config* Instance(){static Config c; return &c;} };
static int calls=0, result=1; static bool module=true, inited=false, entry=true;
static NVSDK_NGX_Result Allocate(NVSDK_NGX_Parameter** out){++calls;if(result==1)*out=new NVSDK_NGX_Parameter;return result;}
struct NVNGXProxy {
 static void* NVNGXModule(){return module?(void*)1:nullptr;}
 static bool IsDx11Inited(){return inited;} static bool IsDx12Inited(){return inited;} static bool IsVulkanInited(){return inited;}
 static auto D3D11_AllocateParameters(){return entry?Allocate:nullptr;}
 static auto D3D12_AllocateParameters(){return entry?Allocate:nullptr;}
 static auto VULKAN_AllocateParameters(){return entry?Allocate:nullptr;}
};
'''
post = r'''
int main(){
 assert(ALLOC(nullptr)==NVSDK_NGX_Result_FAIL_InvalidParameter && calls==0);
 for(bool ready:{false,true}){
  inited=ready; calls=0; NVSDK_NGX_Parameter* p=nullptr;
  assert(ALLOC(&p)==1);
  // Native allocation is allowed even before a successful init, as the real core does in loopback.
  // Skipping it leaves an internal table that is later passed to the now-initialized native DLSS.
  assert(calls==1 && p && p->kind==1); delete p;
 }
 for(int refusal=0;refusal<4;++refusal){
  module=refusal!=0;entry=refusal!=1;Config::Instance()->DLSSEnabled.enabled=refusal!=2;
  result=refusal==3?-1:1;calls=0;NVSDK_NGX_Parameter* p=nullptr;
  assert(ALLOC(&p)==1 && p && p->kind==2);
  assert(calls==(refusal==3?1:0)); delete p;
 }
}
'''
failed=[]
with tempfile.TemporaryDirectory(prefix='ngx-parameter-lazy-') as folder:
    for api,file in (('D3D11','Dx11'),('D3D12','Dx12'),('VULKAN','Vk')):
        name='NVSDK_NGX_'+api+'_AllocateParameters'
        source=(ROOT/f'OptiScaler/inputs/NVNGX_DLSS_{file}.cpp').read_text()
        body=function(source,'NVSDK_NGX_API NVSDK_NGX_Result '+name+'(')
        cpp=Path(folder)/(api+'.cpp');exe=Path(folder)/api
        cpp.write_text('#include <initializer_list>\n'+pre+body+post.replace('ALLOC',name))
        subprocess.run(['g++','-std=c++20','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(cpp),'-o',str(exe)],check=True)
        r=subprocess.run([str(exe)],capture_output=True,text=True)
        print(('PASS' if r.returncode==0 else 'FAIL')+': '+api+' parameters before lazy init',flush=True)
        if r.returncode: failed.append(api);print(r.stderr,flush=True)
if failed: raise SystemExit('Failed: '+', '.join(failed))

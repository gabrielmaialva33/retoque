from support import source,function,execute
s=source('OptiScaler/hooks/DxgiFactory_WrappedCalls.cpp')
a=s.index('                    DXGI_SWAP_CHAIN_DESC fgDesc = localDesc;')
b=s.index('                    if (SUCCEEDED(realScResult) && realDx11SwapChain',a)
block=s[a:b]
pre=r'''
#include <cassert>
#include <cstdint>
#include <initializer_list>
#define TRUE 1
#define FALSE 0
#define SUCCEEDED(x) ((x)>=0)
#define FAILED(x) ((x)<0)
#define LOG_WARN(...) ((void)0)
#define IID_PPV_ARGS(x) x
using HRESULT=int;using UINT=unsigned;constexpr int E_FAIL=-1;
struct DXGI_SWAP_CHAIN_DESC{int Windowed=FALSE;void* OutputWindow=nullptr;};
struct IDXGISwapChain4{};
struct IDXGISwapChain{void QueryInterface(IDXGISwapChain4** out){static IDXGISwapChain4 s;*out=&s;}};
static bool realFG=false;static int observed=-1;
struct Factory{HRESULT CreateSwapChain(void*,DXGI_SWAP_CHAIN_DESC* d,IDXGISwapChain** out){observed=d->Windowed;static IDXGISwapChain sc;*out=&sc;return 0;}};
struct ScopedSkipFGSCCreation{~ScopedSkipFGSCCreation(){}};struct ScopedSkipParentWrapping{~ScopedSkipParentWrapping(){}};
namespace DxgiFactoryHooks{bool IsTearingSupported(Factory*){return true;}bool PrepareDx12InteropDesc(DXGI_SWAP_CHAIN_DESC&,bool){return true;}}
namespace FGHooks{HRESULT CreateSwapChain(Factory* f,void* q,DXGI_SWAP_CHAIN_DESC* d,IDXGISwapChain** out){return realFG?f->CreateSwapChain(q,d,out):-1;}}
struct Dx11wDx12SC{template<class F>static HRESULT RetryAfterTakeover(void*,HRESULT r,F){return r;}};
int main(){for(bool provider:{false,true}){
 realFG=provider;Factory f;auto* realFactory=&f;auto* wrappedFactory=&f;void* pDevice=nullptr; (void)pDevice;
 void* dx12Queue=nullptr;DXGI_SWAP_CHAIN_DESC localDesc;HRESULT realScResult=0;
'''
# The extracted creation block has state used by the surrounding production function.
post='''assert(observed==TRUE);assert(fgSwapChain4);(void)fgSwapChainIsRealFG;}}'''
failed=False
try:execute('3-windowed-presenter',pre+block+post)
except SystemExit:failed=True
s=source('OptiScaler/with_dx12/dx11_with_dx12_sc.cpp')
resize=function(s,'HRESULT STDMETHODCALLTYPE Dx11wDx12SC::ResizeBuffers1(')
a=resize.index('        if (State::Instance().currentFeature == nullptr)')
state_block=function(resize[a:],'if (State::Instance().currentFeature == nullptr)')
unit=r'''
#include <cassert>
#include <cstdlib>
#define STDMETHODCALLTYPE
#define SUCCEEDED(x) ((x)>=0)
using HRESULT=int;using BOOL=int;constexpr int TRUE=1,FALSE=0,S_OK=0,DXGI_ERROR_DEVICE_REMOVED=-1;
using UINT=unsigned;
struct IDXGIOutput{void AddRef(){}};
struct DXGI_SWAP_CHAIN_DESC{void* OutputWindow=nullptr;BOOL Windowed=TRUE;};
struct Chain{HRESULT GetDesc(DXGI_SWAP_CHAIN_DESC* d){d->Windowed=TRUE;return 0;}HRESULT GetFullscreenState(BOOL* v,IDXGIOutput**){*v=FALSE;return 0;}};
struct Dx11wDx12SC{Chain* _real=nullptr;Chain* _fgSwapChain=nullptr;void* _handle=nullptr;bool _emulatedFullscreen=true;IDXGIOutput* _emulatedTarget=nullptr;bool _EmulatesFullscreen(){return true;}HRESULT GetDesc(DXGI_SWAP_CHAIN_DESC*);HRESULT GetFullscreenState(BOOL*,IDXGIOutput**);};
struct State{void* currentFeature=nullptr;float screenWidth=0,screenHeight=0,lastMipBias=0,lastMipBiasMax=0;static State& Instance(){static State s;return s;}};
'''+function(s,'HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetFullscreenState(')+function(s,'HRESULT STDMETHODCALLTYPE Dx11wDx12SC::GetDesc(')+r'''
int main(int argc,char** argv){assert(argc==2);
 if(std::atoi(argv[1])==0){
 [[maybe_unused]]UINT Width=0,Height=0,hiddenWidth=841,hiddenHeight=1356;
'''+state_block+r'''
 assert(State::Instance().screenWidth==841 && State::Instance().screenHeight==1356);
 }else{
 Chain hidden,presenter;Dx11wDx12SC sc;sc._real=&hidden;sc._fgSwapChain=&presenter;
 DXGI_SWAP_CHAIN_DESC d;BOOL full=FALSE;
 assert(sc.GetFullscreenState(&full,nullptr)==0 && full);assert(sc.GetDesc(&d)==0 && d.Windowed==FALSE);
 sc._emulatedFullscreen=false;assert(sc.GetDesc(&d)==0 && d.Windowed==TRUE);
 }
}
'''
try:execute('6-bridge-state',unit,('0','1'))
except SystemExit:failed=True
if failed:raise SystemExit(1)

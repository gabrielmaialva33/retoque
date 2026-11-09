import ast
from support import source,function,execute
s=source('OptiScaler/hooks/FG_Hooks.cpp')
tree=ast.parse(source('tests/fg-locks/teardown_cases.py'))
pre=next(ast.literal_eval(n.value) for n in tree.body if isinstance(n,ast.Assign) and any(isinstance(t,ast.Name) and t.id=='pre' for t in n.targets))
pre='#include <vector>\n#include <cstdlib>\n'+pre
pre=pre.replace('static bool WaitForFGQueuesIdle(){return proof;}','static bool WaitForQueueIdle(void*){return proof;}')
pre=pre.replace('static bool WaitForQueueIdle(void*){return proof;}',r'''
static void* currentCommandQueue=(void*)1;static void* producer=(void*)2;static void* failedQueue=nullptr;
static std::vector<void*> waited;
static bool WaitForQueueIdle(void* q){waited.push_back(q);return proof && q && q!=failedQueue;}
''').replace(' void ReleaseSwapchain(void*)',' void* GetCommandQueue(){return producer;}\n void ReleaseSwapchain(void*)')
helper=function(s,'static bool WaitForFGQueuesIdle(') if 'static bool WaitForFGQueuesIdle(' in s else ''
body=function(s,'void FGHooks::ReleaseFGSwapchain(')
resizes=[]
for sig in ('static HRESULT hkResizeBuffers(', 'static HRESULT hkResizeBuffers1('):
    # Locate the function independent of calling convention and pointer type spelling.
    name=sig.split(' ')[-1]
    pos=s.index(name);part=function(s[pos:],name)
    old='if (State::Instance().currentCommandQueue != nullptr && resizeFence != nullptr && resizeFenceEvent != nullptr)'
    if old in part:block=function(part,old)
    else:
        block=function(part,'if (!WaitForFGQueuesIdle())')+'\nReleaseSynthD3D12();'
    resizes.append(block)
pre+='\nusing HRESULT=int;constexpr int DXGI_ERROR_WAS_STILL_DRAWING=-1;[[maybe_unused]]static void* resizeFence=(void*)1;[[maybe_unused]]static void* resizeFenceEvent=(void*)1;\n'
unit=pre+helper+body+'\nint Resize0(){'+resizes[0]+'return 0;}\nint Resize1(){'+resizes[1]+'return 0;}\n'
post=r'''
int main(int argc,char**argv){assert(argc==2);int mode=std::atoi(argv[1]);
 IUnknown chain;FG fg;auto& state=State::Instance();state.currentFG=&fg;state.currentFGSwapchain=&chain;
 state.currentCommandQueue=mode==0?nullptr:(void*)3;proof=true;failedQueue=mode==2?producer:nullptr;
 FGHooks::ReleaseFGSwapchain(&chain,false);
 assert(waited==std::vector<void*>({currentCommandQueue,producer}));
 assert(fg.releases==(mode==2?0:1) && synthReleases==(mode==2?0:1));
 for(auto resize:{Resize0,Resize1}){waited.clear();synthReleases=0;int result=resize();
  assert(waited==std::vector<void*>({currentCommandQueue,producer}));
  assert(synthReleases==(mode==2?0:1));assert(result==(mode==2?DXGI_ERROR_WAS_STILL_DRAWING:0));
 }
}
'''
execute('5-owned-FG-queues',unit+post,('0','1','2'))

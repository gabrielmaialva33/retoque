#!/usr/bin/env python3
"""Actual FG teardown: completion is required and borrowed COM references are never consumed."""
from pathlib import Path
import subprocess
import tempfile
ROOT=Path(__file__).resolve().parents[2]
s=(ROOT/'OptiScaler/hooks/FG_Hooks.cpp').read_text()
def function(sig):
 a=s.index(sig);b=s.index('{',a);d=1;e=b+1
 while d:
  d+=(s[e]=='{')-(s[e]=='}');e+=1
 return s[a:e]
body=function('void FGHooks::ReleaseFGSwapchain(')
pre=r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#define LOG_DEBUG(...) ((void)0)
#define LOG_ERROR(...) ((void)0)
#define LOG_WARN(...) ((void)0)
struct IUnknown { unsigned refs=3; unsigned Release(){return --refs;} };
static bool proof=false; static int synthReleases=0;
static bool WaitForFGQueuesIdle(){return proof;}
static void ReleaseSynthD3D12(){++synthReleases;}
struct FG { bool keep=false; int releases=0; void* context=(void*)1;
 void ReleaseSwapchain(void*){++releases;if(!keep)context=nullptr;}
 void* SwapchainContext(){return context;}
};
struct State { void* currentCommandQueue=nullptr; FG* currentFG=nullptr;
 IUnknown* currentFGSwapchain=nullptr; IUnknown* currentSwapchain=nullptr;
 IUnknown* currentWrappedSwapchain=nullptr; bool SCExclusiveFullscreen=true;
 static State& Instance(){static State s; return s;}
};
struct FGHooks { static inline bool _skipReleaseChecks=false, _borderlessPending=true;
 static inline IUnknown* _releasedFGSwapchain=nullptr;
 static inline void* _hwnd=nullptr;
 static void ReleaseFGSwapchain(IUnknown*,bool);
};
'''
post=r'''
int main(){
 IUnknown chain, wrapper; FG fg; auto& state=State::Instance();
 state.currentFG=&fg;state.currentFGSwapchain=state.currentSwapchain=&chain;state.currentWrappedSwapchain=&wrapper;
 for(bool releaseRefs:{false,true}) {
  proof=false;FGHooks::ReleaseFGSwapchain(&chain,releaseRefs);
  assert(fg.releases==0 && synthReleases==0 && state.currentFGSwapchain==&chain);
  assert(wrapper.refs==3 && chain.refs==3 && !FGHooks::_skipReleaseChecks);
 }
 proof=true;fg.keep=true;FGHooks::ReleaseFGSwapchain(&chain,false);
 assert(fg.releases==1 && synthReleases==1 && state.currentFGSwapchain==&chain);
 assert(!FGHooks::_skipReleaseChecks);
 fg.keep=false;FGHooks::ReleaseFGSwapchain(&chain,true);
 assert(fg.releases==2 && synthReleases==2 && state.currentFGSwapchain==nullptr);
 assert(wrapper.refs==3 && chain.refs==3 && !FGHooks::_skipReleaseChecks);
}
'''
pre='#include <initializer_list>\n'+pre
with tempfile.TemporaryDirectory(prefix='retoque-fg-teardown-') as folder:
 p=Path(folder)
 variants={'baseline':body,
 'no-drain-refusal':body.replace('if (!waitResult)','if (false && !waitResult)'),
 'consume-borrowed-ref':body.replace('    _skipReleaseChecks = true;', '    State::Instance().currentWrappedSwapchain->Release();\n    _skipReleaseChecks = true;')}
 for name,variant in variants.items():
  assert name=='baseline' or variant!=body
  cpp=p/(name+'.cpp');exe=p/name;cpp.write_text(pre+variant+post)
  subprocess.run(['g++','-std=c++20','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(cpp),'-o',str(exe)],check=True)
  result=subprocess.run([str(exe)],capture_output=True)
  assert (result.returncode==0)==(name=='baseline'), (name,result.stderr.decode())
print('PASS: FG teardown refuses unproven completion, retains kept context, preserves borrowed refs; 2 mutants rejected')

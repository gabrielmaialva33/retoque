from support import source,function,execute
s=function(source('retoque/dlssnr/DlssNr_VkPresentHost.cpp'),'std::string TeardownDevice(')
a=s.index('\n',s.index('DlssNr::ShutdownPresentNgx('))+1;b=s.index('    return "freed:',a)
part=s[a:b]
pre=r'''
#include <cassert>
#include <cstdint>
#include <vector>
#include <utility>
using VkSemaphore=unsigned;static int destroyed=0;
struct Fn{void destroySemaphore(void*,unsigned,void*){++destroyed;}void destroyCommandPool(void*,unsigned,void*){++destroyed;}};
struct Device{void* device=nullptr;bool leaked=false;Fn fn;std::vector<unsigned>retained{1,2};std::vector<std::pair<int,unsigned>>pools{{0,1}};};
int main(){for(bool unproven:{false,true}){destroyed=0;Device dev;
'''
post=r'''
(void)retained;assert(destroyed==(unproven?0:3));}}
'''
execute('1-retained-objects-after-drain',pre+part+post)

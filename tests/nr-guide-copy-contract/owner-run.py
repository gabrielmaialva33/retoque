#!/usr/bin/env python3
"""Actual owner admission plus actual pre-recording Dispatch prefix, not a policy fake."""
from pathlib import Path
import subprocess
import tempfile
HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
main = (ROOT/'retoque/shaders/dlssnr/DlssNr_Dx12.cpp').read_text()
internal = (ROOT/'retoque/shaders/dlssnr/DlssNr_Dx12_Internal.h').read_text()
resources = (ROOT/'retoque/shaders/dlssnr/DlssNr_Dx12_Resources.cpp').read_text()
guides = (ROOT/'retoque/shaders/dlssnr/DlssNr_Dx12_PresentGuides.cpp').read_text()
def function(text, anchor):
    start=text.index(anchor); brace=text.index('{', start); depth=0
    for i in range(brace,len(text)):
        depth+=(text[i]=='{')-(text[i]=='}')
        if depth==0:return text[start:i+1]+'\n'
    raise AssertionError(anchor)
unit=(HERE/'fakes.h').read_text()+'\n#include <mutex>\n#include <type_traits>\n'
if 'struct NrDeviceOwner\n' in internal:
    unit+=function(internal,'struct NrDeviceOwner\n')+';\nNrDeviceOwner g_nrDeviceOwner;\n'
for overload in ('bool DeviceChildBelongsTo(ID3D12Device* device, ID3D12DeviceChild* child)',
                 'bool DeviceChildBelongsTo(ID3D12Device* device, ID3D12Resource* resource)'):
    if overload in resources:
        unit+=function(resources,overload)
if 'bool PresentGuideCopyCompatible(' in resources:
    unit+=function(resources,'bool PresentGuideCopyCompatible(')
unit+='''
std::mutex g_nrMutex;
struct Config { static Config* Instance() { static Config c; return &c; } };
struct { bool failed=false; } g_nr;
struct DlssNrFrameInfo { bool GameGuides=false; unsigned RenderSubrectWidth=1, RenderSubrectHeight=1; void* ExposureTexture=nullptr; };
namespace DlssNr { namespace Detail { struct CoverageSample { const char* reason=""; }; }
namespace Chain { struct RecordingLease {}; } namespace PairCapture { void Poll() {} } }
struct State { unsigned frameCount=1; static State& Instance() { static State s; return s; } };
void ReportSkipOnce(const char*) {} void AdvanceNrClock() {} void NoteModelFault() {} void NotePulsePresent(unsigned) {}
bool PausedWithoutFrameGen(const Config&) { return false; }
struct ID3D12CommandQueue : ID3D12DeviceChild {};
struct DlssNr_Dx12 {
    ID3D12Device* _device;
    DlssNr_Dx12(const char*,ID3D12Device* d):_device(d) {}
    bool Dispatch(ID3D12GraphicsCommandList*,ID3D12Resource*,ID3D12Resource*,ID3D12Resource*,ID3D12Resource*,
                  const DlssNrFrameInfo&,ID3D12CommandQueue*,DlssNr::Detail::CoverageSample*,DlssNr::Chain::RecordingLease*);
};
std::unique_ptr<DlssNr_Dx12> g_compose;
'''
anchor='static bool EnsureCompose(' if 'static bool EnsureCompose(' in main else 'static void EnsureCompose('
unit+=function(main,anchor)
dispatch=main[main.index('bool DlssNr_Dx12::Dispatch('):main.index('    g_activePasses = 0;',main.index('bool DlssNr_Dx12::Dispatch('))]
unit+=dispatch+'    return true;\n}\n'
unit+='struct GuideCapture { ID3D12Resource* depth=nullptr;ID3D12Resource* motion=nullptr; };\n'
capture=guides[guides.index('static bool CaptureGuides('):guides.index('    struct AllocFailKey',guides.index('static bool CaptureGuides('))]
unit+=capture+'    device->Release();return true;\n}\n'
unit+='struct { unsigned invalidations=0; void InvalidateSlot(size_t slot) { assert(slot==2);++invalidations; } } g_presentGuideRing;\n'
unit+='bool RejectUnusableClone() { ID3D12Resource d,m; struct {ID3D12Resource* depth;ID3D12Resource* motion;} clone{&d,&m}; bool copyCompatible=false;size_t slot=2; (void)clone;(void)copyCompatible;(void)slot;\n'
post_anchor='    if (clone.depth != nullptr && clone.motion != nullptr && !copyCompatible)'
assert post_anchor in guides, 'clone rejection guard missing'
begin=guides.index(post_anchor)
unit+=guides[begin:guides.index('    if (clone.depth == nullptr || clone.motion == nullptr)',begin)]
unit+='    return true; }\n'

unit+='''
template<class F> bool Compose(F fn,ID3D12Device* d) {
    if constexpr(std::is_same_v<std::invoke_result_t<F,ID3D12Device*>,bool>) return fn(d);
    else { fn(d);return true; }
}
int main(int argc,char**argv) {
    assert(argc==2); const std::string which=argv[1];
    ID3D12Device a,b,wrapper;
    wrapper.luid=a.luid;wrapper.wraps=&a;
    ID3D12GraphicsCommandList cmd;cmd.device=&a;
    ID3D12Resource color,depth,motion,output,exposure;
    for(auto* r:{&color,&depth,&motion,&output,&exposure})r->device=&a;
    DlssNrFrameInfo frame;
    if(which=="capture-postallocation") {
        assert(!RejectUnusableClone());assert(g_presentGuideRing.invalidations==1);
    } else if(which.starts_with("capture-")) {
        if(which=="capture-cmd")cmd.device=&b;
        else if(which=="capture-motion")motion.device=&b;
        else if(which=="capture-planes")a.failFormatInfo=true;
        else if(which=="capture-quality")depth.desc.SampleDesc.Quality=1;
        else if(which=="capture-mips")depth.desc.MipLevels=0;
        else if(which=="capture-two-planes")depth.desc.Format=DXGI_FORMAT_R24G8_TYPELESS;
        else assert(which=="capture-valid");
        assert(CaptureGuides(&cmd,GuideCapture{&depth,&motion},frame)==
               (which=="capture-valid" || which=="capture-two-planes"));
    } else if(which=="retain") {
        assert(Compose(EnsureCompose,&a));
        assert(a.refs==2 && "first device needs an explicit retained identity");
        assert(Compose(EnsureCompose,&a));assert(a.refs==2);
    } else if(which=="refuse-second") {
        assert(Compose(EnsureCompose,&a));auto* original=g_compose.get();
        assert(!Compose(EnsureCompose,&b));assert(g_compose.get()==original);
        assert(g_compose->_device==&a && b.refs==1);
    } else if(which=="dispatch-wrapped" || which=="dispatch-wrapped-foreign") {
        // A wrapper layer's list: the pass lives on the list's device, the game's resources report the one beneath.
        DlssNr_Dx12 pass("test",&wrapper);cmd.device=&wrapper;
        if(which=="dispatch-wrapped-foreign")depth.device=&b;
        const bool accepted=pass.Dispatch(&cmd,&color,&depth,&motion,&output,frame,nullptr,nullptr,nullptr);
        assert(accepted==(which=="dispatch-wrapped"));assert(cmd.copies==0 && cmd.barriers==0);
        if(accepted)assert(g_nrDeviceOwner.device==&wrapper);
    } else if(which=="dispatch-wrapped-list") {
        // The list is held to the pass's exact device: another interface on the adapter is not enough for a list.
        DlssNr_Dx12 pass("test",&a);cmd.device=&wrapper;
        assert(!pass.Dispatch(&cmd,&color,&depth,&motion,&output,frame,nullptr,nullptr,nullptr));
    } else {
        DlssNr_Dx12 pass("test",&a);
        if(which=="dispatch-cmd")cmd.device=&b;
        else if(which=="dispatch-depth")depth.device=&b;
        else if(which=="dispatch-motion")motion.device=&b;
        else if(which=="dispatch-color")color.device=&b;
        else if(which=="dispatch-output")output.device=&b;
        else if(which=="dispatch-exposure") { exposure.device=&b;frame.ExposureTexture=&exposure; }
        else if(which=="dispatch-owner")assert(Compose(EnsureCompose,&b));
        else assert(which=="dispatch-valid");
        const bool accepted=pass.Dispatch(&cmd,&color,&depth,&motion,&output,frame,nullptr,nullptr,nullptr);
        assert(accepted==(which=="dispatch-valid"));assert(cmd.copies==0 && cmd.barriers==0);
    }
    g_compose.reset();
'''
if 'struct NrDeviceOwner\n' in internal:unit+='    g_nrDeviceOwner.Reset();\n'
unit+='    assert(a.refs==1 && b.refs==1 && wrapper.refs==1);\n}\n'
with tempfile.TemporaryDirectory(prefix='retoque-device-owner-') as temp:
    cpp=Path(temp)/'owner.cpp';binary=Path(temp)/'owner';cpp.write_text(unit)
    subprocess.run(['g++','-std=c++20','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-parameter',
                    '-fsanitize=address,undefined','-fno-omit-frame-pointer','-I'+str(ROOT),str(cpp),'-o',str(binary)],check=True)
    failures=[]
    for name in ('retain','refuse-second','dispatch-cmd','dispatch-depth','dispatch-motion','dispatch-color',
                 'dispatch-output','dispatch-exposure','dispatch-owner','dispatch-valid',
                 'dispatch-wrapped','dispatch-wrapped-foreign','dispatch-wrapped-list',
                 'capture-cmd','capture-motion','capture-planes','capture-quality','capture-mips',
                 'capture-two-planes','capture-valid','capture-postallocation'):
        r=subprocess.run([str(binary),name])
        if r.returncode:failures.append(name)
        else:print('PASS:',name,flush=True)
    if failures:raise SystemExit('FAIL: '+', '.join(failures))

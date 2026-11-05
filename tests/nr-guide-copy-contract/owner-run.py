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
unit=(HERE/'fakes.h').read_text()+'\n#include <mutex>\n#include <type_traits>\n#include "retoque/dlssnr/DlssNr_Pictures.h"\n#include "retoque/dlssnr/DlssNr_PresentGuideIdentity.h"\n'
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
struct { bool failed=false; bool reset=false; } g_nr;
struct DlssNrFrameInfo { bool GameGuides=false; unsigned RenderSubrectWidth=1, RenderSubrectHeight=1; void* ExposureTexture=nullptr; uint64_t PictureEpoch=0; };
// The main-picture epoch gate (DlssNr_Pictures.h; tests/nr-main-picture covers it): no epoch here, so never Restart.
DlssNr::Pictures::EpochGate g_pictureEpoch; void InvalidateExposureMeter() {}
namespace DlssNr { namespace Detail { struct CoverageSample { const char* reason=""; }; }
namespace Chain { struct RecordingLease {}; } namespace PairCapture { void Poll() {} } }
struct State { unsigned frameCount=1; static State& Instance() { static State s; return s; } };
void ReportSkipOnce(const char*) {} void AdvanceNrClock() {} void NoteModelFault() {} void NotePulsePresent(unsigned) {}
// The game's own Streamline NR stand-down (DlssNr_SlNativeNr.h; tests/nr-sl-native-nr covers it): never here.
static const char* GameNrStandDown() { return nullptr; }
std::atomic<bool> g_resetOnReturn { false };
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
unit+='DlssNr::PresentGuideIdentity::ProducerReason g_lastSkipReason = DlssNr::PresentGuideIdentity::ProducerReason::Recorded;\n'
unit+='static uint64_t ResidentGuideBytes() { return 0; }\n'
unit+='static uint64_t ParkedGuideBytes() { return 0; }\n'
unit+='static void NoteGuideCapture(DlssNr::PresentGuideIdentity::ProducerReason reason, const DlssNr::PresentGuides::AcquireReport&, uint64_t, uint64_t) { g_lastSkipReason = reason; }\n'
unit+='static void NoteCaptureSkipUnlocked(DlssNr::PresentGuideIdentity::ProducerReason reason) { g_lastSkipReason = reason; }\n'
capture=guides[guides.index('static bool CaptureGuides('):guides.index('    struct AllocFailKey',guides.index('static bool CaptureGuides('))]
unit+=capture+'    device->Release();return true;\n}\n'
unit+='struct { unsigned invalidations=0; void InvalidateSlot(size_t slot) { assert(slot==2);++invalidations; } } g_presentGuideRing;\n'
unit+='bool RejectUnusableClone(bool replaceDepth = false) { ID3D12Device dev; ID3D12Device* device=&dev; device->AddRef(); ID3D12Resource d,m; ID3D12Resource* candDepth=&d; ID3D12Resource* candMotion=&m; bool copyCompatible=false; bool replaceMotion=false; size_t slot=2; DlssNr::PresentGuides::AcquireReport acquire{}; (void)candDepth;(void)candMotion;(void)copyCompatible;(void)replaceDepth;(void)replaceMotion;(void)slot;(void)acquire;\n'
post_anchor='    if (candDepth != nullptr && candMotion != nullptr && !copyCompatible)'
assert post_anchor in guides, 'clone rejection guard missing'
begin=guides.index(post_anchor)
unit+=guides[begin:guides.index('    if (candDepth == nullptr || candMotion == nullptr)',begin)]
unit+='    return true; }\n'
unit+=function(guides,'static ID3D12Resource* CreatePresentGuideClone(')
unit+='''
namespace Microsoft::WRL {
template <typename T> class ComPtr {
    T* p_ = nullptr;
public:
    ComPtr() = default;
    ~ComPtr() { delete p_; }
    void Attach(T* p) { delete p_; p_ = p; }
    T* Get() const { return p_; }
    T* Detach() { T* out = p_; p_ = nullptr; return out; }
};
}
namespace DlssNr::Submission {
struct Usage { int tag = 0; };
static bool g_trackOk = true;
inline bool Track(ID3D12GraphicsCommandList*, Usage& u) {
    if (!g_trackOk) return false;
    u.tag += 10;
    return true;
}
}
static bool g_retireCapOk = true;
static bool PresentGuideRetirementCapacity() { return g_retireCapOk; }
static uint64_t PresentGuideCloneBytes(ID3D12Device*, ID3D12Resource* res) {
    return res ? res->desc.Width * res->desc.Height * 4 : 0;
}
static unsigned g_parkedClones = 0;
static int g_lastParkedWriteTag = -1;
static void ParkNrClone(ID3D12Resource*& res, const DlssNr::Submission::Usage& u, bool) {
    if (res) {
        ++g_parkedClones;
        g_lastParkedWriteTag = u.tag;
        retired.emplace_back(res);
        res = nullptr;
    }
}
struct FakeSlotClone {
    ID3D12Resource* depth = nullptr;
    ID3D12Resource* motion = nullptr;
    const void* deviceIdentity = nullptr;
    DlssNr::Submission::Usage writeUsage {};
    DlssNr::Submission::Usage captureWriteUsage {};
    bool tracked = false;
    uint64_t depthBytes = 0;
    uint64_t motionBytes = 0;
    DXGI_FORMAT sourceDepthFormat = DXGI_FORMAT_UNKNOWN;
    uint64_t sourceDepthWidth = 0;
    uint32_t sourceDepthHeight = 0;
    unsigned sourceDepthFlags = 0;
    DXGI_FORMAT sourceMotionFormat = DXGI_FORMAT_UNKNOWN;
    uint64_t sourceMotionWidth = 0;
    uint32_t sourceMotionHeight = 0;
    unsigned sourceMotionFlags = 0;
};
bool RunCaptureReplaceStep(FakeSlotClone& clone, ID3D12Device* device, ID3D12GraphicsCommandList* cmdList,
                           ID3D12Resource* depth, ID3D12Resource* motion) {
    device->AddRef();
    const void* const devId = static_cast<const void*>(device);
    const auto depthDesc = depth->GetDesc();
    const auto motionDesc = motion->GetDesc();
    size_t slot = 2;
    DlssNr::PresentGuides::AcquireReport acquire {};
    struct AllocFailKey {
        const void* device;
        DXGI_FORMAT format;
        uint64_t width;
        uint32_t height;
        auto operator<=>(const AllocFailKey&) const = default;
    };
    std::set<AllocFailKey> s_failedAllocs;
'''
replace_start = guides.index('    const bool replaceDepth =')
replace_end = guides.index('    // With GpuTiming off nothing below reads the timing settings', replace_start)
unit += guides[replace_start:replace_end] + '    return true;\n}\n'

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
        assert(!RejectUnusableClone(false));assert(g_presentGuideRing.invalidations==1);
        assert(g_lastSkipReason==DlssNr::PresentGuideIdentity::ProducerReason::Incompatible);
        assert(!RejectUnusableClone(true));assert(g_presentGuideRing.invalidations==1);
    } else if(which=="capture-prepark-order") {
        FakeSlotClone clone;
        assert(RunCaptureReplaceStep(clone, &a, &cmd, &depth, &motion));
        ID3D12Resource* oldDepth = clone.depth;
        ID3D12Resource* oldMotion = clone.motion;
        assert(oldDepth != nullptr && oldMotion != nullptr && g_parkedClones == 0);
        const int oldWriteTag = clone.writeUsage.tag;
        // Now change source resolution to trigger replaceDepth + replaceMotion:
        depth.desc.Width = motion.desc.Width = 1280;
        depth.desc.Height = motion.desc.Height = 720;
        // 1. Retirement cap full -> refused BEFORE allocation or ParkNrClone.
        g_retireCapOk = false;
        const unsigned allocsBeforeCap = allocations;
        assert(!RunCaptureReplaceStep(clone, &a, &cmd, &depth, &motion));
        assert(g_lastSkipReason == DlssNr::PresentGuideIdentity::ProducerReason::RetiredCap);
        assert(allocations == allocsBeforeCap && g_parkedClones == 0);
        assert(clone.depth == oldDepth && clone.motion == oldMotion && clone.sourceDepthWidth == 640);
        g_retireCapOk = true;
        // 2. Motion allocation fails -> temporary depth freed, old clones NOT parked.
        motion.desc.Format = DXGI_FORMAT_R16G16_FLOAT;
        a.failedFormats.insert(DXGI_FORMAT_R16G16_FLOAT);
        assert(!RunCaptureReplaceStep(clone, &a, &cmd, &depth, &motion));
        assert(g_lastSkipReason == DlssNr::PresentGuideIdentity::ProducerReason::AllocFailed);
        assert(g_parkedClones == 0 && clone.depth == oldDepth && clone.motion == oldMotion);
        a.failedFormats.clear();
        // 3. Submission::Track refuses -> temporary depth+motion freed, writeUsage restored, old clones NOT parked.
        DlssNr::Submission::g_trackOk = false;
        assert(!RunCaptureReplaceStep(clone, &a, &cmd, &depth, &motion));
        assert(g_lastSkipReason == DlssNr::PresentGuideIdentity::ProducerReason::TrackRefused);
        assert(g_parkedClones == 0 && clone.depth == oldDepth && clone.motion == oldMotion);
        assert(clone.writeUsage.tag == oldWriteTag);
        DlssNr::Submission::g_trackOk = true;
        // 4. All checks pass -> old clones parked with savedWriteUsage, new clones attached.
        assert(RunCaptureReplaceStep(clone, &a, &cmd, &depth, &motion));
        assert(g_parkedClones == 2 && g_lastParkedWriteTag == oldWriteTag);
        assert(clone.depth != oldDepth && clone.motion != oldMotion && clone.sourceDepthWidth == 1280);
        assert(clone.depthBytes == 1280ull * 720 * 4 && clone.motionBytes == 1280ull * 720 * 4);
        ParkNrClone(clone.depth, clone.writeUsage, clone.tracked);
        ParkNrClone(clone.motion, clone.writeUsage, clone.tracked);
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
                 'capture-two-planes','capture-valid','capture-postallocation','capture-prepark-order'):
        r=subprocess.run([str(binary),name])
        if r.returncode:failures.append(name)
        else:print('PASS:',name,flush=True)
    if failures:raise SystemExit('FAIL: '+', '.join(failures))

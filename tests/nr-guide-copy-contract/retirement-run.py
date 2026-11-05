#!/usr/bin/env python3
"""Production collector and capture admission prefixes: diagnostics must not advance lifetime.

D3D interfaces are host fakes; all retirement/skip/capacity code is extracted verbatim.
The prefix stops before allocation/recording, not before the replacement-capacity decision.
"""
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
GUIDES = (ROOT / 'retoque/shaders/dlssnr/DlssNr_Dx12_PresentGuides.cpp').read_text()
LIFETIME = (ROOT / 'retoque/shaders/dlssnr/DlssNr_Dx12_Lifetime.cpp').read_text()
INTERNAL = (ROOT / 'retoque/shaders/dlssnr/DlssNr_Dx12_Internal.h').read_text()
RESOURCES = (ROOT / 'retoque/shaders/dlssnr/DlssNr_Dx12_Resources.cpp').read_text()
CHAIN = (ROOT / 'retoque/dlssnr/DlssNr_Chain.h').read_text()


def function(text, anchor):
    start = text.index(anchor)
    brace = text.index('{', start)
    depth = 0
    for i in range(brace, len(text)):
        depth += (text[i] == '{') - (text[i] == '}')
        if depth == 0:
            return text[start:i + 1] + '\n'
    raise AssertionError(anchor)


def unit(guides):
    text = '#include "pch.h"\n' + (HERE / 'fakes.h').read_text()
    text += '''
#include <mutex>
#include "retoque/dlssnr/DlssNr_PresentGuideIdentity.h"
#include "retoque/dlssnr/DlssNr_SubmissionModel.h"
namespace DlssNr::Submission {
using Usage = Detail::Usage;
uint64_t Clock() { return 0; }
bool Ready(const Usage&) { return false; }
bool Settled(const Usage&) { return false; }
bool OnlyUnanswerableLeft(const Usage&) { return false; }
bool Completed(const Usage&) { return false; }
bool CaptureReusable(const Usage& usage) {
    return Detail::IsCaptureReusable(usage, [](size_t) { return uint64_t{0}; });
}
}
struct OS_Dx12 {}; struct DlssNr_Stabilizer_Dx12 {}; struct DlssNr_UiMask_Dx12 {};
struct DlssNr_Cadence_Dx12 {}; struct NrEnlarger {}; struct DlssNr_DetailStats_Dx12 {};
struct DlssNr_Pulse_Dx12 {}; struct DlssNr_MotionProbe_Dx12 {};
'''
    text += function(INTERNAL, 'struct NrRetired\n') + ';\n'
    text += '''
std::vector<NrRetired> g_nrRetired;
std::mutex g_nrMutex;
struct { bool failed=false; void (*release)(void*)=nullptr; } g_nr;
void TickNrRetired(bool shutdown=false);
'''
    text += function(LIFETIME, 'void TickNrRetired(bool shutdown)')
    text += 'namespace DlssNr::Chain {\n' + function(CHAIN, 'inline bool RetirementAllowed(') + '}\n'
    for anchor in ('bool DeviceChildBelongsTo(ID3D12Device* device, ID3D12DeviceChild* child)',
                   'bool DeviceChildBelongsTo(ID3D12Device* device, ID3D12Resource* resource)',
                   'bool PresentGuideCopyCompatible('):
        text += function(RESOURCES, anchor)
    text += '''
using Reason = DlssNr::PresentGuideIdentity::ProducerReason;
Reason lastReason = Reason::Recorded;
void NoteGuideCapture(Reason reason, const DlssNr::PresentGuides::AcquireReport&, uint64_t, uint64_t) {
    lastReason = reason;
}
uint64_t ResidentGuideBytes() { return 0; }
uint64_t ParkedGuideBytes() { return 0; }
struct GuideCapture { ID3D12Resource* depth=nullptr; ID3D12Resource* motion=nullptr; };
struct DlssNrFrameInfo {};
struct PresentGuideClone {
    ID3D12Resource* depth=nullptr; ID3D12Resource* motion=nullptr;
    const void* deviceIdentity=nullptr;
    DXGI_FORMAT sourceDepthFormat=0, sourceMotionFormat=0;
    uint64_t sourceDepthWidth=0, sourceMotionWidth=0;
    uint32_t sourceDepthHeight=0, sourceMotionHeight=0;
    unsigned sourceDepthFlags=0, sourceMotionFlags=0;
    bool readTracked=false;
    DlssNr::Submission::Usage presentReadUsage;
};
std::array<PresentGuideClone, DlssNr::PresentGuides::kRingSize> g_presentGuideClones {};
DlssNr::PresentGuides::Ring g_presentGuideRing;
struct { struct { void SetBytes(uint64_t, uint64_t) {} } producer; } g_guideIdentity;
'''
    text += function(guides, 'static bool PresentGuideRetirementCapacity(')
    text += function(guides, 'static void NoteCaptureSkipUnlocked(')
    capture = function(guides, 'static bool CaptureGuides(')
    text += capture[:capture.index('    Microsoft::WRL::ComPtr<ID3D12Resource> newDepth;')]
    text += '    (void)needDepth; (void)needMotion; device->Release(); return true;\n}\n'
    text += '''
struct NVSDK_NGX_Parameter { ID3D12Resource* depth=nullptr; ID3D12Resource* motion=nullptr; };
constexpr int NVSDK_NGX_Parameter_Depth=0, NVSDK_NGX_Parameter_MotionVectors=1;
ID3D12Resource* GetResource(NVSDK_NGX_Parameter* p, int which, const char*) {
    return p ? (which == 0 ? p->depth : p->motion) : nullptr;
}
'''
    temporal = function(guides, 'void CaptureTemporal(')
    text += temporal[:temporal.index('    const auto& cfg')] + '}\n'
    bridge = function(guides, 'bool CaptureBridgedGuides(')
    text += bridge[:bridge.index('    GuideCapture source;')] + '    return true;\n}\n'
    text += (HERE / 'retirement-cases.h').read_text()
    return text


with tempfile.TemporaryDirectory(prefix='nr-guide-retirement-') as folder:
    folder = Path(folder)
    (folder / 'pch.h').write_text('// Host platform supplied by fakes.h.\n')
    variants = [('baseline', GUIDES)]
    for name, anchor in (
        ('tick-on-skip', '    NoteGuideCapture(reason, {}, ResidentGuideBytes(), ParkedGuideBytes());'),
        ('second-capacity-tick', 'static bool PresentGuideRetirementCapacity()\n{'),
    ):
        assert GUIDES.count(anchor) == 1
        replacement = ('    TickNrRetired();\n' + anchor if name == 'tick-on-skip'
                       else anchor + '\n    TickNrRetired();')
        variants.append((name, GUIDES.replace(anchor, replacement, 1)))
    for name, guides in variants:
        cpp, binary = folder / (name + '.cpp'), folder / name
        cpp.write_text(unit(guides))
        flags = ['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                 '-Wno-unused-parameter', '-I' + str(ROOT)]
        if name == 'baseline':
            flags += ['-fsanitize=address,undefined', '-fno-omit-frame-pointer']
        subprocess.run(flags + [str(cpp), '-o', str(binary)], check=True)
        result = subprocess.run([str(binary)], capture_output=True, text=True)
        if name == 'baseline':
            assert result.returncode == 0, result.stderr
        else:
            assert result.returncode != 0, f'mutant survived: {name}'
            assert 'gpuOutstanding' in result.stderr, result.stderr
        print(f'PASS: retirement {name}: compiled; ' + ('cases passed' if name == 'baseline' else 'release assertion killed mutant'))

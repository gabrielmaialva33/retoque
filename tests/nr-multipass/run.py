#!/usr/bin/env python3
"""Run portable production helpers; no GPU/NGX execution is claimed."""
from pathlib import Path
import subprocess
import tempfile
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / '_support'))
from nr_sources import renderer_source  # noqa: E402  every DlssNr_Dx12*.cpp unit, in a fixed order
root = Path(__file__).resolve().parent
with tempfile.TemporaryDirectory(prefix='optiscaler-nr-chain-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['g++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer', '-g', str(root / 'test.cpp'), '-o', binary], check=True)
    subprocess.run([binary], check=True)

source = renderer_source()
dispatch = source[source.index('bool DlssNr_Dx12::Dispatch('):source.index('namespace DlssNr', source.index('bool DlssNr_Dx12::Dispatch('))]
assert dispatch.count('g_nr.superDown->Dispatch(cmdList, finalOutput, g_nr.outputNative)') == 1
assert 'DlssNr::Chain::Routing<ID3D12Resource*> chain(modelInput, g_nr.output, g_nr.passPing)' in dispatch
assert 'DlssNr::Chain::Resolve(g_nr.colorCopy, modelInput, g_nr.outputNative, finalOutput, superDownOk)' in dispatch
print('PASS: renderer uses tested routing/resolve; one supersampling down-leg after finalOutput selection')

# Rebuild headroom (docs/dlssnr/design/chain-in-flight.md): a refused generation first parks the one it replaces
# and waits; only a refusal with nothing left to free latches the session failure.
refusal = source[source.index('!DlssNr::Chain::Admit(budget.Budget, budget.CurrentUsage'):]
refusal = refusal[:refusal.index('g_nr.reason = "chain generation refused')]
assert refusal.index('if (MakeRoomForGeneration())') < refusal.index('g_nr.failed = true;')
assert 'return false;' in refusal[refusal.index('if (MakeRoomForGeneration())'):refusal.index('g_nr.failed = true;')]
room = source[source.index('bool MakeRoomForGeneration()\n{'):source.index('void ParkAltSurfaces()')]
for name in ('g_nr.feature', 'g_nr.passFeature', 'g_nr.output', 'g_nr.passPing', 'g_nr.colorCopy', 'g_nr.hdrCopy',
             'g_nr.colorSmall', 'g_nr.outputNative', 'g_nr.depthMatched', 'g_nr.motionMatched'):
    assert name in room, name
assert '!r.stuck' in room
print('PASS: a refused chain generation parks the current one and waits; latches only with nothing left to free')

# Ten passes (bounded-chain.md, "Ten passes"): every per-pass array is sized by the setting's bound, every loop over
# them stops at that bound or at the requested count, and nothing still assumes three.
internal = (Path(__file__).resolve().parents[2] / 'retoque/shaders/dlssnr/DlssNr_Dx12_Internal.h').read_text()
state = internal[internal.index('struct NrState'):internal.index('extern NrState g_nr;')]
assert 'static constexpr uint32_t kMaxPasses = DlssNr::PassConfig::MaxPasses;' in state
for field in ('void* passFeature[kMaxPasses]', 'DlssNrResolvedPassSettings passBuilt[kMaxPasses]',
              'std::array<bool, kMaxPasses> passReset', 'bool passFailed[kMaxPasses]',
              'DlssNr::Submission::Usage creation[kMaxPasses]', 'uint64_t creationFrame[kMaxPasses]'):
    assert field in state, field
assert 'all.fill(true);' in state[state.index('passReset'):state.index('bool passFailed')]
import re  # noqa: E402
assert not re.search(r'pass\w*\[3\]', state)
loops = re.findall(r'for \(unsigned (?:i|downstream) = [^;]+; ([^;]+);', source)
assert len(loops) >= 8, loops  # the pass loops of Dispatch and the timing contract, not an empty match
for loop in loops:
    assert '< 3' not in loop and '<= 3' not in loop, loop
    assert 'kMaxPasses' in loop or 'Count' in loop, loop
timing = (Path(__file__).resolve().parents[2] / 'retoque/dlssnr/DlssNr_GpuTimingModel.h').read_text()
assert 'constexpr size_t MaxModels = PassConfig::MaxPasses;' in timing
print('PASS: per-pass state, loops and GPU timing pairs follow the ten-pass bound')

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

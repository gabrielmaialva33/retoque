#!/usr/bin/env python3
"""Neural-producer markers, and the hand-over contract: config round trip and call order in both loaders."""
import pathlib
import re
import subprocess
import tempfile

root = pathlib.Path(__file__).resolve().parents[2]

with tempfile.TemporaryDirectory(prefix='nr-consumers-') as temp:
    binary = pathlib.Path(temp) / 'test'
    subprocess.run(['g++', '-std=c++20', '-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    '-Wall', '-Wextra', '-Werror', '-I' + str(root / 'retoque/dlssnr'),
                    str(root / 'tests/nr-consumers/cases.cpp'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

# Four config points for [DlssNr] YieldToAddons, default true.
config_h = (root / 'OptiScaler/Config.h').read_text(encoding='utf-8-sig')
config_cpp = (root / 'OptiScaler/Config.cpp').read_text()
assert 'CustomOptional<bool> DlssNrYieldToAddons { true };' in config_h
assert 'readBool("DlssNr", "YieldToAddons")' in config_cpp[:config_cpp.index('bool Config::SaveIni()')]
assert 'ini.SetValue("DlssNr", "YieldToAddons"' in config_cpp
for shipped in ('OptiScaler.ini', 'Config.md'):
    text = (root / shipped).read_text(encoding='utf-8-sig')
    assert re.search(r'Default \(auto\) is true\r?\nYieldToAddons=auto', text), shipped

# The hand-over runs before anything of the model is loaded, on both routes.
def before(path, first, then):
    text = (root / path).read_text()
    a, b = text.find(first), text.find(then)
    assert a != -1 and b != -1 and a < b, f'{path}: {first!r} must come before {then!r}'

before('retoque/shaders/dlssnr/DlssNr_Dx12_Model.cpp', 'Consumers::HandOverIfCompeting()', 'return EnsureDirectRuntime();')
before('retoque/shaders/dlssnr/DlssNr_Dx12_Model.cpp', 'Consumers::HandOverIfCompeting()', 'g_nr.forwarder = LoadLibraryW(')
before('retoque/dlssnr/DlssNrFeature_Vk.cpp', 'Consumers::HandOverIfCompeting()', 'g_fwd.module = LoadLibraryW(')

# On D3D12 the hand-over also comes first at both route entries, ahead of every gate of ours that could decline with
# another reason (a ReShade-wrapped command list used to be refused as "unsupported-runtime-or-device" before it).
dx12 = (root / 'retoque/shaders/dlssnr/DlssNr_Dx12.cpp').read_text()
def entry(signature, end_marker):
    start = dx12.index(signature)
    return dx12[start:dx12.index(end_marker, start)]
evaluate = entry('void EvaluateAfterUpscale(', 'Funnel::Evaluation funnel(')
assert 'Consumers::HandOverIfCompeting()' in evaluate, 'EvaluateAfterUpscale hands over before its funnel'
present = entry('void RunPresentPass(', 'g_resetOnReturn = true;')
assert 'Consumers::HandOverIfCompeting()' in present, 'RunPresentPass hands over at its Enabled gate'

# ReShade 6.7+ proxies are unwrapped for submission tracking, like Streamline's.
restrack = (root / 'OptiScaler/resource_tracking/ResTrack_dx12.cpp').read_text()
real = restrack[restrack.index('IUnknown* ResTrack_Dx12::NrRealObject('):]
real = real[:real.index('\n}\n') + 3]
assert 'Util::GetReShadeOriginal(object)' in real and 'original->Release();' in real

# Standing down is the toggle key's mechanism: volatile, the ini is not rewritten.
consumers = (root / 'retoque/dlssnr/DlssNr_Consumers.cpp').read_text()
assert 'DlssNrEnabled.set_volatile_value(false)' in consumers
assert 'DlssNrYieldToAddons.value_or_default()' in consumers
print('PASS: YieldToAddons round-trips, the hand-over precedes every model load and both D3D12 route gates, '
      'and ReShade proxies are unwrapped for submission tracking')

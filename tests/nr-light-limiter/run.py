#!/usr/bin/env python3
"""The light limiter at any cadence (docs/dlssnr/design/light-limiter.md): the production gate and the production
per-pixel rules the shader runs, compiled under ASan/UBSan with a CPU model of the three regional dispatches; mutants of
the key lines (each must fail the cases); and source guards on the key, the surfaces, the renderer's call and the menu
control. No GPU and no shader execution: the model is the shader's decisions over arrays, and the bytecode is checked
for freshness only by tests/nr-invariants and by whoever edits the HLSL."""
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / '_support'))
from nr_sources import renderer_source  # noqa: E402

here = Path(__file__).resolve().parent
root = here.parents[1]
retoque = root / 'retoque'
rule_path = retoque / 'shaders/dlssnr/precompile/dlssnr_cadence_rule.h'
gate_path = retoque / 'dlssnr/DlssNr_LightLimit.h'
FLAGS = ['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-variable',
         '-fsanitize=address,undefined', '-fno-omit-frame-pointer']
MUTANT_FLAGS = [f for f in FLAGS if f not in ('-Werror', '-Wall', '-Wextra')] + ['-w']


def build_and_run(include_dir, flags):
    binary = Path(include_dir) / 'cases'
    subprocess.run(flags + ['-I', str(include_dir), '-I', str(retoque), str(here / 'cases.cpp'), '-o', str(binary)],
                   check=True)
    return subprocess.run([str(binary)], capture_output=True, text=True)


with tempfile.TemporaryDirectory(prefix='optiscaler-nr-light-limiter-') as directory:
    clean = build_and_run(directory, FLAGS)
    sys.stdout.write(clean.stdout)
    assert clean.returncode == 0, clean.stderr

    rule = rule_path.read_text()
    gate = gate_path.read_text()
    mutants = {
        # (file, old, new)
        'region: no gate on the tile\'s own light': ('rule', 'if (abs(input - prevInput) >= inputTol)\n        return r;\n    if (isfinite(motion)',
                                                     'if (isfinite(motion)'),
        'region: no motion release': ('rule', 'if (isfinite(motion) && motion >= motionRelease)\n        return r;', ''),
        'region: no release band': ('rule', 'const float weight = 1.0f - smoothstep(releaseLow, releaseHigh, abs(raw - prevShown));',
                                    'const float weight = 1.0f;'),
        'region: held outright (weight 0)': ('rule', 'const float weight = 1.0f - smoothstep(releaseLow, releaseHigh, abs(raw - prevShown));',
                                             'const float weight = 0.0f;'),
        'region: step ignored': ('rule', 'const float held = clamp(raw, prevShown - step, prevShown + step);\n    const float weight',
                                 'const float held = prevShown;\n    const float weight'),
        'region: gain unclamped': ('rule', 'r.gainStops = clamp((held - raw) * weight, -maxGain, maxGain);',
                                   'r.gainStops = (held - raw) * weight;'),
        'region: invalid state held to': ('rule', 'if (valid == 0 || !isfinite(raw) || !isfinite(input) || !isfinite(prevShown) || !isfinite(prevInput))\n        return r;\n    if (abs(input - prevInput) >= inputTol)\n        return r;\n    if (isfinite(motion)',
                                          'if (!isfinite(raw) || !isfinite(input) || !isfinite(prevShown) || !isfinite(prevInput))\n        return r;\n    if (abs(input - prevInput) >= inputTol)\n        return r;\n    if (isfinite(motion)'),
        'gate: second call in a present limited': ('gate', 'if (_seen && present != 0 && present == _lastPresent)', 'if (false)'),
        'gate: no reset after a frame it did not limit': ('gate', '!_limited || gap', 'gap'),
        'gate: no reset on a gap': ('gate', '!_limited || gap ||', '!_limited ||'),
        'gate: no reset on another mode': ('gate', ' || mode != _lastMode', ''),
        'gate: no reset on a changed signature': ('gate', ' || signature != _signature', ''),
        'gate: no reset on the model\'s reset': ('gate', ' || resetPending;', ';'),
        'gate: runs under the cadence limiter': ('gate', 'if (mode == Mode::Off || otherLimiter)', 'if (mode == Mode::Off)'),
        'gate: a typo is a mode': ('gate', 'return configured == 1 ? Mode::Global : configured == 2 ? Mode::Regional : Mode::Off;',
                                   'return configured >= 1 ? Mode::Regional : Mode::Off;'),
        'step: region rate doubled': ('gate', 'constexpr float kRegionRate = 0.75f;', 'constexpr float kRegionRate = 1.5f;'),
    }
    for name, (which, old, new) in mutants.items():
        text = rule if which == 'rule' else gate
        assert text.count(old) == 1, f'mutant anchor missing or repeated: {name}'
        mutant_dir = Path(directory) / re.sub(r'\W+', '_', name)
        (mutant_dir / 'dlssnr').mkdir(parents=True)
        (mutant_dir / 'shaders/dlssnr/precompile').mkdir(parents=True)
        (mutant_dir / 'dlssnr/DlssNr_LightLimit.h').write_text(text.replace(old, new) if which == 'gate' else gate)
        (mutant_dir / 'shaders/dlssnr/precompile/dlssnr_cadence_rule.h').write_text(
            text.replace(old, new) if which == 'rule' else rule)
        result = build_and_run(mutant_dir, MUTANT_FLAGS)
        assert result.returncode != 0, f'mutant survived: {name}'
    print(f'{len(mutants)} mutants killed')

# --- source guards --------------------------------------------------------------------------------------------------
config_h = (root / 'OptiScaler/Config.h').read_text()
config_cpp = (root / 'OptiScaler/Config.cpp').read_text()
ini = (root / 'OptiScaler.ini').read_text()
config_md = (root / 'Config.md').read_text()
menu = (retoque / 'dlssnr/DlssNr_Menu.cpp').read_text()
renderer = renderer_source()
internal = (retoque / 'shaders/dlssnr/DlssNr_Dx12_Internal.h').read_text()
lifetime = (retoque / 'shaders/dlssnr/DlssNr_Dx12_Lifetime.cpp').read_text()
shader = (retoque / 'shaders/dlssnr/precompile/dlssnr_cadence.hlsl').read_text()
pass_h = (retoque / 'shaders/dlssnr/DlssNr_Cadence_Dx12.h').read_text()
pass_cpp = (retoque / 'shaders/dlssnr/DlssNr_Cadence_Dx12.cpp').read_text()
vulkan = (retoque / 'dlssnr/DlssNrFeature_Vk.cpp').read_text()


def strip(text):
    return re.sub(r'//[^\n]*', '', text)


# The key: off by default, an unsigned choice, round-trips through declaration, read, save and both shipped files.
assert re.search(r'CustomOptional<uint32_t>\s+DlssNrLightLimiter\s*\{\s*0\s*\}', config_h), 'off by default'
assert 'DlssNrLightLimiter.set_from_config(readUInt("DlssNr", "LightLimiter"))' in config_cpp
assert re.search(r'ini\.SetValue\("DlssNr", "LightLimiter", GetIntValue\(Instance\(\)->DlssNrLightLimiter\.value_for_config'
                 r'\(\)\)\.c_str\(\)\)', config_cpp)
assert re.search(r'^LightLimiter=auto$', ini, re.M) and re.search(r'^LightLimiter=auto$', config_md, re.M)
assert 'LightLimiter' not in vulkan, 'the Vulkan route has no limiter and never reads the key'

# The shader: modes 10-12, in the three places a mode lives, and the cbuffer grew at its end only (nr-cadence checks the
# struct against the cbuffer field by field; here, that the old tail is still where it was).
for mode in (10, 11, 12):
    assert f'gMode == {mode}' in shader, mode
    assert f'Base({mode}, ' in pass_cpp, mode
cb = shader[shader.index('cbuffer Params'):]
cb = cb[cb.index('{') + 1:cb.index('};')]
names = re.findall(r'\b(?:uint|float)\s+(\w+)\s*;', strip(cb))
assert names.index('gProbeHistory') == names.index('gLimitMaxGain') + 1, 'append-only: the old tail is unmoved'
assert names[names.index('gProbeHistory') + 1:] == ['gRegionTile', 'gRegionTilesX', 'gRegionTilesY', 'gHaveMotion',
                                                    'gRegionStep', 'gRegionReleaseLow', 'gRegionReleaseHigh',
                                                    'gRegionMotionPixels']
# The regional state is R32F (a typed UAV load every adapter has), the grid RGBA32F (stored, never loaded as a UAV).
assert 'DXGI_FORMAT_R32_FLOAT, _tilesX * 4, _tilesY' in re.sub(r'\s+', ' ', pass_cpp)
assert 'DXGI_FORMAT_R32G32B32A32_FLOAT, _tilesX, _tilesY' in re.sub(r'\s+', ' ', pass_cpp)
assert 'gOut2[uint2(s' in shader and 'RWTexture2D<float>  gOut2' in shader

# The light-only object: built for the limiter alone, returns before any cadence surface exists, and every call that
# needs the cadence's edit refuses it.
ctor = pass_cpp[pass_cpp.index('DlssNr_Cadence_Dx12::DlssNr_Cadence_Dx12('):]
ctor = ctor[:ctor.index('\n}\n')]
cadence_surfaces = ctor[ctor.index('if (!_lightOnly)\n    {'):ctor.index('if (_smoothingAsked)')]
for surface in ('_residual = CreateTexture(', '_proxyThen = CreateTexture(', '_depthThen = CreateTexture(',
                '_residualLow = CreateTexture(', '_chain[0] = CreateTexture(', '_chain[1] = CreateTexture('):
    assert surface in cadence_surfaces, surface
assert 'if (!_lightOnly)\n            _fade = CreateTexture(' in ctor, 'no fade on a light-only object'
tiles = ctor[ctor.index('if (_lightOnly)\n        {\n            _tilesX'):ctor.index('_smoothing = (_lightOnly')]
assert '_regionGrid = CreateTexture(' in tiles and '_regionState = CreateTexture(' in tiles
tail = ctor[ctor.index('if (_lightOnly)\n    {\n        if (!_smoothing)'):]
assert 'return;' in tail and '_init = true;' in tail and tail.index('_init = true;') < len(tail)
assert ctor.count('_init = true;') == 2 and ctor.rindex('_init = true;') > ctor.index('if (_lightOnly)\n    {\n        if (!_smoothing)')
for name in ('::Record(', '::Carry(', '::ModelMotion(', '::Fade(', '::Probe('):
    body = pass_cpp[pass_cpp.index('DlssNr_Cadence_Dx12' + name):]
    assert '_lightOnly' in body[:body.index('\n}\n')][:600], name
assert 'InLightOnly == _lightOnly' in pass_h
assert pass_cpp.count('SAFE_RELEASE(_regionGrid);') >= 2 and pass_cpp.count('SAFE_RELEASE(_regionState);') >= 2

# The renderer: surfaces only while the key is on, one build, after the gate says run; the call after the record and the
# cadence's own fade and before anything reads the answer; the resolve, the down-leg and the unpack read the limited one.
assert renderer.count('new DlssNr_Cadence_Dx12("DLSS-NR light limiter"') == 1
off = renderer.index('Requested(cfg.DlssNrLightLimiter.value_or_default()) == DlssNr::LightLimit::Mode::Off)')
assert 'ParkNrCadence(g_nr.lightLimiter);' in renderer[off:off + 300]
block = renderer.index('const auto lightMode =')
assert renderer.index('g_nr.cadence->ShowFaded(') < block < renderer.index('// Supersampling down-leg.')
built = renderer.index('new DlssNr_Cadence_Dx12("DLSS-NR light limiter"')
assert renderer.index('if (lightNow.run && proxyAtWorkSize)') < built
assert renderer.index('const auto lightMode =') < renderer.index('g_nr.lightGate.Decide(') < built
assert 'g_nr.lightGate.Reset();' in renderer[renderer.index('if (lightMode == DlssNr::LightLimit::Mode::Off)'):built]
assert 'cadenceFaded || (carried && cadence.limit && g_nr.cadence != nullptr && g_nr.cadence->Smoothing())' in renderer
assert 'Decide(lightMode, cadenceLimited, observedFrame, cadenceNowMs,' in renderer
after = renderer[block:]
tail = after[after.index('// Supersampling down-leg.'):after.index('const auto resolved =') + 400]
assert 'g_nr.superDown->Dispatch(cmdList, shownAnswer, g_nr.outputNative)' in tail
assert 'DlssNr::Chain::Resolve(g_nr.colorCopy, modelInput, g_nr.outputNative, shownAnswer, superDownOk)' in tail
assert 'g_nr.periphery->Unpack(cmdList, periphery, modelInput, shownAnswer,' in after
# g_nr.output / chain.answer is the model's own surface: the limiter reads it and never writes it, and it is still the
# one moved back to UNORDERED_ACCESS for the next frame's model write.
assert 'Barrier(cmdList, finalOutput, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE,' in after
assert 'LimitLight(cmdList, cadenceFrame, uiAlpha, light, shownAnswer)' in renderer
# The interface is out of the light and keeps its answer: the limiter gets the same mask the cadence does.
assert 'light.passthrough = !isHdrBuffer;' in renderer
# One object in the state, parked with the generation and deleted at shutdown.
assert internal.count('DlssNr_Cadence_Dx12* lightLimiter = nullptr;') == 1
assert 'ParkNrCadence(g_nr.lightLimiter);' in lifetime
shutdown = renderer[renderer.index('delete g_nr.lightLimiter;') - 200:renderer.index('delete g_nr.lightLimiter;') + 200]
assert 'g_nr.lightLimiter != nullptr' in shutdown

# The menu: D3D12 only, never under the driver proxy or without the cadence shader (its bytecode), its own ID.
fn = menu[menu.index('static void RenderLightLimiter('):]
fn = fn[:fn.index('\n}\n')]
assert 'if (nativeVulkan || config->DlssNrUseProxy.value_or_default() || !DlssNr::CadenceAvailable())\n        return;' in fn
assert 'Light limiter###nrLightLimiter' in fn and 'config->DlssNrLightLimiter = ' in fn
assert menu.count('RenderLightLimiter(config, nativeVulkan);') == 1

print('light limiter: source guards ok')

#!/usr/bin/env python3
"""Detail stability (docs/dlssnr/design/detail-stability.md): the production per-pixel rule and gate compiled under
ASan/UBSan over synthetic sequences; mutants of the key lines (each must fail the cases); source guards on the key, the
shader/host pair, the renderer's call and the menu control. No GPU and no shader execution."""
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
rule_path = retoque / 'shaders/dlssnr/precompile/dlssnr_detail_stability_rule.h'
gate_path = retoque / 'dlssnr/DlssNr_DetailStability.h'
FLAGS = ['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-variable',
         '-fsanitize=address,undefined', '-fno-omit-frame-pointer']
MUTANT_FLAGS = [f for f in FLAGS if f not in ('-Werror', '-Wall', '-Wextra')] + ['-w']


def build_and_run(include_dir, flags):
    binary = Path(include_dir) / 'cases'
    subprocess.run(flags + ['-I', str(include_dir), '-I', str(retoque), str(here / 'cases.cpp'), '-o', str(binary)],
                   check=True)
    return subprocess.run([str(binary)], capture_output=True, text=True)


with tempfile.TemporaryDirectory(prefix='optiscaler-nr-detail-stability-') as directory:
    clean = build_and_run(directory, FLAGS)
    sys.stdout.write(clean.stdout)
    assert clean.returncode == 0, clean.stderr

    rule = rule_path.read_text()
    gate = gate_path.read_text()
    mutants = {
        'no clip to the current 3x3': ('rule', 'const float3 clipped = DsMin3(DsMax3(he, lo), hi);',
                                       'const float3 clipped = he;'),
        'no rejection by the input': ('rule', 'const float rejection = smoothstep(P.bandLow, P.bandHigh, outside);',
                                      'const float rejection = 0.0f;'),
        'rejection judged on the pixel alone': ('rule', 'const float outside = max(max(slo - hs, hs - shi), 0.0f);',
                                                'const float outside = abs(s - hs);'),
        'wide rejection band': ('rule', 'smoothstep(P.bandLow, P.bandHigh, outside)',
                                'smoothstep(P.bandLow, P.bandHigh * 6.0f, outside)'),
        'no motion in the reprojection': ('rule', 'const float qx = (float) px + motion.x;',
                                          'const float qx = (float) px;'),
        'off the picture not rejected': ('rule', 'if (!(qx >= -0.5f && qx <= limitX && qy >= -0.5f && qy <= limitY))\n        return r;',
                                         ''),
        'a texel with nothing stored used': ('rule', 'if ((h00.w < 0.0f && (1.0f - fx) * (1.0f - fy) > 0.0f)',
                                             'if (false && (h00.w < 0.0f && (1.0f - fx) * (1.0f - fy) > 0.0f)'),
        'alpha ignored': ('rule', 'float beta = max(P.alpha, rejection);', 'float beta = rejection;'),
        'repeat not exact (blends)': ('rule', 'P.holdRepeats != 0 && motion.x == 0.0f', 'false && motion.x == 0.0f'),
        'repeat taken with motion': ('rule', 'P.holdRepeats != 0 && motion.x == 0.0f && motion.y == 0.0f && hs == s',
                                     'P.holdRepeats != 0 && hs == s'),
        'repeat taken without input equality': ('rule', 'motion.y == 0.0f && hs == s && rejection <= 0.0f',
                                                'motion.y == 0.0f'),
        'interface filtered': ('rule', 'filtered = filtered + (e - filtered) * ui;', ''),
        'no history means filtered': ('rule', 'if (P.historyValid == 0 || P.alpha >= 1.0f)\n        return r;',
                                      'if (P.alpha >= 1.0f)\n        return r;'),
        'stored edit not rounded': ('rule', 'filtered = DsRound3(filtered);', ''),
        'non-finite not guarded': ('rule', 'if (!DsFinite3(proxy) || !DsFinite3(answerNow))\n        return r;', ''),
        'offset removed': ('rule', '#define DS_K (1.0f / 256.0f)', '#define DS_K (1.0f / 1048576.0f)'),
        'gate: no history after a gap': ('gate', 'd.historyValid = _seen && _ran && !gap &&', 'd.historyValid = _seen && _ran &&'),
        'gate: no history after an unrecorded frame': ('gate', 'd.historyValid = _seen && _ran && !gap', 'd.historyValid = _seen && !gap'),
        'gate: signature ignored': ('gate', ' && signature == _signature', ''),
        'gate: reset ignored': ('gate', ' && !resetPending;', ';'),
        'gate: a second view runs': ('gate', 'if (_seen && present != 0 && present == _lastPresent)', 'if (false)'),
        'gate: runs with a cadence': ('gate', 'if (in.cadence)\n        return Reason::Cadence;', ''),
        'gate: runs before the upscaler': ('gate', 'if (in.beforeUpscale)\n        return Reason::BeforeUpscale;', ''),
        'gate: runs without guides': ('gate', 'if (!in.gameGuides)\n        return Reason::NoGameGuides;', ''),
        'blend: NaN passes': ('gate', 'return std::isfinite(configured) ? std::clamp(configured, kMinBlend, kMaxBlend) : kDefaultBlend;',
                              'return std::clamp(configured, kMinBlend, kMaxBlend);'),
    }
    for name, (which, old, new) in mutants.items():
        text = rule if which == 'rule' else gate
        assert text.count(old) == 1, f'mutant anchor missing or repeated: {name}'
        mutant_dir = Path(directory) / re.sub(r'\W+', '_', name)
        (mutant_dir / 'dlssnr').mkdir(parents=True)
        (mutant_dir / 'shaders/dlssnr/precompile').mkdir(parents=True)
        (mutant_dir / 'dlssnr/DlssNr_DetailStability.h').write_text(text.replace(old, new) if which == 'gate' else gate)
        (mutant_dir / 'shaders/dlssnr/precompile/dlssnr_detail_stability_rule.h').write_text(
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
shader = (retoque / 'shaders/dlssnr/precompile/dlssnr_detail_stability.hlsl').read_text()
pass_h = (retoque / 'shaders/dlssnr/DlssNr_DetailStability_Dx12.h').read_text()
pass_cpp = (retoque / 'shaders/dlssnr/DlssNr_DetailStability_Dx12.cpp').read_text()
vulkan = (retoque / 'dlssnr/DlssNrFeature_Vk.cpp').read_text()
vcxproj = (root / 'OptiScaler/OptiScaler.vcxproj').read_text()


def strip(text):
    return re.sub(r'//[^\n]*', '', text)


# The keys: off by default, round-tripping through declaration, read, save and both shipped files.
assert re.search(r'CustomOptional<bool>\s+DlssNrDetailStability\s*\{\s*false\s*\}', config_h), 'off by default'
assert re.search(r'CustomOptional<float>\s+DlssNrDetailStabilityBlend\s*\{\s*0\.15f\s*\}', config_h)
assert re.search(r'CustomOptional<bool>\s+DlssNrDetailStabilityHold\s*\{\s*false\s*\}', config_h)
for key, reader in (('DetailStability', 'readBool'), ('DetailStabilityBlend', 'readFloat'),
                    ('DetailStabilityHold', 'readBool')):
    member = 'DlssNr' + key
    assert f'{member}.set_from_config({reader}("DlssNr", "{key}"))' in config_cpp, key
    assert re.search(r'ini\.SetValue\("DlssNr", "' + key + r'",\s*Get\w+Value\(Instance\(\)->' + member +
                     r'\.value_for_config\(\)\)\.c_str\(\)\)', config_cpp), key
    assert re.search(rf'^{key}=auto$', ini, re.M) and re.search(rf'^{key}=auto$', config_md, re.M), key
assert 'DetailStability' not in vulkan, 'the Vulkan route has no filter and never reads the keys'

# The shader and the host constants are one ordered list of 4-byte scalars, and the struct a multiple of 256 bytes.
struct_body = pass_h[pass_h.index('struct alignas(256) Constants'):]
struct_body = struct_body[struct_body.index('{') + 1:struct_body.index('};')]
cpp_fields = [(t.replace('uint32_t', 'uint'), n.lower())
              for t, n in re.findall(r'\b(uint32_t|float)\s+(\w+)\s*=', strip(struct_body))]
cbuffer = shader[shader.index('cbuffer Params'):]
cbuffer = cbuffer[cbuffer.index('{') + 1:cbuffer.index('};')]
hlsl_fields = [(t, n[1:].lower()) for t, n in re.findall(r'\b(uint|float)\s+(\w+)\s*;', strip(cbuffer))]
assert cpp_fields and cpp_fields == hlsl_fields, (cpp_fields, hlsl_fields)
assert len(cpp_fields) * 4 <= 256
# Every register the shader declares is a slot the root signature has (five inputs, two outputs, one constant buffer).
assert sorted(int(n) for n in re.findall(r'register\(t(\d+)\)', shader)) == list(range(5))
assert sorted(int(n) for n in re.findall(r'register\(u(\d+)\)', shader)) == list(range(2))
assert 'SetupRootSignature(InDevice, 5, 2, 1)' in pass_cpp
# The shader runs the rule header and defines every hook it reads.
assert '#include "dlssnr_detail_stability_rule.h"' in shader
needed = set(re.findall(r'\b(DS_[A-Z0-9_]+)\b', strip(rule))) - {'DS_K'}
defined = set(re.findall(r'#define\s+(DS_[A-Z0-9_]+)', shader))
assert needed <= defined, needed - defined
assert '-T cs_6_0 -E CSMain' in shader and 'DlssNr_DetailStability_cso' in shader
assert 'DlssNr_DetailStability_cso' in pass_cpp
# The alpha the model would return is the frame's own.
assert 'gOut[id.xy] = float4(result.answer, gAnswer.Load(int3(id.xy, 0)).a);' in shader

# The pass registers in the project, once.
for item in ('<ClCompile Include="..\\retoque\\shaders\\dlssnr\\DlssNr_DetailStability_Dx12.cpp" />',
             '<ClInclude Include="..\\retoque\\shaders\\dlssnr\\DlssNr_DetailStability_Dx12.h" />',
             '<ClInclude Include="..\\retoque\\dlssnr\\DlssNr_DetailStability.h" />',
             '<ClInclude Include="..\\retoque\\shaders\\dlssnr\\precompile\\dlssnr_detail_stability_rule.h" />',
             '<ClInclude Include="..\\retoque\\shaders\\dlssnr\\precompile\\DlssNr_DetailStability_Shader.h" />'):
    assert vcxproj.count(item) == 1, item

# Surfaces: three working-size textures, in the resting state, and a failed object still fits.
assert pass_cpp.count('CreateTexture(InDevice, L"') == 3
assert 'InDevice == _device && InWidth == _width && InHeight == _height' in pass_h

# The renderer: one build, only after the gate says run; parked with the generation, when the key is off or before the
# upscaler; deleted at shutdown; the filtered answer read by the limiter, the down-leg, the unpack and the resolve.
assert renderer.count('new DlssNr_DetailStability_Dx12(') == 1
off = renderer.index('if (!cfg.DlssNrDetailStability.value_or_default() || beforeUpscale)')
assert 'ParkNrDetailStability(g_nr.detailStability);' in renderer[off:off + 200]
decide = renderer.index('g_nr.detailGate.Decide(')
built = renderer.index('new DlssNr_DetailStability_Dx12(')
assert renderer.index('g_nr.cadence->ShowFaded(') < decide < renderer.index('if (stabilityNow.run)') < built
assert built < renderer.index('g_nr.detailStability->Filter(')
assert renderer.index('g_nr.detailStability->Filter(') < renderer.index('const auto lightMode =') < \
    renderer.index('// Supersampling down-leg.')
for field in ('enabled = cfg.DlssNrDetailStability.value_or_default()', 'driverProxy = useProxy',
              'beforeUpscale = beforeUpscale', 'gameGuides = frame.GameGuides',
              'frameGeneration = FrameGenerationActive()', 'cadence = cadenceInputs.requested > 1'):
    assert f'stability.{field};' in renderer, field
assert 'Decide(stability, observedFrame, cadenceNowMs, stabilitySignature, stabilizerReset)' in renderer
assert 'Filter(cmdList, stabilityFrame, finalOutput, stabilitySettings)' in renderer
assert 'stabilityFrame.proxy = modelInput;' in renderer and 'stabilityFrame.motion = cadenceFrame.motion;' in renderer
assert 'stabilityFrame.mvToWorkX = cadenceFrame.mvToWorkX;' in renderer
assert 'LimitLight(cmdList, cadenceFrame, uiAlpha, light, shownAnswer)' in renderer
assert 'g_nr.superDown->Dispatch(cmdList, shownAnswer, g_nr.outputNative)' in renderer
# The model's own surface is read, never written.
assert internal.count('DlssNr_DetailStability_Dx12* detailStability = nullptr;') == 2  # the state and the retired entry
assert 'ParkNrDetailStability(g_nr.detailStability);' in lifetime
assert 'delete retired.detailStability;' in lifetime and 'delete r.detailStability;' in renderer
assert 'delete g_nr.detailStability;' in renderer

# The menu: D3D12 only, never under the driver proxy, three controls with their own IDs.
fn = menu[menu.index('static void RenderDetailStability('):]
fn = fn[:fn.index('\n}\n')]
assert 'if (nativeVulkan || config->DlssNrUseProxy.value_or_default())\n        return;' in fn
for ident in ('Detail stability###nrDetailStability', 'Detail stability blend###nrDetailStabilityBlend',
              'Hold repeated frames###nrDetailStabilityHold'):
    assert ident in fn, ident
assert menu.count('RenderDetailStability(config, nativeVulkan);') == 1

print('detail stability: source guards ok')

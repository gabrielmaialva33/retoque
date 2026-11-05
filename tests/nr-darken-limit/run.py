#!/usr/bin/env python3
"""Compile the actual composition writeback/floor as float32 host maths. No GPU."""
from pathlib import Path
import re
import subprocess
import tempfile
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / '_support'))
from cpp_source import function, mutate_function
from nr_sources import renderer_with_internal_types as renderer_source  # noqa: E402  every DlssNr_Dx12*.cpp unit, in a fixed order

here = Path(__file__).resolve().parent
root = here.parents[1]
shader = (root / 'retoque/shaders/dlssnr/precompile/dlssnr.hlsl').read_text()
start = shader.index('    // Back out of the normalised space')
end = shader.index('    // The side being shown untouched', start)
helpers = ''
if '// --- darkening floor maths ---' in shader:
    helpers = shader.split('// --- darkening floor maths ---')[1].split('// --- end darkening floor maths ---')[0]
# Placement and wiring guard the cases whose upstream composition paths differ.
live = re.sub(r'//[^\n]*', '', shader)
assert live.count('ApplyDarkeningFloor(') == 2, 'one definition, one final application'
gate = live.index('if (gMinRatio > 0.0 && isfinite(gMinRatio))')
for before in ('if (gReversibleMode == 2)', 'if (gReversibleMode == 4)',
               'result = max(result * normScale, 0.0) + signedRest;'):
    assert live.index(before) < gate
assert gate < live.index('if (showOriginal)', gate)
renderer = renderer_source()
vulkan = (root / 'retoque/dlssnr/DlssNrFeature_Vk.cpp').read_text()
assert 'resolveParams.MinRatio = DlssNr::MinimumRatio(cfg.DlssNrMinRatio.value_or_default());' in renderer
floor = 'encode.MinRatio = DlssNr::MinimumRatio(cfg.DlssNrMinRatio.value_or_default());'
routes = ('EvaluateAfterUpscaleVk', 'RecordPreUpscaleVk', 'VkPresentModel::Record')


def check_vulkan(text):
    for route in routes:
        code = function(text, route)
        assert code.count(floor) == 1, route + ': one configured darkening floor'
        assert code.index(floor) < code.index('DlssNrConstants resolve = encode;'), route + ': resolve inherits floor'
    assert text.count(floor) == len(routes), 'unreviewed floor site'


check_vulkan(vulkan)
for route in routes:
    try:
        check_vulkan(mutate_function(vulkan, route, floor, 'encode.MinRatio = 0.0f;'))
    except AssertionError:
        continue
    raise AssertionError('floor mutant survived: ' + route)
print('PASS: configured MinRatio reaches resolve in post, pre and present; 3 independent mutants rejected')
assert 'minRatio == other.minRatio' in renderer
assert 'float minRatio = 0.0f;' in renderer
luma = re.search(r'(?:static )?const float3 kLuma = [^;]+;', shader).group(0)
with tempfile.TemporaryDirectory(prefix='nr-darken-limit-') as directory:
    inc = Path(directory) / 'composition.inc'
    inc.write_text(luma + '\n' + helpers + '\n' +
        'float3 Finish(float3 result, float normScale, float3 signedRest, float3 originalRGB, '
        '[[maybe_unused]] float gMinRatio) {\nstruct Sample { float3 rgb; }; [[maybe_unused]] Sample originalSample { originalRGB };\n' +
        shader[start:end] + '\nreturn result;\n}\n')
    binary = str(Path(directory) / 'test')
    subprocess.run(['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
        '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-I', directory, '-I', str(root / 'retoque'),
        str(here / 'test.cpp'), '-o', binary], check=True)
    subprocess.run([binary], check=True)

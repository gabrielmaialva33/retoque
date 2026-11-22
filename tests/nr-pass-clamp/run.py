#!/usr/bin/env python3
"""Host contracts and mutants for F3 interpass clamp and PassFeedback (docs/dlssnr/design/2026-11-22-interpass-clamp.md)."""
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
RETOQUE = ROOT / 'retoque'
OPTI = ROOT / 'OptiScaler'
sys.path.insert(0, str(HERE.parent / '_support'))
from nr_sources import renderer_source  # noqa: E402

FLAGS = [
    'g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
    '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined', '-fno-omit-frame-pointer',
]

RULE_MUTANTS = [
    ('single_pass_active', 'dlssnr/DlssNr_PassClamp.h',
     'requestedPasses > 1 &&', 'requestedPasses >= 1 &&'),
    ('missing_second_scratch_on_3_passes', 'dlssnr/DlssNr_PassClamp.h',
     'requestedPasses > 2 &&', 'requestedPasses > 3 &&'),
    ('boundary_always_uses_clamp0', 'dlssnr/DlssNr_PassClamp.h',
     'slot == 0u ? clamp0 : clamp1,', '((void) clamp1, clamp0),'),
    ('boundary_clamps_against_raw', 'dlssnr/DlssNr_PassClamp.h',
     '        prevPassInput,\n', '        ((void) prevPassInput, rawAnswer),\n'),
    ('per_channel_saturate_loses_hue', 'shaders/dlssnr/precompile/dlssnr_pass_clamp_rule.h',
     'return clamp(proxy + saturate(alpha) * d, lo, hi);', 'return clamp(target, lo, hi);'),
    ('nan_falls_back_to_grey_half', 'shaders/dlssnr/precompile/dlssnr_pass_clamp_rule.h',
     'const float3 sanRaw = PassClampFinite3(raw, sanProxy);',
     'const float3 sanRaw = PassClampFinite3(raw, float3(0.5f, 0.5f, 0.5f));'),
    ('passthrough_superwhite_crushed_to_unit', 'shaders/dlssnr/precompile/dlssnr_pass_clamp_rule.h',
     'const float3 hi = max(proxy, float3(1.0f, 1.0f, 1.0f));',
     'const float3 hi = float3(1.0f, 1.0f, 1.0f);'),
    ('feedback_ignored', 'shaders/dlssnr/precompile/dlssnr_pass_clamp_rule.h',
     'return feedback >= 1.0f ? restored : sanProxy + feedback * (restored - sanProxy);',
     'return ((void) feedback, restored);'),
]


def check_source_guards(renderer: str, vcxproj: str, filters: str) -> None:
    for item in (
        '..\\retoque\\dlssnr\\DlssNr_PassClamp.h',
        '..\\retoque\\shaders\\dlssnr\\DlssNr_PassClamp_Dx12.h',
        '..\\retoque\\shaders\\dlssnr\\precompile\\dlssnr_pass_clamp_rule.h',
        '..\\retoque\\shaders\\dlssnr\\precompile\\DlssNr_PassClamp_Shader.h',
        '..\\retoque\\shaders\\dlssnr\\DlssNr_PassClamp_Dx12.cpp',
    ):
        assert vcxproj.count(f'Include="{item}"') == 1, f'OptiScaler.vcxproj missing {item}'
        assert filters.count(f'Include="{item}"') == 1, f'OptiScaler.vcxproj.filters missing {item}'

    assert re.search(r'DlssNr::PassClamp::PlanBoundary\(\s*i,\s*input,\s*prevPassInput,', renderer), \
        'Dispatch does not route interpass boundaries through PlanBoundary(..., prevPassInput, ...)'
    assert 'prevPassInput = input;' in renderer, \
        'Dispatch does not advance prevPassInput to each pass\'s own input'
    assert re.search(r'g_nr\.passClamp->Dispatch\(\s*cmdList,\s*boundary\.raw,\s*boundary\.proxy,\s*boundary\.target',
                     renderer), \
        'Dispatch does not invoke DlssNr_PassClamp_Dx12::Dispatch on interpass boundary'

    untracked = renderer[renderer.index('static void ParkUntrackedGeneration('):renderer.index('bool SecondGenerationFits(')]
    assert 'g_nr.passClampScratch' in untracked and 'g_nr.passClampScratch2' in untracked and \
           'shaders.passClamp' in untracked, \
        'ParkUntrackedGeneration must park both interpass clamp scratch surfaces and passClamp shader'

    room = renderer[renderer.index('bool MakeRoomForGeneration()\n{'):renderer.index('void ParkAltSurfaces()')]
    assert 'g_nr.passClampScratch' in room and 'g_nr.passClampScratch2' in room, \
        'MakeRoomForGeneration must park both interpass clamp scratch surfaces'

    alt = renderer[renderer.index('void ParkAltSurfaces()'):renderer.index('bool AltSurfacesFit(')]
    assert 'alt.passClampScratch' in alt and 'alt.passClampScratch2' in alt, \
        'ParkAltSurfaces must park alternate-format interpass clamp surfaces'

    swap = renderer[renderer.index('void ReleaseSurfacesIfFormatChanged('):renderer.index('void ParkNrClone(')]
    assert 'std::swap(g_nr.passClampScratch, alt.passClampScratch);' in swap and \
           'std::swap(g_nr.passClampScratch2, alt.passClampScratch2);' in swap, \
        'ReleaseSurfacesIfFormatChanged must swap alternate-format interpass clamp surfaces'


with tempfile.TemporaryDirectory(prefix='nr-pass-clamp-') as temp_dir:
    folder = Path(temp_dir)
    binary = folder / 'cases'
    subprocess.run(FLAGS + ['-I', str(RETOQUE), str(HERE / 'cases.cpp'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
    print('PASS: interpass clamp math, NaN/Inf fallback, PassFeedback, 2..10 pass routing and temporal-age rules hold')

    for idx, (name, rel, old, new) in enumerate(RULE_MUTANTS):
        src_text = (RETOQUE / rel).read_text()
        assert src_text.count(old) == 1, f'rule mutant {name!r} no longer matches uniquely'
        overlay = folder / f'mutant_{idx}'
        for copied in (
            'dlssnr/DlssNr_Chain.h',
            'dlssnr/DlssNr_PassSettings.h',
            'dlssnr/DlssNr_PassClamp.h',
            'shaders/dlssnr/precompile/dlssnr_pass_clamp_rule.h',
        ):
            dest = overlay / copied
            dest.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(RETOQUE / copied, dest)
        (overlay / rel).write_text(src_text.replace(old, new, 1))
        mutant_bin = overlay / 'cases'
        built = subprocess.run(FLAGS + ['-I', str(overlay), str(HERE / 'cases.cpp'), '-o', str(mutant_bin)],
                               capture_output=True)
        assert built.returncode == 0, f'rule mutant {name!r} failed to compile:\n{built.stderr.decode()}'
        ran = subprocess.run([str(mutant_bin)], capture_output=True)
        assert ran.returncode != 0, f'rule mutant {name!r} escaped cases.cpp'
    print(f'PASS: {len(RULE_MUTANTS)} rule mutants compile and fail cases.cpp')

renderer = renderer_source()
vcxproj = (OPTI / 'OptiScaler.vcxproj').read_text(encoding='utf-8-sig')
filters = (OPTI / 'OptiScaler.vcxproj.filters').read_text(encoding='utf-8-sig')
check_source_guards(renderer, vcxproj, filters)

SOURCE_MUTANTS = [
    ('stale_modelInput_proxy', renderer.replace('prevPassInput = input;', '', 1), vcxproj, filters),
    ('unparked_scratch2_in_untracked', renderer.replace('&g_nr.passClampScratch2,', '', 1), vcxproj, filters),
    ('unparked_scratch2_in_room', renderer.replace('&g_nr.passClampScratch2', ''), vcxproj, filters),
    ('unswapped_alt_scratch', renderer.replace('std::swap(g_nr.passClampScratch, alt.passClampScratch);', '', 1),
     vcxproj, filters),
    ('missing_vcxproj_unit', renderer,
     vcxproj.replace('<ClCompile Include="..\\retoque\\shaders\\dlssnr\\DlssNr_PassClamp_Dx12.cpp" />', '', 1),
     filters),
]
for name, mr, mv, mf in SOURCE_MUTANTS:
    assert (mr, mv, mf) != (renderer, vcxproj, filters), f'source mutant {name!r} did not apply'
    try:
        check_source_guards(mr, mv, mf)
    except AssertionError:
        continue
    raise SystemExit(f'FAIL: source guard accepted mutant {name!r}')
print(f'PASS: {len(SOURCE_MUTANTS)} source mutants rejected')

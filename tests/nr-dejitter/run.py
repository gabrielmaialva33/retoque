#!/usr/bin/env python3
"""Host contracts and mutants for F5 motion-vector de-jittering (docs/dlssnr/design/2026-11-22-dejitter-motion-vectors.md)."""
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
    ('allow_before_upscale', 'dlssnr/DlssNr_Dejitter.h',
     '!in.beforeUpscale &&', 'true &&'),
    ('allow_unmarked_mvs', 'dlssnr/DlssNr_Dejitter.h',
     'in.mvJittered &&', 'true &&'),
    ('allow_display_res_mvs', 'dlssnr/DlssNr_Dejitter.h',
     'in.motionWidth == in.renderWidth &&', 'true &&'),
    ('ignore_sequence_gap', 'dlssnr/DlssNr_Dejitter.h',
     'in.renderSequence != state.prevRenderSequence + 1', 'false'),
    ('wrong_sign_subtract', 'dlssnr/DlssNr_Dejitter.h',
     '(in.jitterX - state.prevJitterX) / in.mvScaleX',
     '(state.prevJitterX - in.jitterX) / in.mvScaleX'),
    ('multiply_instead_of_divide_scale', 'dlssnr/DlssNr_Dejitter.h',
     '(in.jitterX - state.prevJitterX) / in.mvScaleX',
     '(in.jitterX - state.prevJitterX) * in.mvScaleX'),
    ('no_reset_on_disable', 'dlssnr/DlssNr_Dejitter.h',
     'wasActive && !in.held', '((void) wasActive, false)'),
    ('overflow_corrupts_mv', 'shaders/dlssnr/precompile/dlssnr_dejitter_rule.h',
     '(isfinite(corrected.x) && isfinite(corrected.y)) ? corrected : mv',
     'corrected'),
]


def check_source_guards(renderer: str, vcxproj: str, filters: str) -> None:
    for item in (
        '..\\retoque\\dlssnr\\DlssNr_Dejitter.h',
        '..\\retoque\\shaders\\dlssnr\\DlssNr_Dejitter_Dx12.h',
        '..\\retoque\\shaders\\dlssnr\\precompile\\dlssnr_dejitter_rule.h',
        '..\\retoque\\shaders\\dlssnr\\precompile\\DlssNr_Dejitter_Shader.h',
        '..\\retoque\\shaders\\dlssnr\\DlssNr_Dejitter_Dx12.cpp',
    ):
        assert vcxproj.count(f'Include="{item}"') == 1, f'OptiScaler.vcxproj missing {item}'
        assert filters.count(f'Include="{item}"') == 1, f'OptiScaler.vcxproj.filters missing {item}'

    assert 'NVSDK_NGX_DLSS_Feature_Flags_MVJittered' in renderer, \
        'GatherGuideFrame must read NVSDK_NGX_DLSS_Feature_Flags_MVJittered'
    assert 'NVSDK_NGX_Parameter_Jitter_Offset_X' in renderer and \
           'NVSDK_NGX_Parameter_Jitter_Offset_Y' in renderer, \
        'GatherGuideFrame must read NVSDK_NGX_Parameter_Jitter_Offset_X/Y'
    assert 'DlssNr::Dejitter::Step(nextDejitter, dejitterInputs)' in renderer, \
        'Dispatch must step DlssNr::Dejitter::State'
    assert 'cadenceSettings = Cadence::Mix(cadenceSettings, dejitterPlan.active ? 1u : 0u);' in renderer, \
        'Cadence signature must include dejitter active state'
    assert re.search(
        r'g_nr\.dejitterPass->Dispatch\(\s*cmdList,\s*motionIn,\s*g_nr\.motionDejittered,', renderer), \
        'Dispatch must invoke DlssNr_Dejitter_Dx12::Dispatch into g_nr.motionDejittered'
    assert renderer.index('g_nr.dejitterPass->Dispatch(') < renderer.index('g_nr.guideMatch->Dispatch('), \
        'Motion de-jitter must run before GuideMatch resamples motion for the model'
    assert re.search(
        r'DispatchPass\(\s*cmdList,\s*resolveParams,\s*resolveProxy,\s*resolveAnswer,\s*g_nr\.hdrCopy,\s*rawMotionIn,',
        renderer), \
        'Final Resolve DispatchPass must receive rawMotionIn'
    assert 'frame.RenderSequence = temporalSeq;' in renderer, \
        'RunPresentPass must forward captured render sequence to DlssNrFrameInfo'

    untracked = renderer[renderer.index('static void ParkUntrackedGeneration('):renderer.index('bool SecondGenerationFits(')]
    assert '&g_nr.motionDejittered' in untracked and 'shaders.dejitterPass' in untracked, \
        'ParkUntrackedGeneration must park motionDejittered and dejitterPass'

    room = renderer[renderer.index('bool MakeRoomForGeneration()\n{'):renderer.index('void ParkAltSurfaces()')]
    assert '&g_nr.motionDejittered' in room, \
        'MakeRoomForGeneration must park motionDejittered'


with tempfile.TemporaryDirectory(prefix='nr-dejitter-') as temp_dir:
    folder = Path(temp_dir)
    binary = folder / 'cases'
    subprocess.run(FLAGS + ['-I', str(RETOQUE), str(HERE / 'cases.cpp'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
    print('PASS: motion de-jitter eligibility, sign cancellation, sequence/contract resets and overflow guards hold')

    for idx, (name, rel, old, new) in enumerate(RULE_MUTANTS):
        src_text = (RETOQUE / rel).read_text()
        assert src_text.count(old) == 1, f'rule mutant {name!r} no longer matches uniquely'
        overlay = folder / f'mutant_{idx}'
        for copied in (
            'dlssnr/DlssNr_Dejitter.h',
            'shaders/dlssnr/precompile/dlssnr_dejitter_rule.h',
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
    ('missing_jitter_flag', renderer.replace('NVSDK_NGX_DLSS_Feature_Flags_MVJittered', '0', 1), vcxproj, filters),
    ('resolve_uses_dejittered_motion', renderer.replace('rawMotionIn,\n', 'motionIn,\n', 1), vcxproj, filters),
    ('unparked_in_untracked', renderer.replace('&g_nr.motionDejittered,', '', 1), vcxproj, filters),
    ('unparked_in_room', renderer.replace('&g_nr.motionDejittered', ''), vcxproj, filters),
    ('missing_present_sequence', renderer.replace('frame.RenderSequence = temporalSeq;', '', 1), vcxproj, filters),
    ('missing_vcxproj_unit', renderer,
     vcxproj.replace('<ClCompile Include="..\\retoque\\shaders\\dlssnr\\DlssNr_Dejitter_Dx12.cpp" />', '', 1),
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

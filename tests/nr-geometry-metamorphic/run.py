#!/usr/bin/env python3
"""Metamorphic controls of the NR input geometry (Codex N-03). See README.md.

Pastes the production code that prepares the model's depth and motion into harness.cpp.in -- GatherGuideFrame,
Dispatch's geometry, reference and scale blocks, the Vulkan route's geometry and plan, and the guide resample's HLSL --
compiles it (g++, C++20, ASan/UBSan, -Werror) and runs one analytic scene through many equivalent representations.
Then the mutants: each reads the margin or changes a unit without compensating the scale, in the production text or in
the transformation, and each must make the harness fail. A mutant that survives fails the suite.

No device, no NGX, no model: these are the CPU contracts and the shader's own arithmetic on a CPU shim."""
from pathlib import Path
import os
import re
import shutil
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def between(text, start, end, after=None):
    """text from the one occurrence of `start` up to the first `end` after it."""
    if text.count(start) != 1:
        raise SystemExit(f'FAIL: expected exactly one {start!r} in the production source, found {text.count(start)}')
    i = text.index(start)
    j = text.index(end, i if after is None else text.index(after, i))
    return text[i:j]


def pieces():
    dispatch = (ROOT / 'retoque/shaders/dlssnr/DlssNr_Dx12.cpp').read_text()
    inputs = (ROOT / 'retoque/shaders/dlssnr/DlssNr_Dx12_Inputs.cpp').read_text()
    vulkan = (ROOT / 'retoque/dlssnr/DlssNrFeature_Vk.cpp').read_text()
    hlsl = (ROOT / 'retoque/shaders/dlssnr/precompile/dlssnr_guides.hlsl').read_text()
    shader = hlsl[hlsl.rindex('#endif') + len('#endif'):]
    shader = re.sub(r'\[numthreads\([^)]*\)\]', '', shader)
    shader = shader.replace(': SV_DispatchThreadID', '')
    return {
        '@GATHER@': between(inputs, 'DlssNrFrameInfo GatherGuideFrame(', 'DlssNrFrameInfo GatherFrame('),
        # The same cut tests/nr-render-geometry makes.
        '@GEOMETRY@': between(dispatch, '    const D3D12_RESOURCE_DESC guideDesc = depth->GetDesc();',
                              '    // What the model is actually being shown'),
        '@REFERENCES@': between(dispatch, '    const unsigned int motionReferenceWidth =', '    if (peripheryActive)'),
        '@SCALE@': between(dispatch, '    const bool renderMotionScale = cfg.DlssNrRenderMotionScale.value_or_default();',
                           '    const int guideDepthInverted'),
        '@VULKAN@': between(vulkan, '    const unsigned int createFlags = GameCreateFlags(params);',
                            '    // Both have to agree.'),
        '@SHADER@': shader,
    }


# Each mutant: (name, piece, production text, replacement) or (name, None, macro, None). Every one must fail.
MUTANTS = [
    # Reading the margin.
    ('d3d12 motion region is the allocation past its base', '@GEOMETRY@',
     'std::min(wantedMotionWidth, motionAllocWidth - motionBaseX)', '(motionAllocWidth - motionBaseX)'),
    ('d3d12 depth region ignores the render size', '@GEOMETRY@', '        guideWidth = subW;\n', '        (void) subW;\n'),
    ('d3d12 depth base dropped', '@GEOMETRY@', 'std::min(frame.DepthSubrectBaseX, guideAllocWidth)', '0u'),
    ('gather ignores the motion base', '@GATHER@',
     'params->Get(NVSDK_NGX_Parameter_DLSS_Input_MV_SubrectBase_X, &frame.MotionSubrectBaseX);', ''),
    ('vulkan motion region is the allocation past its base', '@VULKAN@',
     'std::min(wantedMotionWidth, motionAllocWidth - motionBaseX)', '(motionAllocWidth - motionBaseX)'),
    ('vulkan depth region ignores the render size', '@VULKAN@', 'guideWidth = std::min(renderWidth, guideWidth);',
     '(void) renderWidth;'),
    ('shader ignores the region base', '@SHADER@', 'return regionBase + min(', 'return 0u * regionBase + min('),
    ('shader maps over the destination size, not the region', '@SHADER@',
     '((destination * 2u + 1u) * regionExtent) / (destinationExtent * 2u)', 'destination'),
    # Changing a unit without compensating the scale.
    ('the transformation scales the vectors and not the MV scale', None, 'MUTATE_UNCOMPENSATED', None),
    ('gather drops the game MV scale', '@GATHER@',
     'params->Get(NVSDK_NGX_Parameter_MV_Scale_X, &frame.MvScaleX) != NVSDK_NGX_Result_Success', 'true'),
    ('d3d12 scale measured against the output whatever MVLowRes says', '@SCALE@',
     'ModelMotionScale(g_nr.guideMvScaleX, modelMotionWidth, motionReferenceWidth)',
     'ModelMotionScale(g_nr.guideMvScaleX, modelMotionWidth, width)'),
    ('d3d12 render reference from the allocation', '@GEOMETRY@',
     'frame.RenderSubrectWidth ? frame.RenderSubrectWidth : guideAllocWidth', 'guideAllocWidth'),
    ('vulkan reference ignores MVLowRes', '@VULKAN@',
     'MotionReference(lowResolutionMotion, renderReferenceWidth, width)',
     'MotionReference(false, renderReferenceWidth, width)'),
    ('vulkan motion region from depth\'s clipped width', '@VULKAN@',
     'lowResolutionMotion ? renderReferenceWidth : width', 'lowResolutionMotion ? guideWidth : width'),
    ('vulkan drops the game MV scale', '@VULKAN@', 'params->Get(NVSDK_NGX_Parameter_MV_Scale_X, &mvScaleX);', ''),
]


def compiler():
    for name in ('g++', 'clang++'):
        if shutil.which(name):
            return name
    raise SystemExit('SKIP: no C++ compiler')


def build(cxx, source, binary, sanitize, defines=()):
    # Mutants may leave a parameter unused; only the real harness is held to -Werror.
    flags = [cxx, '-std=c++20', '-Wall', '-Wextra', '-Wno-unused-variable', '-Wno-unused-but-set-variable',
             '-I', str(ROOT / 'retoque'), '-I', str(ROOT / 'OptiScaler'), '-I', str(ROOT / 'external/nvngx_dlss_sdk'),
             '-I', str(ROOT / 'external/vulkan/include')]
    flags += ['-Werror', '-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer'] if sanitize else \
        ['-O1', '-w']
    flags += [f'-D{d}' for d in defines]
    return subprocess.run(flags + [str(source), '-o', str(binary)], capture_output=True, text=True)


def main():
    for header in ('external/nvngx_dlss_sdk/nvsdk_ngx.h', 'external/vulkan/include/vulkan/vulkan.h'):
        if not (ROOT / header).is_file():
            print(f'SKIP: {header} is not checked out')
            return 0
    cxx = compiler()
    template = (HERE / 'harness.cpp.in').read_text()
    real = pieces()

    def render(replacements):
        text = template
        for key, value in replacements.items():
            text = text.replace(key, value)
        return text

    failures = []
    with tempfile.TemporaryDirectory(prefix='nr-geometry-metamorphic-') as folder:
        folder = Path(folder)
        unit, binary = folder / 'harness.cpp', folder / 'harness'
        unit.write_text(render(real))
        built = build(cxx, unit, binary, sanitize=True)
        if built.returncode:
            print(built.stdout + built.stderr)
            print('FAIL: the harness does not compile')
            return 1
        ran = subprocess.run([str(binary)], capture_output=True, text=True)
        print(ran.stdout.strip())
        if ran.returncode:
            print(ran.stderr.strip())
            failures.append('the production geometry breaks a relation')

        for name, key, old, new in MUTANTS:
            if key is None:
                pieces_now, defines = real, (old,)
            else:
                if real[key].count(old) != 1:
                    failures.append(f'mutant "{name}": the production text it mutates is gone ({key}: {old!r})')
                    continue
                pieces_now, defines = dict(real, **{key: real[key].replace(old, new)}), ()
            unit.write_text(render(pieces_now))
            built = build(cxx, unit, binary, sanitize=False, defines=defines)
            if built.returncode:
                verdict = 'killed (does not compile)'
            else:
                result = subprocess.run([str(binary)], capture_output=True, text=True)
                verdict = 'killed' if result.returncode else 'SURVIVED'
                if result.returncode:
                    first = next((l for l in result.stdout.splitlines() if l.startswith('FAIL ')), '')
                    verdict += f': {first.split(" -- ", 1)[-1][:150]}'
            print(f'  mutant {name}: {verdict}')
            if verdict.startswith('SURVIVED') or 'does not compile' in verdict:
                # A mutant that no longer compiles proves nothing about the relations.
                failures.append(f'mutant "{name}" {verdict}')

    if failures:
        for f in failures:
            print(f'FAIL: {f}')
        return 1
    print(f'PASS: nr-geometry-metamorphic, the relations hold and all {len(MUTANTS)} mutants are killed')
    return 0


if __name__ == '__main__':
    sys.exit(main())

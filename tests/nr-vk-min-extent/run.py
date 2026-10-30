#!/usr/bin/env python3
"""The Vulkan NR model is never built below kDlssNrMinExtent: the shared decision (DlssNr_Common.h) on host, the present
host's count and line for its refusal, and source guards that the after-upscale route, the present host's admission
and its builder all ask that one decision before anything is built, and that the route counts and logs what it refuses.
"""
from pathlib import Path
import os
import re
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
VULKAN = ROOT / 'external/vulkan/include'
assert (VULKAN / 'vulkan/vulkan.h').exists(), 'external/vulkan is not checked out'
failed = []


def say(ok, what):
    print(f'{"PASS" if ok else "FAIL"} {what}', flush=True)
    if not ok:
        failed.append(what)


with tempfile.TemporaryDirectory(prefix='nr-vk-min-extent-') as folder:
    binary = Path(folder) / 'test'
    built = subprocess.run(['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                            '-fno-sanitize-recover=undefined', '-fno-omit-frame-pointer', '-isystem', str(VULKAN),
                            '-I', str(ROOT / 'retoque'), '-I', str(ROOT / 'retoque/dlssnr'), str(HERE / 'test.cpp'),
                            '-o', str(binary)], capture_output=True, text=True)
    sys.stdout.write(built.stderr[-3000:])
    say(built.returncode == 0, 'the decision and the present host counters compile')
    if built.returncode == 0:
        result = subprocess.run([str(binary)], capture_output=True, text=True,
                                env=dict(os.environ, ASAN_OPTIONS='detect_leaks=0'))
        sys.stdout.write(result.stdout + result.stderr[-3000:])
        say(result.returncode == 0, 'the decision: 1x1 and every size below 64 refused, 64 and above built')


def function(source, signature):
    start = source.index(signature)
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


feature = (ROOT / 'retoque/dlssnr/DlssNrFeature_Vk.cpp').read_text()
common = (ROOT / 'retoque/shaders/dlssnr/DlssNr_Common.h').read_text()

# One floor, shared: defined once, and the Vulkan side compares only through the decision.
definitions = [str(p.relative_to(ROOT)) for d in ('retoque', 'OptiScaler') for p in (ROOT / d).rglob('*')
               if p.suffix in ('.h', '.cpp') and re.search(r'constexpr[^;\n]*\bkDlssNrMinExtent\s*=', p.read_text(errors='ignore'))]
say(definitions == ['retoque/shaders/dlssnr/DlssNr_Common.h'], f'kDlssNrMinExtent is defined once (found {definitions})')
say('constexpr bool BelowMinExtent(' in common, 'the decision lives beside the constant')
say('< kDlssNrMinExtent' not in feature, 'the Vulkan routes compare only through DlssNr::BelowMinExtent')

CHECK = 'DlssNr::BelowMinExtent(width, height, workWidth, workHeight)'

# The after-upscale route: asked before the forwarder, NGX or any image; counted; logged with the count.
route = function(feature, 'void EvaluateAfterUpscaleVk(')
at = route.find(CHECK)
say(at >= 0, 'the after-upscale route asks the decision')
for later in ('LoadForwarder()', 'g_fwd.init(', 'g_fwd.create(', 'ClaimModelForRoute(', 'CreateImage('):
    if later in route:
        say(0 <= at < route.index(later), f'the after-upscale route refuses before {later}')
branch = route[at:route.index('return;', at)] if at >= 0 else ''
say('++g_vkTooSmall' in branch, 'the after-upscale route counts every refused frame')
say('LOG_INFO' in branch and 'g_vkTooSmall' in branch[branch.find('LOG_INFO'):] and 'kDlssNrMinExtent' in branch,
    'the after-upscale route logs the reason with the count')

# The present host's admission: before a build is decided or requested; its refusal is NrSkip::TooSmall.
admit = function(feature, 'VkPresentAdmission VkPresentModel::Admit(')
at = admit.find(CHECK)
say(at >= 0 and at < admit.index('DecideBuild(') and at < admit.index('m.Request('),
    'the present host refuses before deciding or requesting a build')
say('NrSkip::TooSmall' in admit[at:admit.index('DecideBuild(')], 'the refusal is NrSkip::TooSmall, counted by the host')

# The builder: a key below the floor builds nothing, whatever asked for it.
build = function(feature, 'void RunBuild(PresentBuildJob& job)')
at = build.find('DlssNr::BelowMinExtent(job.key.width, job.key.height, job.key.workWidth, job.key.workHeight)')
say(at >= 0, 'the builder asks the decision for its key')
for later in ('LoadForwarderShared()', 'g_fwd.init(', 'NVNGXProxy::InitVulkan(', 'g_fwd.create(', 'CreateImage'):
    if later in build:
        say(0 <= at < build.index(later), f'the builder refuses before {later}')

# D3D12 keeps its own check on the same constant.
dx12 = (ROOT / 'retoque/shaders/dlssnr/DlssNr_Dx12.cpp').read_text()
say('width < kDlssNrMinExtent || height < kDlssNrMinExtent || workWidth < kDlssNrMinExtent' in dx12,
    'the D3D12 route keeps its floor on the shared constant')

if failed:
    raise SystemExit(f'Failed: {len(failed)}: ' + '; '.join(failed))
print('PASS: one floor; the route, the admission and the builder refuse below it, counted and logged')

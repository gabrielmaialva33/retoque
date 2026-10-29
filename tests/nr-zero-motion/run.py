#!/usr/bin/env python3
"""Zero-motion detection (docs/dlssnr/design/zero-motion-detection.md): the production CPU half compiled under
ASan/UBSan and driven over synthetic grids, plus source guards on the GPU half and on the renderer that wires them.
No GPU, no shader compile."""
from pathlib import Path
import re
import subprocess
import tempfile
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / '_support'))
from nr_sources import renderer_source, renderer_internal_header  # noqa: E402

here = Path(__file__).resolve().parent
root = here.parents[1]
opti = root / 'OptiScaler'
retoque = root / 'retoque'
precompile = retoque / 'shaders/dlssnr/precompile'


def strip_comments(text):
    return re.sub(r'//[^\n]*', '', text)


def squash(text):
    return re.sub(r'\s+', ' ', text)


header = (retoque / 'dlssnr/DlssNr_MotionProbe.h').read_text()
shader = (precompile / 'dlssnr_motion_probe.hlsl').read_text()
pass_h = (retoque / 'shaders/dlssnr/DlssNr_MotionProbe_Dx12.h').read_text()
pass_cpp = (retoque / 'shaders/dlssnr/DlssNr_MotionProbe_Dx12.cpp').read_text()
renderer = renderer_source()
internal = renderer_internal_header()

# The constants and the cbuffer are one ordered list of 4-byte scalars, padded to the 256 bytes of a view.
struct_body = pass_h[pass_h.index('struct alignas(256) Params'):]
struct_body = struct_body[struct_body.index('{') + 1:struct_body.index('};')]
cpp_fields = [n.lower() for t, n in re.findall(r'\b(uint32_t|float)\s+(\w+)\s*=', strip_comments(struct_body))]
cbuffer = shader[shader.index('cbuffer Params'):]
cbuffer = cbuffer[cbuffer.index('{') + 1:cbuffer.index('};')]
hlsl_fields = [n[1:].lower() for t, n in re.findall(r'\b(uint|float)\s+(\w+)\s*;', strip_comments(cbuffer))]
assert cpp_fields and cpp_fields == hlsl_fields, (cpp_fields, hlsl_fields)
assert all(t == 'uint32_t' for t, _ in re.findall(r'\b(uint32_t|float)\s+(\w+)\s*=', strip_comments(struct_body)))

# Every register the shader declares is a slot the root signature has.
srv = sorted(int(n) for n in re.findall(r'register\(t(\d+)\)', shader))
uav = sorted(int(n) for n in re.findall(r'register\(u(\d+)\)', shader))
assert srv == list(range(int(re.search(r'kSrvCount = (\d+)', pass_h).group(1))))
assert uav == list(range(int(re.search(r'kUavCount = (\d+)', pass_h).group(1))))

# The grid the shader writes is the grid the CPU reads, sampled where the CPU says, and the dispatch covers it.
grid = int(re.search(r'constexpr unsigned kGrid = (\d+);', header).group(1))
taps = int(re.search(r'constexpr unsigned kMotionTaps = (\d+);', header).group(1))
assert int(re.search(r'static const uint kGrid = (\d+);', shader).group(1)) == grid
assert int(re.search(r'static const uint kMotionTaps = (\d+);', shader).group(1)) == taps
threads = int(re.search(r'kThreads = (\d+);', pass_h).group(1))
assert f'[numthreads({threads}, {threads}, 1)]' in shader
assert 'cmdList->Dispatch((kGrid + kThreads - 1) / kThreads, (kGrid + kThreads - 1) / kThreads, 1);' in pass_cpp
for expression in ('((2u * cell + 1u) * extent) / (2u * kGrid)',
                   '(((cell * kMotionTaps + tap) * 2u + 1u) * extent) / (2u * kGrid * kMotionTaps)'):
    assert expression in shader and expression in header, expression
# Per cell: depth, slope, non-zero motion count, samples read -- the order FromGrid reads.
assert 'gGrid[id.xy] = float4(d, slope, float(moving), float(kMotionTaps * kMotionTaps));' in shader
# A NaN vector counts as non-zero: the ordered comparison, negated.
assert 'if (!(mv.x == 0.0 && mv.y == 0.0))' in shader
# Every read is inside the region: depth clamped to it, motion taps inside it by construction (the host test).
assert 'clamp(x, 0, int(gDepthWidth) - 1)' in shader and 'clamp(y, 0, int(gDepthHeight) - 1)' in shader
assert 'RWTexture2D<float4> gGrid' in shader and 'DXGI_FORMAT_R32G32B32A32_FLOAT' in pass_cpp

# Its bytecode is optional: included only behind __has_include, named as the build comment says, and committed with
# its .cso (nr-invariants holds the header to create_header.py over it).
assert '#if __has_include("precompile/DlssNr_MotionProbe_Shader.h")' in pass_cpp
assert 'DlssNr_MotionProbe_cso' in pass_cpp
assert ('create_header.py DlssNr_MotionProbe_Shader.cso DlssNr_MotionProbe_Shader.h DlssNr_MotionProbe_cso'
        in shader)
assert '-T cs_6_0 -E CSMain' in shader
assert (precompile / 'DlssNr_MotionProbe_Shader.cso').exists() == (precompile / 'DlssNr_MotionProbe_Shader.h').exists()

# Wiring. The probe is recorded in one place: inside Dispatch, only for the game's own guides, after both were made
# readable and the early return for an unreadable one, and before the guide resample reads them.
dispatch = renderer[renderer.index('bool DlssNr_Dx12::Dispatch('):]
dispatch = dispatch[:dispatch.index('\n}\n')]
assert renderer.count('UpdateMotionProbe(cmdList, device, depthIn, motionIn, probeShape,') == 1
call = dispatch.index('UpdateMotionProbe(cmdList, device, depthIn, motionIn, probeShape,')
gate = dispatch.rindex('if (frame.GameGuides && cfg.DlssNrZeroGuideReset.value_or_default())', 0, call)
assert dispatch.index('ID3D12Resource* motionIn = ReadableGuide(') < gate
assert dispatch.index('LOG_ERROR("DLSS-NR unavailable: depth {} | motion vectors {}"') < gate
assert call < dispatch.index('g_nr.guideMatch->Dispatch(cmdList, depthIn, motionIn,')
assert call < dispatch.index('PackGuides(cmdList, periphery, depthIn, motionIn,')
# The shape is the region the pass computed, depth and motion each.
probe_block = squash(dispatch[gate:call])
for field, value in (('depthWidth', 'guideWidth'), ('depthHeight', 'guideHeight'), ('depthBaseX', 'depthBaseX'),
                     ('depthBaseY', 'depthBaseY'), ('motionWidth', 'motionWidth'), ('motionHeight', 'motionHeight'),
                     ('motionBaseX', 'motionBaseX'), ('motionBaseY', 'motionBaseY'),
                     ('motionLowResolution', 'frame.MotionVectorsLowResolution')):
    assert f'probeShape.{field} = {value};' in probe_block, field

# The verdict is applied where the frame's reset is read, through the pure rule, before anything consumes the reset;
# and every reader of the frame's reset or its policy flag inside Dispatch reads the combined one.
rule = squash('DlssNr::MotionProbe::PolicyReset( MotionProbeDead(), frame.GameGuides, '
              'cfg.DlssNrZeroGuideReset.value_or_default(), frame.Reset)')
flat = squash(dispatch)
assert flat.count(rule) == 1, 'the reset rule is applied once in Dispatch'
assert 'const bool frameReset = frame.Reset || deadMotionReset;' in dispatch
assert 'const bool frameResetIsPolicy = deadMotionReset || frame.ResetIsPolicy;' in dispatch
assert dispatch.index('const bool resetBeforeFrameFlag = g_nr.reset;') < dispatch.index('const bool deadMotionReset')
assert dispatch.index('const bool deadMotionReset') < dispatch.index('if (frameReset)')
assert 'const bool stabilizerReset = frameResetIsPolicy ? resetBeforeFrameFlag : g_nr.reset;' in dispatch
assert not re.search(r'\bframe\.(Reset|ResetIsPolicy)\b', dispatch[dispatch.index('if (frameReset)'):]), \
    'a reader of the frame reset after the combined flags bypasses the dead-motion policy'
assert 'model history dropped on the game\'s all-zero motion vectors, as ZeroGuideReset' in squash(dispatch).replace('" "', '')

# Never waits: completion through the submission tracker, a busy slot skips the probe.
update = renderer[renderer.index('void UpdateMotionProbe('):]
update = update[:update.index('\n}\n')]
assert 'if (DlssNr::Submission::Completed(slot.usage))' in update
# A dropped list never completes: settled without completing frees the slot unread, or four drops end the probing.
settled = update.index('else if (DlssNr::Submission::Settled(slot.usage))')
assert update.index('if (DlssNr::Submission::Completed(slot.usage))') < settled
assert update[settled:settled + 120].count('slot.pending = false;') == 1
assert 'if (slot.pending)\n        return;' in update
assert 'DlssNr::Submission::Track(cmdList, slot.usage)' in update
assert 'WaitForSingleObject' not in update and 'SetEventOnCompletion' not in update
assert update.index('if (!DlssNr_MotionProbe_Dx12::Available())') < update.index('new DlssNr_MotionProbe_Dx12(')
assert renderer.count('new DlssNr_MotionProbe_Dx12(') == 1
# Readbacks are fed oldest first, each in the shape it was recorded with.
assert 's_motionProbe.detector.Add(slot.shape, s_motionProbe.probe)' in update
assert 'std::sort(done, done + doneCount' in update

# Parked through the retired list, freed with it, and at shutdown only once its readbacks settled.
assert 'delete retired.motionProbe;' in renderer and 'delete r.motionProbe;' in renderer
assert 'ParkNrMotionProbe(s_motionProbe.pass);' in renderer
assert renderer.count('ShutdownMotionProbe();') == 1
shutdown = renderer[renderer.index('void ShutdownMotionProbe()'):]
shutdown = shutdown[:shutdown.index('\n}\n')]
assert shutdown.index('DlssNr::Submission::Ready(slot.usage)') < shutdown.index('delete s_motionProbe.pass;')
assert 'DlssNr_MotionProbe_Dx12* motionProbe = nullptr;' in internal
# No new shared global: the session is file-local to the diagnostics unit.
assert 'MotionProbeSession s_motionProbe;' in renderer and 'g_motionProbe' not in renderer + internal

# Registered in the project, once each.
project = (opti / 'OptiScaler.vcxproj').read_text(encoding='utf-8-sig')
filters = (opti / 'OptiScaler.vcxproj.filters').read_text(encoding='utf-8-sig')
for item in ('<ClCompile Include="..\\retoque\\shaders\\dlssnr\\DlssNr_MotionProbe_Dx12.cpp" />',
             '<ClInclude Include="..\\retoque\\shaders\\dlssnr\\DlssNr_MotionProbe_Dx12.h" />',
             '<ClInclude Include="..\\retoque\\dlssnr\\DlssNr_MotionProbe.h" />'):
    assert project.count(item) == 1, item
for item in ('<ClCompile Include="..\\retoque\\shaders\\dlssnr\\DlssNr_MotionProbe_Dx12.cpp">',
             '<ClInclude Include="..\\retoque\\shaders\\dlssnr\\DlssNr_MotionProbe_Dx12.h">',
             '<ClInclude Include="..\\retoque\\dlssnr\\DlssNr_MotionProbe.h">'):
    assert filters.count(item) == 1, item

print('PASS: constants == cbuffer, registers == root signature, one grid and one sampling on both sides, NaN is not '
      'zero, optional bytecode, probed once in Dispatch on the game\'s readable guides before the resample, the reset '
      'rule wired where the frame reset is read, never waits, parked and freed, registered', flush=True)

with tempfile.TemporaryDirectory(prefix='optiscaler-nr-zero-motion-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer', '-I', str(retoque), str(here / 'test.cpp'), '-o', binary], check=True)
    subprocess.run([binary], check=True)

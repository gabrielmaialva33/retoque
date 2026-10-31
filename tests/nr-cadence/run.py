#!/usr/bin/env python3
"""Model cadence (docs/dlssnr/design/model-cadence.md): the production scheduler and the production per-pixel rules,
compiled under ASan/UBSan, plus source guards on the pass that runs them. No GPU, no NGX, no shader compile."""
from pathlib import Path
import re
import subprocess
import tempfile
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / '_support'))
from nr_sources import renderer_source  # noqa: E402  every DlssNr_Dx12*.cpp unit, in a fixed order

here = Path(__file__).resolve().parent
root = here.parents[1]
opti = root / 'OptiScaler'
retoque = root / 'retoque'


def read(relative):
    return (opti / relative).read_text()


def read_nr(relative):
    return (retoque / relative).read_text()


def strip_comments(text):
    return re.sub(r'//[^\n]*', '', text)


# --- Source guards -------------------------------------------------------------------------------------------

shader = read_nr('shaders/dlssnr/precompile/dlssnr_cadence.hlsl')
rule = read_nr('shaders/dlssnr/precompile/dlssnr_cadence_rule.h')
pass_h = read_nr('shaders/dlssnr/DlssNr_Cadence_Dx12.h')
pass_cpp = read_nr('shaders/dlssnr/DlssNr_Cadence_Dx12.cpp')
renderer = renderer_source()

# The constant struct and the cbuffer are one ordered list of 4-byte scalars (DEVELOPMENT.md rule 5, for this
# pass's own pair), and the struct is a multiple of 256 bytes, the size every constant-buffer view must have.
struct_body = pass_h[pass_h.index('struct alignas(256) Constants'):]
struct_body = struct_body[struct_body.index('{') + 1:struct_body.index('};')]
cpp_fields = [(t.replace('uint32_t', 'uint'), n.lower())
              for t, n in re.findall(r'\b(uint32_t|float)\s+(\w+)\s*=', strip_comments(struct_body))]
cbuffer = shader[shader.index('cbuffer Params'):]
cbuffer = cbuffer[cbuffer.index('{') + 1:cbuffer.index('};')]
hlsl_fields = [(t, n[1:].lower()) for t, n in re.findall(r'\b(uint|float)\s+(\w+)\s*;', strip_comments(cbuffer))]
assert cpp_fields and cpp_fields == hlsl_fields, (cpp_fields, hlsl_fields)
assert len(cpp_fields) * 4 <= 256

# Every register the shader declares is a slot the root signature has.
srv = sorted(int(n) for n in re.findall(r'register\(t(\d+)\)', shader))
uav = sorted(int(n) for n in re.findall(r'register\(u(\d+)\)', shader))
assert srv == list(range(int(re.search(r'kSrvCount = (\d+)', pass_h).group(1))))
assert uav == list(range(int(re.search(r'kUavCount = (\d+)', pass_h).group(1))))
assert pass_cpp.count('kNullSrv[]') == 1 and pass_cpp.count('kNullUav[]') == 1

# The smoothed model frame's fade stops before the fill where the carry accepts nothing (its value there is zero
# whatever the fill was); every other caller of the carry runs it whole. rule.cpp holds the values equal.
fade_body = rule[rule.index('CR_FN float3 CadenceFade('):]
fade_body = fade_body[:fade_body.index('\n}\n')]
assert 'CadenceCarryEdit(px, py, acc, P, true)' in fade_body, 'CadenceFade must ask the carry to stop where nothing is kept'
assert len(re.findall(r'CadenceCarryEdit\([^;]*,\s*true\)', strip_comments(rule) + strip_comments(shader))) == 1
carry_body = rule[rule.index('CR_FN CadenceCarry CadenceCarryEdit('):]
carry_body = carry_body[:carry_body.index('\n}\n')]
assert carry_body.index('if (stopWhenRejected && w <= 0.0f)') < carry_body.index('if (w < 0.5f)')

# The shader runs the rule header, and defines every hook the header reads.
assert '#include "dlssnr_cadence_rule.h"' in shader
needed = set(re.findall(r'\b(CR_[A-Z_]+)\b', strip_comments(rule)))
defined = set(re.findall(r'#define\s+(CR_[A-Z_]+)', shader))
assert needed <= defined, needed - defined

# Its bytecode is optional: included only behind __has_include, and named as the build comment says.
assert '#if __has_include("precompile/DlssNr_Cadence_Shader.h")' in pass_cpp
assert 'DlssNr_Cadence_cso' in pass_cpp
assert 'create_header.py DlssNr_Cadence_Shader.cso DlssNr_Cadence_Shader.h DlssNr_Cadence_cso' in shader
assert '-T cs_6_0 -E CSMain' in shader

# Off allocates nothing: the pass is built in one place, only where the scheduler says the cadence would run,
# and given back where it says the cadence is off or cannot exist.
assert renderer.count('new DlssNr_Cadence_Dx12(') == 1
released = renderer.index('if (Cadence::ReleasesSurfaces(cadenceRefusal))')
built = renderer.index('else if (Cadence::BuildsSurfaces(cadenceRefusal))')
assert released < renderer.index('ParkNrCadence(g_nr.cadence);', released) < built
assert built < renderer.index('new DlssNr_Cadence_Dx12(') < renderer.index('cadenceInputs.surfaces =', built)
# Off dispatches nothing: every dispatch hangs off a decision that only a running cadence makes.
assert renderer.count('g_nr.cadence->Carry(') == 1 and renderer.count('g_nr.cadence->ModelMotion(') == 1
assert renderer.count('g_nr.cadence->Record(') == 1
assert 'if (cadence.handChain && g_nr.cadence != nullptr)' in renderer
assert 'if (cadence.record && !carried && chain.completed == passSnapshot.Count && g_nr.cadence != nullptr)' in renderer
# The refusals the design note lists are what the renderer feeds the scheduler.
for field in ('driverProxy = useProxy', 'available = DlssNr_Cadence_Dx12::Available()',
              'presentsCounted = observedFrame != 0',
              'beforeUpscale = beforeUpscale', 'gameGuides = frame.GameGuides',
              'frameGeneration = FrameGenerationActive()', 'hold = holdFrame || g_nr.heldActive',
              'capture = g_capture.isActive()'):
    assert f'cadenceInputs.{field};' in renderer, field
# Only an upscaler's own guides count as the game's: a present host with nothing captured has none.
assert 'bool GameGuides = false;' in read_nr('shaders/dlssnr/DlssNr_Common.h')
# Set once, in GatherGuideFrame, the guide description every capture of the game's guides is made from: the after-
# upscale route's GatherFrame calls it, and so does the native D3D11 guide transport (CaptureBridgedGuides).
assert renderer.count('frame.GameGuides = true;') == 1
guide = renderer[renderer.index('DlssNrFrameInfo GatherGuideFrame('):]
assert guide.index('frame.GameGuides = true;') < guide.index('\n}\n')
gather = renderer[renderer.index('DlssNrFrameInfo GatherFrame('):]
assert gather.index('DlssNrFrameInfo frame = GatherGuideFrame(params);') < gather.index('\n}\n')
# Parked, never freed under the GPU, and freed with the rest.
assert 'delete retired.cadence;' in renderer and 'delete r.cadence;' in renderer
# Under peripheral compression the vectors the carry reads are the packed ones the model is handed, already in
# packed working pixels: their scale is 1 for both, never a second conversion from the game's units, which
# carries every edit too far. Both readers decide on the same flag, set before either reads it.
packed_set = renderer.index('guidesPacked = true;')
cadence_frame = renderer[renderer.index('DlssNr_Cadence_Dx12::Frame cadenceFrame {};'):]
cadence_frame = cadence_frame[:cadence_frame.index('cadenceFrame.scale =')]
for axis in 'XY':
    carry = re.search(rf'cadenceFrame\.mvToWork{axis}\s*=\s*([^;]*);', cadence_frame)
    model = re.search(rf'const float guideMvScale{axis}ToWork\s*=\s*([^;]*);', renderer)
    assert carry and re.match(r'guidesPacked\s*\?\s*1\.0f\s*:', carry.group(1)), f'cadence mvToWork{axis}'
    assert model and re.match(r'guidesPacked\s*\?\s*1\.0f\s*:', model.group(1)), f'model guideMvScale{axis}ToWork'
    assert packed_set < model.start() < renderer.index('DlssNr_Cadence_Dx12::Frame cadenceFrame {};')

# Smoothing (docs/dlssnr/design/cadence-smoothing.md). Its surfaces exist only on an object built with it, and the
# switch is part of what an object was built for and of the settings the scheduler watches, so a toggle rebuilds the
# surfaces and drops the stored edit together.
assert re.search(r'cadenceInputs\.smoothing\s*=\s*cfg\.DlssNrCadenceSmoothing\.value_or_default\(\);', renderer)
assert 'cadenceSettings = Cadence::Mix(cadenceSettings, cadenceInputs.smoothing ? 1u : 0u);' in renderer
assert renderer.count('g_nr.cadence->Fits(device, workWidth, workHeight, cadenceInputs.smoothing)') == 2
assert 'new DlssNr_Cadence_Dx12("DLSS-NR cadence", device, workWidth, workHeight, cadenceInputs.smoothing)' in renderer
ctor = pass_cpp[pass_cpp.index('DlssNr_Cadence_Dx12::DlssNr_Cadence_Dx12('):]
ctor = ctor[:ctor.index('\n}\n')]
smoothing_block = ctor[ctor.index('if (_smoothingAsked)'):]
for surface in ('_fade = CreateTexture(', '_shown = CreateTexture(', '_lightGrid = CreateTexture(',
                '_lightState = CreateTexture('):
    assert surface in smoothing_block and ctor.count(surface) == 1, surface
assert 'L"DLSS-NR cadence light state", kWrite)' in re.sub(r'\s+', ' ', smoothing_block).replace('( ', '(')
# The model frame fades before the record overwrites the edit the fade reads, and shows after it; the carried
# frames and the model frame are the only places smoothing is dispatched; the scheduler hears what really ran.
assert renderer.count('g_nr.cadence->Fade(') == 1 and renderer.count('g_nr.cadence->ShowFaded(') == 1
fade_at = renderer.index('g_nr.cadence->Fade(')
assert fade_at < renderer.index('cadenceRecorded = g_nr.cadence->Record(') < renderer.index('g_nr.cadence->ShowFaded(')
assert 'cadence.fade && cadenceChainHanded && cadence.record && !carried' in renderer
assert 'g_cadence.Recorded(cadenceFaded);' in renderer
assert 'g_cadence.Carried(cadence.limit && g_nr.cadence != nullptr && g_nr.cadence->Smoothing());' in renderer
assert 'cadenceSmooth.limitStep = Cadence::LimitStep(' in renderer
# The limiter's grid is one size on both sides, and its two group-wide modes run on whole groups only.
assert int(re.search(r'constexpr uint32_t kLightGrid = (\d+);', read_nr('dlssnr/DlssNr_Cadence.h')).group(1)) == \
    int(re.search(r'static const uint kLightGrid = (\d+);', shader).group(1))
assert 'Run(InCmdList, c, srvs, uavs, DlssNr::Cadence::kLightGrid, DlssNr::Cadence::kLightGrid);' in pass_cpp
assert 'Run(InCmdList, c, srvs, uavs, 1, 1);' in pass_cpp
csmain = shader[shader.index('void CSMain('):]
assert csmain.index('if (gMode == 6)') < csmain.index('PixelModes(id);')
assert csmain.index('if (gMode == 7)') < csmain.index('PixelModes(id);')
# At a weight of zero the carried frame is the carry it always was: the fade is neither bound nor read.
assert 'ID3D12Resource* const fade = fadeWeight > 0.0f ? _fade : nullptr;' in pass_cpp
assert 'if (weight > 0.0f)' in rule
# The light limiter leaves the interface alone: out of the light it measures, and out of the gain it applies.
assert 'const float keep = gHaveUi != 0u ? 1.0 - saturate(UiAt(px, py)) : 1.0;' in shader
assert "gOut2[uint2(3, 0)] * keep" in shader
assert 'const float scenery = gHaveUi != 0u ? 1.0 - saturate(gUi.Load(p).x) : 1.0;' in shader
assert 'g_nr.cadence->ShowFaded(cmdList, cadenceFrame, uiAlpha, cadenceSmooth, finalOutput)' in renderer
# A failed allocation of the smoothing surfaces leaves the carry running: the object still initialises.
assert 'carrying without smoothing' in pass_cpp

# Adaptive probes own four independent slots. CPU access requires an exact submission certificate; reuse requires that
# certificate or a settled usage (a list Reset unrun, or released), so one uncertifiable probe cannot latch interval 1.
assert 'Submission::TimingCertificate(probe.usage)' in pass_cpp
assert 'if (!certificate.reusable && !DlssNr::Submission::Settled(probe.usage))' in pass_cpp
assert 'certificate.accepted && !probe.ignored' in pass_cpp
assert 'if (!DlssNr::Submission::Track(cmd, probe.usage))' in pass_cpp
assert 'probe ? probe->mapped : _mappedConstants[slot]' in pass_cpp
assert 'adaptiveRequested && g_tracked && cadenceInputs.surfaces' in renderer
assert 'if (adaptiveActive)' in renderer and 'g_nr.cadence->Probe(' in renderer
assert 'kProbeSlots = 4' in pass_h and 'std::array<ProbeSlot, kProbeSlots> _probes' in pass_h
assert 'std::sort(results.begin(), results.end()' in pass_cpp
assert 'if (probe.pending)' in pass_h and 'probe.ignored = true;' in pass_h
assert 'sizeof(Constants) % D3D12_CONSTANT_BUFFER_DATA_PLACEMENT_ALIGNMENT == 0' in pass_cpp
assert 'probe.heap.GetCbvCPU(0)' in pass_cpp and 'sizeof(Constants)' in pass_cpp
assert 'probe ? probe->heap : _frameHeaps[slot]' in pass_cpp

assert 'return !slot.pending && InitProbe(slot);' in pass_cpp
assert 'for (const auto& sample : g_nr.cadence->PollProbes())' in renderer
assert 'g_cadenceAdaptive.Reset()' in renderer and 'InvalidateProbe()' in renderer
# A change of interval under adaptive cadence is logged with the sample that explains it.
assert 'PublishCadence(cadence, adaptiveActive ? &g_cadenceAdaptive.Last() : nullptr, cadenceEvaluation' in renderer
assert 'carry rejected {:.1f}%' in renderer
# Feedback age counts evaluations, not presents: under frame generation presents run ahead of evaluations.
assert 'const uint64_t cadenceEvaluation = ++g_cadenceEvaluations;' in renderer
assert 'g_cadenceAdaptive.Observe(sample, cadenceEvaluation, cadenceNowMs)' in renderer
assert 'g_cadenceAdaptive.Interval(cadenceInputs.requested, cadenceEvaluation, cadenceNowMs)' in renderer
assert 'cadence.chainStart, cadenceEvaluation, cadenceNowMs)' in renderer

# Registered in the project, once each.
project = read('OptiScaler.vcxproj')
filters = read('OptiScaler.vcxproj.filters')
for item in ('<ClCompile Include="..\\retoque\\shaders\\dlssnr\\DlssNr_Cadence_Dx12.cpp" />',
             '<ClInclude Include="..\\retoque\\shaders\\dlssnr\\DlssNr_Cadence_Dx12.h" />',
             '<ClInclude Include="..\\retoque\\dlssnr\\DlssNr_Cadence.h" />'):
    assert project.count(item) == 1, item
assert filters.count('<ClInclude Include="..\\retoque\\dlssnr\\DlssNr_Cadence.h">') == 1

# The ported rules carry their attribution, and the licence ships.
licence = (root / 'Licenses/OptimizerFps_LICENSE.txt').read_text()
assert 'Copyright (c) 2026 Yuri Grib (BeliyG3)' in licence and 'MIT License' in licence
assert 'BeliyG3' in rule and 'Licenses/OptimizerFps_LICENSE.txt' in rule

print('PASS: cadence constants == cbuffer, registers == root signature, every rule hook defined, optional bytecode, '
      'off allocates and dispatches nothing, refusals wired, packed vectors at scale 1, smoothing built and dispatched '
      'only with its switch and in order, registered, attributed',
      flush=True)

# --- The production headers, compiled ---------------------------------------------------------------------------

with tempfile.TemporaryDirectory(prefix='optiscaler-nr-cadence-') as directory:
    for name in ('scheduler', 'rule', 'adaptive'):
        binary = str(Path(directory) / name)
        subprocess.run(['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-I', str(retoque), '-I', str(opti),
                        str(here / f'{name}.cpp'), '-o', binary], check=True)
        subprocess.run([binary], check=True)

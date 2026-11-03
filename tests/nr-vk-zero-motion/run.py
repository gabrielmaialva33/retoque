#!/usr/bin/env python3
"""Zero-motion detection on the Vulkan after-upscale route (docs/dlssnr/design/zero-motion-detection.md, "Vulkan").

1. Source guards on the shader's VK_MODE block, the pass, the route's wiring in DlssNrFeature_Vk.cpp (every probe step
   behind ZeroGuideReset, in the V-06 order, no CPU wait, the three ends where the route ends its objects), the
   registry's Begin observers and the project.
2. cases.cpp, compiled with g++ under ASan/UBSan against the real Vulkan headers: the production DlssNr_VkMotionProbe.h
   with the real Ring, Registry and Detector, the production DlssNr_MotionProbe_Vk.cpp against recording fakes, and the
   route's probe functions and state extracted verbatim from DlssNrFeature_Vk.cpp.
3. Mutants: each planted violation must make a named case, or a guard, fail.

No GPU, no Wine, no shader compile."""
from pathlib import Path
import os
import re
import shlex
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
RETOQUE = ROOT / 'retoque'
VULKAN = ROOT / 'external/vulkan/include'

FEATURE = 'retoque/dlssnr/DlssNrFeature_Vk.cpp'
PURE = 'retoque/dlssnr/DlssNr_VkMotionProbe.h'
PASS_H = 'retoque/shaders/dlssnr/DlssNr_MotionProbe_Vk.h'
PASS_CPP = 'retoque/shaders/dlssnr/DlssNr_MotionProbe_Vk.cpp'
HLSL = 'retoque/shaders/dlssnr/precompile/dlssnr_motion_probe.hlsl'
PASS_D3D12 = 'retoque/shaders/dlssnr/DlssNr_MotionProbe_Dx12.h'
REGISTRY = 'retoque/dlssnr/DlssNr_VkRecording.h'
HOST = 'retoque/dlssnr/DlssNr_VkPresentHost.cpp'
BRIDGE = 'OptiScaler/hooks/VulkanwDx12_Hooks.cpp'
PROJECT = 'OptiScaler/OptiScaler.vcxproj'
FILTERS = 'OptiScaler/OptiScaler.vcxproj.filters'


def read(path):
    return (ROOT / path).read_text(encoding='utf-8-sig')


def code_mask(source):
    """Strings and comments blanked, offsets kept."""
    pattern = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', re.S)
    return pattern.sub(lambda m: ''.join('\n' if c == '\n' else ' ' for c in m[0]), source)


def extract(source, signature, through_semicolon=False):
    """The definition starting at `signature` (a regex anchored at a line start), braces matched outside strings and
    comments; a struct runs on to its closing semicolon."""
    masked = code_mask(source)
    matches = list(re.finditer(signature, masked, re.M))
    if len(matches) != 1:
        raise ValueError(f'expected one definition for {signature}, found {len(matches)}')
    start = matches[0].start()
    opening = masked.index('{', matches[0].end())
    depth = 0
    for i in range(opening, len(masked)):
        depth += (masked[i] == '{') - (masked[i] == '}')
        if depth == 0:
            end = masked.index(';', i) + 1 if through_semicolon else i + 1
            return source[start:end]
    raise ValueError(f'unclosed definition: {signature}')


ROUTE_FUNCTIONS = [
    (r'^bool\s+MotionProbeSlotReady\s*\(', False),
    (r'^uint32_t\s+MotionProbeReadyMask\s*\(', False),
    (r'^void\s+LoseMotionVerdict\s*\(', False),
    (r'^void\s+RefuseMotionProbe\s*\(', False),
    (r'^bool\s+CreateMotionProbeSlot\s*\(', False),
    (r'^const char\*\s+MakeMotionProbe\s*\(', False),
    (r'^struct\s+MotionProbePlan\b', True),
    (r'^MotionProbePlan\s+PrepareMotionProbe\s*\(', False),
    (r'^bool\s+MotionProbePolicyReset\s*\(', False),
    (r'^std::optional<VkMeterReadback::Reservation>\s+ReserveMotionProbe\s*\(', False),
    (r'^void\s+ConsumeMotionProbe\s*\(', False),
    (r'^void\s+ForgetMotionProbe\s*\(', False),
    (r'^void\s+AbandonMotionProbeReadbacks\s*\(', False),
    (r'^void\s+DestroyMotionProbe\s*\(', False),
]


def route_functions(feature):
    return [extract(feature, signature, semi) for signature, semi in ROUTE_FUNCTIONS]


def generate(feature):
    state = extract(feature, r'^\s*struct\s+MotionProbeState\b', True)
    text = (HERE / 'cases.cpp').read_text()
    return text.replace('// PRODUCTION_STATE', state).replace('// PRODUCTION_ROUTE', '\n\n'.join(route_functions(feature)))


def build_and_run(feature, directory, override=None, label='cases', pass_source=None):
    """Compile and run; returns (passed, output). `override` is a directory searched before retoque/ (a mutated
    header)."""
    unit = Path(directory) / f'{label}.cpp'
    unit.write_text(generate(feature))
    executable = Path(directory) / label
    includes = ([override, Path(override) / 'dlssnr'] if override else []) + [
        HERE / 'stubs', VULKAN, RETOQUE, RETOQUE / 'dlssnr', RETOQUE / 'shaders/dlssnr']
    compiler = shlex.split(os.environ.get('CXX', 'g++'))
    flags = ['-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-pthread', '-fsanitize=address,undefined',
             '-fno-omit-frame-pointer', '-fno-pie', '-no-pie',
             # The pass is written for MSVC's /W4, in the guide resample's style: { sType } initialisers.
             '-Wno-missing-field-initializers']
    built = subprocess.run(compiler + flags + ['-I' + str(p) for p in includes] +
                           [str(unit), str(pass_source or ROOT / PASS_CPP), '-o', str(executable)],
                           capture_output=True, text=True)
    if built.returncode != 0:
        return False, built.stdout + built.stderr
    env = dict(os.environ)
    env.setdefault('ASAN_OPTIONS', 'detect_leaks=1:abort_on_error=1')
    env.setdefault('UBSAN_OPTIONS', 'halt_on_error=1:print_stacktrace=1')
    ran = subprocess.run([str(executable)], env=env, capture_output=True, text=True, timeout=120)
    return ran.returncode == 0, ran.stdout + ran.stderr


# --- Guards ----------------------------------------------------------------------------------------------------------

def body_of(text, signature):
    try:
        return extract(text, signature)
    except ValueError:
        return ''


def block_after(text, needle):
    """The braced block that follows `needle` in `text`, braces included; empty when there is none."""
    start = text.find(needle)
    if start < 0 or text.find('{', start) < 0:
        return ''
    opening = text.index('{', start)
    depth = 0
    for i in range(opening, len(text)):
        depth += (text[i] == '{') - (text[i] == '}')
        if depth == 0:
            return text[opening:i + 1]
    return ''


def fields(text, tag, kind):
    start = text.index(tag)
    block = text[text.index('{', start) + 1:text.index('};', start)]
    return [n for _, n in re.findall(r'\b(' + kind + r')\s+(\w+)\s*;', re.sub(r'//[^\n]*', '', block))]


def guards(t):
    """Every guard over the texts `t` (path -> text); returns the failures."""
    failures = []

    def check(condition, message):
        if not condition:
            failures.append(message)

    hlsl, pass_h, pass_cpp, feature = t[HLSL], t[PASS_H], t[PASS_CPP], t[FEATURE]

    # The shader: the VK_MODE block before the D3D12 declarations, which stay as they were.
    check('#ifdef VK_MODE' in hlsl and hlsl.index('#ifdef VK_MODE') < hlsl.index('cbuffer Params : register(b0)'),
          'the VK_MODE block comes first, the D3D12 declarations are its #else')
    d3d12 = ('cbuffer Params : register(b0)\n{\n    uint gDepthBaseX;\n    uint gDepthBaseY;\n    uint gDepthWidth;\n'
             '    uint gDepthHeight;\n    uint gMotionBaseX;\n    uint gMotionBaseY;\n    uint gMotionWidth;\n'
             '    uint gMotionHeight;\n};\n\nTexture2D<float>    gDepth  : register(t0);\n'
             'Texture2D<float2>   gMotion : register(t1);\nRWTexture2D<float4> gGrid   : register(u0); // per cell: '
             '(depth, depth slope, non-zero motion samples, motion samples)\n#endif\n')
    check(('#else\n' + d3d12) in hlsl, 'the D3D12 declarations are textually what they were, under #else')
    check('[[vk::push_constant]] ProbeParams gParams;' in hlsl, 'the push-constant block')
    check('[[vk::binding(0, 0)]] Texture2D<float> gDepth;' in hlsl and
          '[[vk::binding(1, 0)]] Texture2D<float2> gMotion;' in hlsl and
          '[[vk::binding(2, 0)]] [[vk::image_format("rgba32f")]] RWTexture2D<float4> gGrid;' in hlsl,
          'bindings 0 and 1 sampled, 2 the rgba32f grid')
    push = fields(pass_h, 'struct Push', 'uint32_t')
    params = fields(hlsl, 'struct ProbeParams', 'uint')
    cbuffer = [n[1:] for n in fields(hlsl, 'cbuffer Params', 'uint')]
    check(push and push == params == cbuffer and len(push) == 8, f'Push {push}, ProbeParams {params}, Params {cbuffer}')
    d3d12_params = [n.lower() for _, n in re.findall(r'\b(uint32_t)\s+(\w+)\s*=',
                                                    t[PASS_D3D12][t[PASS_D3D12].index('struct alignas(256) Params'):]
                                                    .split('};')[0])]
    check([n.lower() for n in push] == d3d12_params, 'the Vulkan push block is the D3D12 pass\'s Params')
    check('-spirv -T cs_6_0 -E CSMain -O3 -Qstrip_debug -D VK_MODE dlssnr_motion_probe.hlsl -Fo '
          'DlssNr_MotionProbe_Shader_Vk.spv' in hlsl and
          'create_header.py DlssNr_MotionProbe_Shader_Vk.spv DlssNr_MotionProbe_Shader_Vk.h dlssnr_motion_probe_spv'
          in hlsl, 'the build comment names the SPIR-V command and its array')
    check('[numthreads(8, 8, 1)]' in hlsl and 'constexpr uint32_t kGroup = 8;' in pass_h,
          'one thread per cell in groups of 8x8')
    check('constexpr uint32_t kPushDescriptors = 3;' in pass_h, 'three pushed descriptors')
    check('vkCmdDispatch(cmd, kGroups, kGroups, 1);' in pass_cpp and
          'constexpr uint32_t kGroups = (kGrid + kGroup - 1) / kGroup;' in pass_h, 'the dispatch covers the grid')

    # The pass: optional bytecode, the pch first, no wait.
    check(re.match(r'\s*#include "pch.h"', pass_cpp) is not None, 'the pass includes pch.h first')
    check('#if __has_include("precompile/DlssNr_MotionProbe_Shader_Vk.h")' in pass_cpp and
          'dlssnr_motion_probe_spv' in pass_cpp, 'the SPIR-V is optional, through __has_include')
    check('VkRecording::kSimultaneousUseBit == VK_COMMAND_BUFFER_USAGE_SIMULTANEOUS_USE_BIT' in pass_cpp,
          'the registry\'s SIMULTANEOUS_USE bit is held to Vulkan\'s')
    check('#include <vulkan' not in t[PURE] and '#include <vulkan' not in t[REGISTRY],
          'the route state and the registry stay free of Vulkan types')

    # The route.
    try:
        probe_functions = '\n'.join(route_functions(feature))
    except ValueError as error:
        failures.append(str(error))
        probe_functions = ''
    waits = ('vkDeviceWaitIdle', 'vkWaitForFences', 'vkGetFenceStatus', 'VK_QUERY_RESULT_WAIT_BIT', 'vkQueueWaitIdle',
             'vkWaitSemaphores')
    for text, name in ((code_mask(probe_functions), 'the route\'s probe functions'), (code_mask(pass_cpp), 'the pass'),
                       (code_mask(t[PURE]), 'DlssNr_VkMotionProbe.h')):
        for wait in waits:
            check(wait not in text, f'{name} call {wait}')

    evaluate = body_of(feature, r'^void\s+EvaluateAfterUpscaleVk\s*\(')
    flat = re.sub(r'\s+', ' ', code_mask(evaluate))
    check('const bool zeroGuideReset = cfg.DlssNrZeroGuideReset.value_or_default();' in flat, 'the key, read once')
    for call in ('ConsumeMotionProbe(', 'PrepareMotionProbe(', 'MotionProbePolicyReset(', 'ReserveMotionProbe(',
                 'probe.pass.Record('):
        check(flat.count(call) == 1, f'{call} once in EvaluateAfterUpscaleVk')
    check('if (zeroGuideReset && g_vk.device == device) ConsumeMotionProbe(zeroGuideReset);' in flat,
          'readbacks consumed only with the key on, on the same device')
    check(re.search(r'if \(zeroGuideReset\) \{ probePlan = PrepareMotionProbe\(', flat) is not None,
          'the plan only with the key on')
    check('if (zeroGuideReset && probePlan.due) probeRecording = ReserveMotionProbe(*recording, probePlan);' in flat,
          'the reservation only with the key on and a due probe')
    check('if (probeRecording) {' in flat, 'the probe recorded only with a reservation')
    check('const bool deadMotionReset = MotionProbePolicyReset(device, probePlan, zeroGuideReset, gameAskedReset);'
          in flat, 'the rule takes the game\'s own reset')
    policy_block = block_after(flat, 'if (deadMotionReset)')
    check('g_vk.reset = true;' in policy_block and '++g_vk.motionProbe.policyResets' in policy_block and
          'resets' not in policy_block.replace('policyResets', ''), 'a policy reset has its own counter')
    game_block = block_after(flat, 'if (gameAskedReset)')
    check('++resets;' in game_block and 'dead' not in game_block.lower() and 'probe' not in game_block.lower(),
          'the game\'s counter never counts the probe')

    def at(needle):
        return flat.find(needle)

    order = [('ConsumeMeterReadback();', 'ConsumeMotionProbe('),
             ('g_vk.guideRefusal.active = false;', 'PrepareMotionProbe('),
             ('PrepareMotionProbe(', 'g_vk.pass->Reserve(cmdBuffer, 4)'),
             ('Pin(*recording, { g_vk.recording })', 'ReserveMotionProbe('),
             ('ReserveMeterReadback(*recording)', 'ReserveMotionProbe('),
             ('ReserveMotionProbe(', 'BeginRouteRecording();'),
             ('g_vk.meterFrames++;', 'probe.pass.Record('),
             ('probe.pass.Record(', 'g_fwd.evaluate('),
             ('deadMotionReset', 'g_fwd.evaluate(')]
    for first, then in order:
        check(at(first) >= 0 and at(then) >= 0 and at(first) < at(then), f'{first} before {then}')
    check('if (zeroGuideReset != g_vk.motionProbe.on) {' in flat and
          'g_vk.motionProbe.readbacks.Invalidate(); LoseMotionVerdict(' in flat,
          'switched on again, pending probes are discarded and the verdict starts over')

    shutdown = re.sub(r'\s+', ' ', code_mask(body_of(feature, r'^void\s+ShutdownVk\s*\(')))
    check('if (!deviceAlive) { ForgetMeterReadback(); ForgetMotionProbe();' in shutdown,
          'ShutdownVk(false) forgets the probe without a call')
    gone = re.sub(r'\s+', ' ', code_mask(body_of(feature, r'^void\s+AfterUpscaleDeviceGone\s*\(')))
    check('if (forgetOnly) { ForgetMeterReadback(); ForgetMotionProbe(); }' in gone,
          'a lost device forgets the probe without a call')
    check('g_vk.meterPending.Abandon(); AbandonMotionProbeReadbacks(); }' in gone and
          gone.find('AbandonMotionProbeReadbacks();') < gone.find('ForgetDevice('),
          'a drained device-gone drops the probe\'s readback metadata before the registry forgets the device')
    free = re.sub(r'\s+', ' ', code_mask(body_of(feature, r'^void\s+FreeRouteObjects\s*\(')))
    check(free.startswith('void FreeRouteObjects() { if (RouteRecordingBusy()) return;') and
          free.count('DestroyMotionProbe();') == 1, 'the probe freed only once no recording holds the generation')
    check(code_mask(feature).count('MakeMotionProbe()') == 2 and code_mask(feature).count('CreateMotionProbeSlot(') == 2,
          'the probe is made lazily, in PrepareMotionProbe only')
    begin_route = code_mask(body_of(feature, r'^void\s+BeginRouteRecording\s*\('))
    check('motionProbe' not in begin_route, 'the grids are not layout-tracked: each recording discards its own')

    # The registry's Begin observers pass the flags on.
    check('Recordings().Begin( (uint64_t) commandBuffer, DlssNr::VkRecording::ReuseOf(pBeginInfo != nullptr, '
          'pBeginInfo != nullptr ? pBeginInfo->flags : 0));' in re.sub(r'\s+', ' ', t[BRIDGE]),
          'the wDx12 Begin wrapper passes Begin\'s flags on')
    host = re.sub(r'\s+', ' ', t[HOST])
    check('Recordings().Begin( H(buffer), DlssNr::VkRecording::ReuseOf(info != nullptr, info != nullptr ? info->flags '
          ': 0));' in host, 'the present host\'s Begin observer passes the flags on')
    check('Recordings().Begin((uint64_t) buffer, DlssNr::VkRecording::ReuseOf(true, begin.flags));' in host,
          'the present host\'s own Begin says what it began with')

    # The project, once each.
    project, filters = t[PROJECT], t[FILTERS]
    for item in ('<ClCompile Include="..\\retoque\\shaders\\dlssnr\\DlssNr_MotionProbe_Vk.cpp" />',
                 '<ClInclude Include="..\\retoque\\shaders\\dlssnr\\DlssNr_MotionProbe_Vk.h" />',
                 '<ClInclude Include="..\\retoque\\dlssnr\\DlssNr_VkMotionProbe.h" />',
                 '<ClInclude Include="..\\retoque\\shaders\\dlssnr\\precompile\\DlssNr_MotionProbe_Shader_Vk.h" />'):
        check(project.count(item) == 1, f'registered once: {item}')
    for item in ('<ClCompile Include="..\\retoque\\shaders\\dlssnr\\DlssNr_MotionProbe_Vk.cpp">',
                 '<ClInclude Include="..\\retoque\\shaders\\dlssnr\\DlssNr_MotionProbe_Vk.h">',
                 '<ClInclude Include="..\\retoque\\dlssnr\\DlssNr_VkMotionProbe.h">'):
        check(filters.count(item) == 1, f'filtered once: {item}')
    return failures


def texts():
    return {path: read(path) for path in (FEATURE, PURE, PASS_H, PASS_CPP, HLSL, PASS_D3D12, REGISTRY, HOST, BRIDGE,
                                          PROJECT, FILTERS)}


def mutate(text, old, new):
    if text.count(old) != 1:
        raise ValueError(f'mutant anchor not found exactly once: {old!r}')
    return text.replace(old, new)


# Each must make its case fail: (name, file, old, new, the case).
BEHAVIOUR_MUTANTS = [
    ('no sentinel', PURE, 'std::memset(mapped[reservation->slot], 0, kGridBytes);', '(void) mapped;',
     'sentinel-over-old-valid-grid'),
    ('fed in slot order', PURE,
     'std::sort(retired, retired + count, [](const auto& a, const auto& b) { return a.serial < b.serial; });', '',
     'out-of-order-retirement'),
    ('no provenance', PURE, '_device == device &&', '', 'dead-on-a-stable-failure-on-b-no-policy-reset'),
    ('simultaneous admitted', PURE,
     '''    case VkRecording::Reuse::Simultaneous:
        return "the game's''', '''    case VkRecording::Reuse::Simultaneous:
        return nullptr;
        return "the game's''', 'recording-refusals'),
    ('refusal keeps the verdict', FEATURE, '    LoseMotionVerdict("the probe cannot observe these frames");\n', '',
     'refusal-drops-the-verdict'),
    ('staleness keeps the verdict', FEATURE,
     '        LoseMotionVerdict("no probe has been read back for a while: the game holds every readback");\n',
     '        (void) 0;\n', 'saturation-resubmission-staleness'),
    ('lose keeps pending probes', PURE, '        _pending.Invalidate();\n        _detector.Reset();', '        _detector.Reset();',
     'pending-probes-do-not-outlive-the-verdict'),
    ('set layout output stored on failure', PASS_CPP,
     'const VkResult setMade = vkCreateDescriptorSetLayout(device, &setInfo, nullptr, &setLayout);\n    if (setMade == VK_SUCCESS)\n        _setLayout = setLayout;',
     'const VkResult setMade = vkCreateDescriptorSetLayout(device, &setInfo, nullptr, &setLayout);\n    _setLayout = setLayout;',
     'layout-failure-cleans-only-what-was-made'),
    ('pipeline layout output stored on failure', PASS_CPP,
     'const VkResult layoutMade = vkCreatePipelineLayout(device, &layoutInfo, nullptr, &layout);\n    if (layoutMade == VK_SUCCESS)\n        _layout = layout;',
     'const VkResult layoutMade = vkCreatePipelineLayout(device, &layoutInfo, nullptr, &layout);\n    _layout = layout;',
     'layout-failure-cleans-only-what-was-made'),
    ('device change keeps the verdict', FEATURE,
     '    LoseMotionVerdict("the device it was read on is gone");\n', '',
     'dead-on-a-stable-failure-on-b-no-policy-reset'),
]

# Each must make a guard fail: (name, file, old, new).
GUARD_MUTANTS = [
    ('consume without the key', FEATURE, 'if (zeroGuideReset && g_vk.device == device)\n        ConsumeMotionProbe(',
     'if (g_vk.device == device)\n        ConsumeMotionProbe('),
    ('reserve after the first command', FEATURE,
     '        probeRecording = ReserveMotionProbe(*recording, probePlan);\n\n    BeginRouteRecording();\n',
     '        (void) 0;\n\n    BeginRouteRecording();\n    if (zeroGuideReset && probePlan.due)\n'
     '        probeRecording = ReserveMotionProbe(*recording, probePlan);\n'),
    ('a wait in consume', FEATURE, '    auto& probe = g_vk.motionProbe;\n    probe.readbacks.Consume(',
     '    auto& probe = g_vk.motionProbe;\n    vkDeviceWaitIdle(g_vk.device);\n    probe.readbacks.Consume('),
    ('shutdown keeps the probe', FEATURE, '        ForgetMeterReadback();\n        ForgetMotionProbe();\n        // Only',
     '        ForgetMeterReadback();\n        // Only'),
    ('push fields reordered', PASS_H, '    uint32_t DepthBaseX;\n    uint32_t DepthBaseY;\n',
     '    uint32_t DepthBaseY;\n    uint32_t DepthBaseX;\n'),
    ('policy in the game\'s counter', FEATURE, 'const uint64_t count = ++g_vk.motionProbe.policyResets;',
     'const uint64_t count = ++resets;'),
    ('prepare without the key', FEATURE, '    if (zeroGuideReset)\n    {\n        probePlan = PrepareMotionProbe(',
     '    {\n        probePlan = PrepareMotionProbe('),
]


def main():
    failed = []

    baseline = texts()
    problems = guards(baseline)
    for problem in problems:
        print(f'FAIL: guard: {problem}')
    failed += problems
    if not problems:
        print('PASS: source guards (shader VK_MODE block and D3D12 text, push block == ProbeParams == Params, '
              'bindings, optional SPIR-V, every probe step behind ZeroGuideReset in the V-06 order, no CPU wait, '
              'the three ends, Begin flags passed on, project)')

    for name, path, old, new in GUARD_MUTANTS:
        mutated = dict(baseline)
        mutated[path] = mutate(baseline[path], old, new)
        if guards(mutated):
            print(f'PASS: guard mutant caught: {name}')
        else:
            print(f'FAIL: guard mutant survived: {name}')
            failed.append(name)

    with tempfile.TemporaryDirectory(prefix='nr-vk-zero-motion-') as directory:
        passed, output = build_and_run(baseline[FEATURE], directory)
        print(output, end='')
        if not passed:
            failed.append('cases')

        for index, (name, path, old, new, case) in enumerate(BEHAVIOUR_MUTANTS):
            feature = baseline[FEATURE]
            override = None
            pass_source = None
            if path == FEATURE:
                feature = mutate(feature, old, new)
            elif path == PASS_CPP:
                pass_source = Path(directory) / f'pass-{index}.cpp'
                pass_source.write_text(mutate(baseline[path], old, new))
            else:
                override = Path(directory) / f'override-{index}'
                (override / 'dlssnr').mkdir(parents=True)
                (override / 'dlssnr' / Path(path).name).write_text(mutate(baseline[path], old, new))
            survived, output = build_and_run(feature, directory, override, f'mutant-{index}', pass_source)
            if f'FAIL: {case}' in output and not survived:
                print(f'PASS: mutant caught: {name} ({case} fails)')
            else:
                print(f'FAIL: mutant survived: {name} (expected {case} to fail)\n{output}')
                failed.append(name)

    if failed:
        print(f'FAIL: {len(failed)} problem(s)')
        return 1
    print('PASS: Vulkan zero-motion probe: guards, cases and every mutant caught (host only; no GPU)')
    return 0


if __name__ == '__main__':
    sys.exit(main())

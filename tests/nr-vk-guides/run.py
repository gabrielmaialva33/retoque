#!/usr/bin/env python3
"""The Vulkan route's guides below the frame's size (docs/dlssnr/design/reduced-scale-guides-vk.md).

Compiles cases.cpp, the production DlssNr_GuideMatch.h and DlssNr_GuideMatch_Vk.h against the real Vulkan headers
(g++ and clang++, ASan/UBSan, -Werror): what the model is handed (matched, the game's regions, or the frame refused)
against the D3D12 route's own arithmetic, the game's views the resample may read, its outputs, push block and
dispatch. Then source guards on the route (DlssNrFeature_Vk.cpp), the pass (DlssNr_GuideMatch_Vk.cpp), the shader and
the project: the decision taken before the frame records anything, the evaluate's guides and scale from the plan,
nothing new at or above native, frees only after a drain and abandon with a lost device. No Vulkan device:
tests/guide-match-vk runs the pass on one."""
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

here = Path(__file__).resolve().parent
root = here.parents[1]
retoque = root / 'retoque'
vulkan = root / 'external/vulkan/include'

failures = []


def check(condition, message):
    if not condition:
        failures.append(message)
    return condition


def read(path):
    p = root / path
    return p.read_text(encoding='utf-8-sig') if p.exists() else ''


def function(text, signature):
    """The body of the one definition that starts with `signature`, braces matched outside strings and comments;
    empty when there is not exactly one."""
    if text.count(signature) != 1:
        return ''
    i = text.index('{', text.index(signature))
    depth, j = 0, i
    while j < len(text):
        c = text[j]
        if text.startswith('//', j):
            j = text.index('\n', j)
            continue
        if c in '"\'':
            j += 1
            while text[j] != c:
                j += 2 if text[j] == '\\' else 1
        elif c == '{':
            depth += 1
        elif c == '}':
            depth -= 1
            if depth == 0:
                return text[i:j + 1]
        j += 1
    return ''


def code(text):
    """Without comments and string literals, for guards about what the code calls."""
    text = re.sub(r'//[^\n]*', '', text)
    return re.sub(r'"(?:\\.|[^"\\])*"', '""', text)


def flat(text):
    return re.sub(r'\s+', ' ', code(text))


def first(text, *needles):
    found = [text.index(n) for n in needles if n in text]
    return min(found) if found else -1


# --- The route ------------------------------------------------------------------------------------------------------

feature = read('retoque/dlssnr/DlssNrFeature_Vk.cpp')
route = function(feature, 'void EvaluateAfterUpscaleVk(')
check(route, 'EvaluateAfterUpscaleVk not found once in DlssNrFeature_Vk.cpp')
rc = flat(route)

check('no Vulkan resample yet' not in feature,
      'the route still says it has no Vulkan resample: frame-size guides for a working-size colour')

# One decision a frame, from the shared rules, before any of the frame's commands: the first timestamp, barrier or
# dispatch. A refusal records nothing of the pass and starts the model's history over when it runs again.
decide = rc.find('GuideMatch::Choose(')
check(decide >= 0 and rc.count('GuideMatch::Choose(') == 1, 'the route does not decide its guides with GuideMatch::Choose')
recording = first(rc, 'vkCmdResetQueryPool(', 'vkCmdWriteTimestamp(', 'Transition(cmdBuffer', '->Dispatch(cmdBuffer',
                  'g_vk.guideMatch.Record(')
check(decide >= 0 and recording >= 0 and decide < recording,
      'the guide decision comes after the frame has recorded something (a refusal must record nothing)')
check(re.search(r'belowNative && DlssNr::GuideMatch::Wanted\(cfg\.DlssNrMatchGuides\.value_or_default\(\), true, '
                r'workWidth, workHeight, guideWidth, guideHeight, motionWidth, motionHeight\)', rc) is not None,
      'the route does not ask GuideMatch::Wanted, below native only, with MatchGuides and the regions it would hand '
      'over')
check('const bool belowNative = workWidth < width || workHeight < height;' in rc,
      'the resample is not limited to below native (at or above it the route keeps its old guides)')
prepare_fn = flat(function(feature, 'const char* PrepareGuideMatch('))
check(prepare_fn.find('DlssNr::GuideMatch::Vk::RegionRefusal(depthRegion, motionRegion)') ==
      first(prepare_fn, 'DlssNr::GuideMatch::Vk::RegionRefusal(', 'guideMatchDeviceFailure', 'Create(', 'CreateImage('),
      'PrepareGuideMatch does not refuse empty regions before anything else')
refused = re.search(r'if \(guides == DlssNr::GuideMatch::Guides::Refused\) \{(.*?)\}', rc)
check(refused is not None and 'g_vk.reset = true;' in refused.group(1) and 'return;' in refused.group(1),
      'a refused frame does not return with the history reset set')

# What the model is handed: the plan, all of it.
evaluate = rc[rc.find('g_fwd.evaluate('):]
evaluate = evaluate[:evaluate.find(');') + 2]
check(evaluate.startswith('g_fwd.evaluate('), 'the route\'s evaluate call not found')
check('depthForModel, motionForModel' in evaluate, 'the evaluate is not handed the planned depth and motion')
check('handed.depth.width, handed.depth.height, handed.motion.width, handed.motion.height, handed.depth.baseX, '
      'handed.depth.baseY, handed.motion.baseX, handed.motion.baseY' in evaluate,
      'the evaluate\'s guide sizes and bases are not the plan\'s')
check(evaluate.rstrip(');').rstrip().endswith('handed.scaleX, handed.scaleY'),
      'the evaluate\'s motion-vector scale is not the plan\'s')
check(rc.count('GuideMatch::ForModel(') == 1, 'the plan is not made once by GuideMatch::ForModel')
check('&g_vk.depthMatched.ngx' in rc and '&g_vk.motionMatched.ngx' in rc,
      'the matched pair is not what the model is handed when matched')
record = rc.find('g_vk.guideMatch.Record(')
check(record >= 0 and record < rc.find('g_fwd.evaluate('), 'the resample is not recorded before the model')
check('VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL' in rc[record:record + 400] if record >= 0 else False,
      'the game\'s guides are not bound in SHADER_READ_ONLY_OPTIMAL, the layout NGX takes them in')

# Lifetime: the images per working size, destroyed in the resize block after its drain; the pass and the images freed
# only in FreeRouteObjects; abandoned with a lost device.
resize = rc[rc.find('if (g_vk.width != width || g_vk.height != height'):]
resize = resize[:resize.find('g_vk.reset = true; }') + 1]
# V-02: the drain's result decides (lost: abandon; any other failure: free nothing), and the guides are never destroyed
# there: the pair of the last size is kept across this resize, the one before it goes to DeferredRelease.
drain = resize.find('const VkResult idle = vkDeviceWaitIdle(g_vk.device);')
check(drain >= 0 and re.search(r'if \(idle == VK_ERROR_DEVICE_LOST\) \{[^}]*ShutdownVk\(false\);[^}]*return; \}',
                               resize) is not None and
      re.search(r'if \(idle != VK_SUCCESS\) \{.*?return; \}', resize) is not None,
      'the resize block does not act on its drain\'s result (abandon when lost, free nothing on another failure)')
check(drain >= 0 and drain < resize.find('RetireImage(g_vk.depthRetired);') <
      resize.find('g_vk.depthRetired = g_vk.depthMatched;') and 'DestroyImage(g_vk.depthMatched)' not in resize and
      'DestroyImage(g_vk.motionMatched)' not in resize,
      'the resize does not keep the last size\'s guides alive and retire the older pair through DeferredRelease')
retire = flat(function(feature, 'void RetireImage(OwnedImage& img)'))
check('DeferredRelease::Enqueue(' in retire and '(uint64_t) device' in retire and 'img = OwnedImage {};' in retire,
      'RetireImage does not hand the image to DeferredRelease, owned by its device')
free_route = flat(function(feature, 'void FreeRouteObjects()'))
check(all(f'RetireImage(g_vk.{n});' in free_route for n in ('depthMatched', 'motionMatched', 'depthRetired',
                                                             'motionRetired')) and
      'retired.Destroy();' in free_route and 'g_vk.guideMatch.Abandon();' in free_route,
      'FreeRouteObjects does not retire the guide resample through DeferredRelease')
shutdown = flat(function(feature, 'void ShutdownVk(bool deviceAlive)'))
lost = shutdown[:shutdown.find('return; }')]
check('g_vk.guideMatch.Abandon();' in lost and all(f'g_vk.{n} = OwnedImage {{}};' in lost for n in (
      'depthMatched', 'motionMatched', 'depthRetired', 'motionRetired')),
      'ShutdownVk(false) does not abandon the guide resample')
check(re.search(r'vkDeviceWaitIdle\(g_vk.device\) != VK_SUCCESS\) \{ ShutdownVk\(false\); return; \}', shutdown)
      is not None, 'ShutdownVk(true) frees after a drain that did not succeed')
check('g_vk.guideMatch.Destroy()' not in code(feature), 'the route destroys the guide pass directly')
prepare_fn = flat(function(feature, 'const char* PrepareGuideMatch('))
check(prepare_fn.count('&lost)') == 2 and 'CreateImage(g_vk.depthMatched' not in prepare_fn,
      'PrepareGuideMatch does not tell CreateImageOn to report a lost device (abandon, not destroy)')

# V-03: a transient failure is tried again after a backoff, and a refusal that stands stops asking for the model.
check('GuideMatchTransient(' in prepare_fn and 'g_vk.guideMatchRetryMs' in prepare_fn,
      'a transient failure is not retried with a backoff')
stand = rc.find('GuideRefusalStandsDown(device, width, height, workWidth, workHeight, *depth, *motion)')
check(0 <= stand < rc.find('ClaimModelForAfterUpscale(device)'),
      'a refusing route asks for the model before it stands down')
check('NoteGuideRefusal(' in (refused.group(1) if refused else ''), 'a refusal is not recorded for the stand-down')

# V-04 and V-05: the geometry's state reaches the resample; the vectors' reference is the render size.
check('ResolveRenderSize(params)' in rc and 'geometry.source' in rc, 'the route ignores the geometry\'s state')
check('DlssNr::GuideMatch::GeometryRefusal(' in prepare_fn, 'PrepareGuideMatch does not refuse partial or stale geometry')
# V-05 is 242f1a05's (cherry-picked from claude/nr-metamorphic-geometry-20261023, pinned by tests/nr-geometry-metamorphic):
# the render size, when known, is the low-resolution vectors' region and their scale's reference.
check('lowResolutionMotion ? renderReferenceWidth : width' in rc and
      'MotionReference(lowResolutionMotion, renderReferenceWidth, width)' in rc,
      'the low-resolution vectors\' region and scale are not measured against the render size (V-05)')

# Nothing new at or above native: everything of the resample's is behind the wanted test.
prepare = rc.find('PrepareGuideMatch(')
check(prepare >= 0 and re.search(r'wanted \? PrepareGuideMatch\(', rc) is not None,
      'the resample is prepared other than when it is wanted')
check('g_vk.guideMatch.Create(' not in rc, 'the route makes the guide pass outside PrepareGuideMatch')

# --- The pass -------------------------------------------------------------------------------------------------------

pass_cpp = read('retoque/shaders/dlssnr/DlssNr_GuideMatch_Vk.cpp')
pass_h = read('retoque/shaders/dlssnr/DlssNr_GuideMatch_Vk.h')
check(pass_cpp and pass_h, 'retoque/shaders/dlssnr/DlssNr_GuideMatch_Vk.{h,cpp} not found')
if pass_cpp:
    pc = code(pass_cpp)
    check(re.match(r'\s*#include "pch.h"', pass_cpp) is not None, 'DlssNr_GuideMatch_Vk.cpp does not include pch.h first')
    check('#include "precompile/DlssNr_Guides_Shader_Vk.h"' in pass_cpp and '__has_include' not in pass_cpp,
          'the Vulkan bytecode is not included unconditionally (a missing header must fail the build)')
    check('~DlssNr_GuideMatch_Vk' not in pc, 'the pass has a destructor; freeing is Destroy()\'s, after a drain')
    check('LOG_' not in pc, 'the pass logs; it returns its reasons and the route says them')
    destroy = flat(function(pass_cpp, 'void DlssNr_GuideMatch_Vk::Destroy()'))
    for call in ('vkDestroyPipeline(', 'vkDestroyPipelineLayout(', 'vkDestroyDescriptorSetLayout('):
        check(call in destroy, f'Destroy() does not call {call}')
    # V-01: descriptors pushed into the command buffer; no set of the pass's own to outlive a recording.
    for gone in ('vkUpdateDescriptorSets(', 'vkAllocateDescriptorSets(', 'vkCreateDescriptorPool(',
                 'vkCmdBindDescriptorSets('):
        check(gone not in pc, f'the pass still calls {gone}')
    check('vkGetDeviceProcAddr(device, "vkCmdPushDescriptorSetKHR")' in pass_cpp and
          'VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR' in pass_cpp,
          'the pass does not push its descriptors (VK_KHR_push_descriptor, a push-descriptor set layout)')
    abandon = flat(function(pass_cpp, 'void DlssNr_GuideMatch_Vk::Abandon()'))
    check(abandon and 'vk' not in abandon.replace('Vk', ''), 'Abandon() calls Vulkan')
    check('range.size = sizeof(DlssNr::GuideMatch::Vk::Push);' in flat(pass_cpp) and
          'range.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;' in flat(pass_cpp) and 'range.offset = 0;' in flat(pass_cpp),
          'the push-constant range is not the push block, compute stage, offset 0')
    for binding, kind in ((0, 'SAMPLED_IMAGE'), (1, 'SAMPLED_IMAGE'), (2, 'STORAGE_IMAGE'), (3, 'STORAGE_IMAGE')):
        check(f'{{ {binding}, VK_DESCRIPTOR_TYPE_{kind}, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr }}' in flat(pass_cpp),
              f'binding {binding} is not a {kind}')
    rec = flat(function(pass_cpp, 'void DlssNr_GuideMatch_Vk::Record('))
    check('_push(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, _layout, 0, DlssNr::GuideMatch::Vk::kPushDescriptors, writes);'
          in rec, 'Record does not push its four descriptors')
    check('vkCmdPushConstants(' in rec and 'vkCmdDispatch(cmd, DlssNr::GuideMatch::Vk::Groups(outWidth), '
          'DlssNr::GuideMatch::Vk::Groups(outHeight), 1);' in rec, 'Record does not push its block and dispatch '
          'ceil(work / 8) groups')
    check('oldLayout = VK_IMAGE_LAYOUT_UNDEFINED' in rec and 'newLayout = VK_IMAGE_LAYOUT_GENERAL' in rec,
          'Record does not move the outputs from UNDEFINED to GENERAL before writing them')
    pre = rec.find('VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT')
    post = rec.find('VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT')
    dispatch = rec.find('vkCmdDispatch(')
    check(0 <= pre < dispatch < post, 'Record\'s barriers are not ALL_COMMANDS -> compute before the dispatch and '
          'compute -> ALL_COMMANDS after it')

# --- The shader, the bytecode, the project ----------------------------------------------------------------------------

hlsl = read('retoque/shaders/dlssnr/precompile/dlssnr_guides.hlsl')
check('[[vk::push_constant]]' in hlsl, 'dlssnr_guides.hlsl has no push-constant block under VK_MODE')
for binding in range(4):
    check(f'[[vk::binding({binding}, 0)]]' in hlsl, f'dlssnr_guides.hlsl binds nothing at {binding} under VK_MODE')
check('[[vk::image_format("r32f")]]' in hlsl and '[[vk::image_format("rg32f")]]' in hlsl,
      'the storage images\' formats are not stated in the shader')
check('cbuffer Params : register(b0)' in hlsl, 'the D3D12 cbuffer is gone from dlssnr_guides.hlsl')
precompile = retoque / 'shaders/dlssnr/precompile'
check((precompile / 'DlssNr_Guides_Shader_Vk.spv').exists() and (precompile / 'DlssNr_Guides_Shader_Vk.h').exists(),
      'the Vulkan bytecode (DlssNr_Guides_Shader_Vk.spv and .h) is not committed')

project = read('OptiScaler/OptiScaler.vcxproj')
for item in ('<ClCompile Include="..\\retoque\\shaders\\dlssnr\\DlssNr_GuideMatch_Vk.cpp" />',
             '<ClInclude Include="..\\retoque\\shaders\\dlssnr\\DlssNr_GuideMatch_Vk.h" />',
             '<ClInclude Include="..\\retoque\\shaders\\dlssnr\\precompile\\DlssNr_Guides_Shader_Vk.h" />'):
    check(item in project, f'OptiScaler.vcxproj does not list {item}')

# --- The cases ------------------------------------------------------------------------------------------------------

compilers = [c for c in ('g++', 'clang++') if shutil.which(c)]
check(compilers, 'neither g++ nor clang++ is available')
check((vulkan / 'vulkan/vulkan.h').exists(), 'external/vulkan is not checked out')
with tempfile.TemporaryDirectory(prefix='retoque-nr-vk-guides-') as directory:
    for compiler in compilers:
        binary = Path(directory) / f'cases-{compiler}'
        built = subprocess.run([compiler, '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                                '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-sanitize-recover=all',
                                '-I', str(vulkan), '-I', str(retoque), str(here / 'cases.cpp'), '-o', str(binary)],
                               capture_output=True, text=True)
        if not check(built.returncode == 0, f'cases.cpp does not compile with {compiler}'):
            print(built.stderr[-3000:])
            continue
        ran = subprocess.run([str(binary)], capture_output=True, text=True)
        print(f'{compiler}: {ran.stdout.strip()}')
        check(ran.returncode == 0, f'cases.cpp fails under {compiler}: {ran.stdout[-2000:]}{ran.stderr[-2000:]}')

for failure in failures:
    print('FAIL:', failure)
if failures:
    sys.exit(1)
print('PASS: the Vulkan route decides matched, game or refused before recording, hands the model the plan, frees '
      'the resample only after a drain and abandons it with a lost device; the pass, its shader interface and bytecode '
      'are in the build')

"""Guard the evaluate-window placement and resource boundaries; no GPU/opaque-NGX proof."""
from pathlib import Path
import hashlib
import re
import subprocess

ROOT = Path(__file__).resolve().parents[2]
BODIES = (
    ('retoque/dlssnr/DlssNrFeature_Vk.cpp', 'EvaluateAfterUpscaleVk',
     # The post body as the native Vulkan pair capture left it (it records its snapshots there); this branch
     # does not change it.
     'be9224959f59b3f8792db03a9f958c0203980177c8dec074f68ae70362e0ba3e'),
)


def body(source, name):
    match = re.search(r'^.*\b'+re.escape(name)+r'\(', source, re.M)
    assert match, name
    opening = source.index('{', match.end())
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[match.start():end]


def run():
    for file, name, expected in BODIES:
        source = (ROOT/file).read_text()
        original = subprocess.check_output(['git', 'show', 'HEAD:'+file], cwd=ROOT, text=True)
        current = body(source, name)
        assert current == body(original, name), f'evaluate behavior changed: {name}'
        assert hashlib.sha256(current.encode()).hexdigest() == expected, name
        # Self-check the guard with an actual executable mutation of each protected body.
        mutant = current.replace('{', '{ return {};', 1)
        assert hashlib.sha256(mutant.encode()).hexdigest() != expected
    adapter = (ROOT/'retoque/dlssnr/DlssNr_VkPreRuntime.cpp').read_text()
    assert not re.search(r'Recordings\(\)\.(Allocate|Begin|End|Reset|Free|Pin|ForgetDevice)\(', adapter)
    assert 'ScopedPreUpscaleVk' not in adapter
    implementation = (ROOT/'retoque/dlssnr/DlssNrFeature_Vk.cpp').read_text()
    inputs = (ROOT/'OptiScaler/inputs/NVNGX_DLSS_Vk.cpp').read_text()
    header = (ROOT/'retoque/dlssnr/DlssNrFeature_Vk.h').read_text()
    validate(implementation, inputs, header)
    mutations = (
        ('NR in destructor', 'implementation', '~ScopedPreUpscaleVk()\n{', '~ScopedPreUpscaleVk()\n{ vkCmdDispatch();'),
        ('NR after SR', 'inputs', 'pre->Complete();', 'RecordPreUpscaleVk();'),
        ('completion records', 'header', '_colour.Restore(); }', '_colour.Restore(); vkCmdDispatch(); }'),
        ('post fallback', 'inputs', '(!pre || pre->AllowPost())', 'true'),
        ('unscoped helper caller', 'implementation', 'return true;\n}\n\nnamespace\n{\nstd::atomic_flag', 'return RecordPreUpscaleVk();\n}\n\nnamespace\n{\nstd::atomic_flag'),
        ('wait inside prepare', 'implementation', 'emitted = true;', 'vkDeviceWaitIdle(device); emitted = true;'),
        ('scratch left writable', 'implementation', 'Transition(cmdBuffer, g_vk.preScratch, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);', 'Transition(cmdBuffer, g_vk.preScratch, VK_IMAGE_LAYOUT_GENERAL);'),
        ('recording pin absent', 'implementation', 'if (!VkRecording::Recordings().Pin(*recording, { g_vk.recording }))', 'if (false)'),
        ('loan released before post', 'implementation', 'if (PreRecordingBusy())', 'if (false)'),
    )
    for name, target, old, new in mutations:
        variants = dict(implementation=implementation, inputs=inputs, header=header)
        assert old in variants[target], name
        # Pin also occurs in the preserved post body: mutate the pre helper only.
        if name == 'recording pin absent':
            prepared = body(variants[target], 'RecordPreUpscaleVk')
            variants[target] = variants[target].replace(prepared, prepared.replace(old, new, 1), 1)
        else:
            variants[target] = variants[target].replace(old, new, 1)
        try:
            validate(**variants)
        except (AssertionError, ValueError):
            continue
        raise AssertionError('source mutant survived: '+name)
    print(f'PASS: {len(mutations)} placement/layout/lifetime source mutants rejected')
    print('PASS: post-upscale body byte-identical; scope-only NR before each single SR, restore-before-End, layouts, no new waits/submits')


def validate(implementation, inputs, header):
    prepared = body(implementation, 'RecordPreUpscaleVk')
    ctor = body(implementation, 'ScopedPreUpscaleVk::ScopedPreUpscaleVk')
    dtor = body(implementation, 'ScopedPreUpscaleVk::~ScopedPreUpscaleVk')
    assert 'vkDeviceWaitIdle' not in prepared+ctor+dtor
    assert 'DriverInitGate::Scope' not in prepared and prepared.count('DriverInitGate::TryScope')==2
    assert 'g_vk.reset = true; // no submission receipt' in prepared
    # The real pre tail publishes under the route lock; the unchanged post consumes only after picture/recording gates.
    pre_call = prepared.index('const int evaluated = g_fwd.evaluate(')
    assert prepared.index('std::unique_lock<std::mutex> lock(g_vkMutex, std::try_to_lock)') < pre_call
    tail = prepared[pre_call:prepared.index('if (evaluated != 1)', pre_call)]
    assert tail.index('g_vk.reset = false;') < tail.index('g_vkResetOnReturn = true;')
    post = body(implementation, 'EvaluateAfterUpscaleVk')
    assert post.index('Role::Aside') < post.index('lock(g_vkMutex)') < post.index('if (!recording)') < post.index('g_vkResetOnReturn.exchange(false)')
    assert not re.search(r'vk(?:QueueSubmit|QueueWaitIdle|WaitForFences)\w*\(', prepared+ctor+dtor)
    assert implementation.count('RecordPreUpscaleVk(') == 2
    assert 'RecordPreUpscaleVk(' in ctor and 'RecordPreUpscaleVk(' not in dtor
    assert ctor.index('Pictures::Ask(') < ctor.index('Runtime::Facts()') < ctor.index('RecordPreUpscaleVk(')
    assert ctor.index('RecordPreUpscaleVk(') < ctor.index('_colour.Bind(')
    assert 'bindingsKnown' not in ctor
    assert 'RouteRecordingBusy() || VkPre::SameTicket' in prepared
    assert prepared.index('Recordings().Pin(') < prepared.index('g_fwd.create(')
    assert prepared.index('preScratch, VK_IMAGE_LAYOUT_GENERAL') < prepared.index('preScratch.view, VK_NULL_HANDLE')
    assert 'g_vk.preScratch, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL' in prepared
    assert 'colour->Resource.ImageViewInfo.ImageView, VK_NULL_HANDLE' not in prepared
    evaluate = body(inputs, 'NVSDK_NGX_VULKAN_EvaluateFeature')
    for call in ('auto result = NVNGXProxy::VULKAN_EvaluateFeature()', 'auto upscaleResult = deviceContext->Evaluate('):
        at = evaluate.index(call)
        assert evaluate[:at].rfind('pre.emplace(') > evaluate[:at].rfind('pre->Complete()')
        complete = evaluate.index('pre->Complete()', at)
        assert at < complete < evaluate.index('EvaluateAfterPreSafeVk(', at)
        assert evaluate.count(call)==1
    assert evaluate.index('pre->Complete()',evaluate.index('auto upscaleResult')) < evaluate.index('UpscaleEnd(')
    assert evaluate.count('(!pre || pre->AllowPost())')==2
    assert evaluate.count('ScopedPreUpscaleVk')==2 and evaluate.count('pre.emplace(')==2
    assert evaluate.count('DlssNr::EvaluateAfterPreSafeVk(')==2
    post = body(implementation, 'EvaluateAfterPreSafeVk')
    assert post.index('g_preBorrow.test_and_set') < post.index('PreRecordingBusy()') < post.rindex('EvaluateAfterUpscaleVk(')
    assert 'ReleaseLoan' in post and 'g_nativePlacement.PostNeedsLoan()' in post
    assert 'g_nativePlacement.TryPre()' in prepared
    assert not re.search(r'vkCmd|RecordPreUpscaleVk|Dispatch|evaluate\(', dtor)
    assert 'void Complete() noexcept { _colour.Restore(); }' in header
    assert evaluate.count('pre->Complete()') == 2


if __name__ == '__main__':
    run()

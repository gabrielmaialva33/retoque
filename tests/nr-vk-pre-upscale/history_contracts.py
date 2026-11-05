"""Real post gates/wrapper and both model-call tails, with fake allocation/shader preparation."""
from pathlib import Path
import subprocess
from source_contracts import body
ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent


def sources():
    source = (ROOT/'retoque/dlssnr/DlssNrFeature_Vk.cpp').read_text()
    post = body(source, 'EvaluateAfterUpscaleVk')
    pre = body(source, 'RecordPreUpscaleVk')
    def call(text):
        return text[text.index('    const int evaluated = g_fwd.evaluate('):text.index('    if (evaluated != 1)')]
    # No synthetic reset logic: use the entire production admission prefix, including its lock and gates.
    admission = post[:post.index('    const uint32_t wpSource =')]
    return '\n'.join((body(source, 'RouteRecordingBusy'), body(source, 'PreRecordingBusy'),
        'void PreModelCall(VkCommandBuffer cmdBuffer) { auto& cfg = *Config::Instance();\n' + call(pre) + 'assert(evaluated == 1); }',
        admission + '\n(void)instance; if (skipPreparation) return;\n' + call(post) + 'assert(evaluated == 1); }',
        body(source, 'EvaluateAfterPreSafeVk')))


def run(folder, mutants=True):
    from run import FLAGS
    production = sources()
    changes = {
        'post ignores pending restart': ('if (g_vkResetOnReturn.exchange(false))', 'if (false && g_vkResetOnReturn.exchange(false))'),
        'model ignores reset': ('g_vk.reset ? 1 : 0', '0'),
        'post skips retirement': ('if (PreRecordingBusy())', 'if (false && PreRecordingBusy())'),
    }
    variants = [('baseline', production)]
    if mutants:
        pre_end = production.index('void EvaluateAfterUpscaleVk')
        pre_call = production[:pre_end]
        assert 'g_vkResetOnReturn = true;' in pre_call
        variants += [('pre forgets return reset', pre_call.replace('g_vkResetOnReturn = true;', '', 1) + production[pre_end:])]
        for name, (old, new) in changes.items():
            assert old in production, name
            # Only mutate the post call for the reset argument mutant.
            if name == 'model ignores reset':
                variant = production[:pre_end] + production[pre_end:].replace(old, new, 1)
            else:
                variant = production.replace(old, new, 1)
            variants.append((name, variant))
    for name, source in variants:
        cpp = folder/'history_driver.cpp'
        cpp.write_text((HERE/'history_driver.cpp').read_text().replace('// PRODUCTION', source))
        out = folder/'history_driver'
        flags = FLAGS if name == 'baseline' else ['g++', '-std=c++20', '-O0', '-Wall', '-Wextra', '-Werror', '-pthread']
        flags = flags + ['-I'+str(ROOT/'retoque'), '-I'+str(ROOT/'external/vulkan/include')]
        # pch.h in the adapter stubs includes Config; this fixture supplies its own richer Config.
        (folder/'pch.h').write_text('#pragma once\n#include <vulkan/vulkan.h>\n')
        result = subprocess.run(flags + [str(cpp), '-o', str(out)], capture_output=True, text=True)
        assert result.returncode == 0, name + ': ' + result.stderr
        failures = []
        for scenario in ('post-only', 'equal-size', 'skipped-gap'):
            result = subprocess.run([str(out), scenario], capture_output=True, text=True)
            if name == 'baseline':
                assert result.returncode == 0, scenario + ': ' + result.stderr
            failures.append(result.returncode != 0 and 'Assertion' in result.stderr)
        if name != 'baseline':
            assert any(failures), 'survived: ' + name
    print(f'PASS: actual model reset argument, equal-size retirement, skipped gap, post-only; {len(variants)-1} compiling mutants')


if __name__ == '__main__':
    import tempfile
    with tempfile.TemporaryDirectory(dir=HERE/'artifacts', prefix='history-') as tmp:
        run(Path(tmp))

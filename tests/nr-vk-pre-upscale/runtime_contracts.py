"""Compile the complete runtime adapter; device/export lookup and Detours are fake."""
from pathlib import Path
import subprocess
ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
SCENARIOS = ('core-only', 'alias-only', 'all-addresses', 'export-only', 'core-missing', 'alias-failure', 'core-failure',
             'diagnostic-failure', 'export-failure') + tuple(
    f'{kind}:{command}' for command in ('vkCmdPipelineBarrier2', 'vkCmdBeginRenderPass2',
        'vkCmdEndRenderPass2', 'vkCmdEndRendering', 'vkCmdWaitEvents2')
    for kind in ('core-missing', 'alias-failure'))


def run(folder, mutants=True):
    from run import FLAGS
    source = (ROOT/'retoque/dlssnr/DlssNr_VkPreRuntime.cpp').read_text()
    changes = {
        'missing descriptor diagnostic required': ('"vkCmdBindDescriptorBuffersEXT", false', '"vkCmdBindDescriptorBuffersEXT", true'),
        'missing promoted alias required': ('required && !equivalentPresent', 'required && (equivalentPresent || !equivalentPresent)'),
        'core mandatory despite callable alias': ('core, true, available', 'core, true, false'),
        'callable alias failure ignored': ('extension, true, available', 'extension, false, available'),
        'callable core failure ignored': ('core, true, available', 'core, false, available'),
        'missing promoted group accepted': ('const bool available =', 'const bool available = true ||'),
        'export attachment failure ignored': ('status.requiredFailed += required;', 'status.requiredFailed += false;'),
    }
    variants = [('baseline', source)]
    if mutants:
        for name, (old, new) in changes.items():
            assert old in source, name
            # Only suppress failure accounting in the export path, not the device-proc path.
            if name == 'export attachment failure ignored':
                before, after = source.split('    if (module)\n', 1)
                changed = before + '    if (module)\n' + after.replace(old, new, 1)
            else:
                changed = source.replace(old, new, 1)
            variants.append((name, changed))
    for name, text in variants:
        cpp = folder/'runtime_adapter.cpp'; cpp.write_text(text)
        out = folder/'runtime_adapter'
        flags = FLAGS if name == 'baseline' else ['g++', '-std=c++20', '-O0', '-Wall', '-Wextra', '-Werror', '-pthread']
        result = subprocess.run(flags + ['-I'+str(HERE/'stubs'), '-I'+str(ROOT/'retoque'),
            '-I'+str(ROOT/'retoque/dlssnr'), '-I'+str(ROOT/'external/vulkan/include'),
            '-I'+str(ROOT/'external/nvngx_dlss_sdk'), str(HERE/'runtime.cpp'), str(cpp), '-o', str(out)],
            capture_output=True, text=True)
        assert result.returncode == 0, name + ': ' + result.stderr
        rejected = False
        for scenario in SCENARIOS:
            result = subprocess.run([str(out), scenario], capture_output=True, text=True)
            if name == 'baseline':
                assert result.returncode == 0, scenario + ': ' + result.stderr
            rejected |= result.returncode != 0 and 'Assertion' in result.stderr
        if name == 'baseline':
            subprocess.run([str(out)], check=True)
        else:
            assert rejected, 'survived: ' + name
    print(f'PASS: runtime adapter, {len(SCENARIOS)} coverage scenarios; {len(variants)-1} compiling mutants')


if __name__ == '__main__':
    import tempfile
    with tempfile.TemporaryDirectory(dir=HERE/'artifacts', prefix='runtime-') as tmp:
        run(Path(tmp))

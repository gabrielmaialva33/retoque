#!/usr/bin/env python3
"""Host-only execution of the production observer installation and forwarding boundaries."""
import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

from hooks_test import function

HERE = Path(__file__).resolve().parent


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--root', type=Path, default=HERE.parents[1])
    args = parser.parse_args()
    root = args.root.resolve()
    host = (root / 'retoque/dlssnr/DlssNr_VkPresentHost.cpp').read_text()
    common = (HERE / 'hooks_cases.cpp').read_text()
    common_fakes = common.split('// @production@', 1)[0]
    common_helpers = '\n\n'.join(function(common, signature) for signature in
                                    ('Ticket DirectRecording(', 'std::weak_ptr<int> Pin(', 'void Fresh()'))
    hook_device = function(host, 'void HookDevice(VkDevice device)')
    declarations = re.findall(r'^Targets<(PFN_\w+)> (g_\w+);', host, re.MULTILINE)
    real_types = {'PFN_vkAllocateCommandBuffers', 'PFN_vkBeginCommandBuffer', 'PFN_vkEndCommandBuffer',
                  'PFN_vkResetCommandBuffer', 'PFN_vkFreeCommandBuffers', 'PFN_vkResetCommandPool',
                  'PFN_vkDestroyCommandPool'}
    extra_types = '\n'.join(f'using {pfn} = PFN_vkVoidFunction;' for pfn, _ in declarations if pfn not in real_types)
    production = extra_types + '\n' + re.search(r'constexpr size_t kTargets = \d+;', host).group(0)
    production += '\n' + function(host, 'template <typename Pfn> struct Targets') + ';\n'
    production += '\n'.join(f'Targets<{pfn}> {name};' for pfn, name in declarations)
    production += '\n' + re.search(r'template <typename T> Handle H\(T handle\)[^\n]+', host).group(0) + '\n'
    production += function(host, 'template <typename Pfn>\nvoid AttachTarget(') + '\n'

    # Unrelated swapchain/queue callbacks are outside this fixture. Their lookup returns null; only lifecycle
    # callbacks and their slot tables are extracted. This compiles with no wDx12 wrapper implementations linked.
    callbacks = re.findall(r'template <size_t S>\s+(?:VkResult|void) VKAPI_CALL (hkRecording\w+)\(', host)
    for callback in callbacks:
        start = host.index(callback + '(')
        start = host.rfind('template <size_t S>', 0, start)
        production += function(host[start:], 'template <size_t S>') + '\n'
    macro_start = host.index('#define DLSSNR_VK_SLOTS(')
    macro_stop = host.index('\n\n', macro_start)
    production += host[macro_start:macro_stop] + '\n'
    tables = set(re.findall(r'k\w+Hooks', hook_device))
    for table in sorted(tables):
        match = re.search(r'const std::array<void\*, kTargets>\s+' + table + r'\s*=\s*[^;]+;', host)
        if table.startswith('kRecording'):
            production += match.group(0) + '\n'
        else:
            production += f'const std::array<void*, kTargets> {table} {{}};\n'
    production += '#undef DLSSNR_VK_SLOTS\n'
    production += function(host, 'struct QueueCallTargets') + ';\n'
    production += function(host, 'QueueCallTargets QueueCall(') + '\n'
    production += function(host, 'void AttachQueueCall(') + '\n' + hook_device + '\n'
    production += 'void ResetTargetSets()\n{\n'
    production += '\n'.join(f'    {name} = {{}};' for _, name in declarations) + '\n}\n'
    install = function(host, 'void DlssNr::VkPresentHost::Install(')
    prefix = install[:install.index('    std::lock_guard lock(g_installMutex);')]
    production += prefix.replace('void DlssNr::VkPresentHost::Install(', 'void ObserveInstallGate(')
    production += '    ++admissions;\n}\n'

    text = (HERE / 'install_cases.cpp').read_text().replace('// @common-fakes@', common_fakes)
    text = text.replace('// @production@', production).replace('// @common-helpers@', common_helpers)
    with tempfile.TemporaryDirectory(prefix='nr-vk-observer-install-') as directory:
        directory = Path(directory)
        source = directory / 'cases.cpp'
        source.write_text(text)
        executable = directory / 'cases'
        compiler = shlex.split(os.environ.get('CXX', 'g++'))
        subprocess.run(compiler + ['-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-pthread',
                                   '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie',
                                   '-I' + str(root / 'retoque'), str(source), '-o', str(executable)], check=True)
        env = dict(os.environ)
        env.setdefault('ASAN_OPTIONS', 'detect_leaks=1:abort_on_error=1')
        env.setdefault('UBSAN_OPTIONS', 'halt_on_error=1:print_stacktrace=1')
        subprocess.run([str(executable)], env=env, check=True, timeout=30)
    print('PASS: independent Vulkan observer installation and forwarding (host only)')


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Compile production lifecycle wrappers against a driver boundary fake; no GPU is executed."""
import argparse
import os
from pathlib import Path
import re
import shlex
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent


def function(source, signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth = 1
    end = brace + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('--root', type=Path, default=HERE.parents[1])
    args = parser.parse_args()
    root = args.root.resolve()
    bridge = (root / 'OptiScaler/hooks/VulkanwDx12_Hooks.cpp').read_text()
    host = (root / 'retoque/dlssnr/DlssNr_VkPresentHost.cpp').read_text()
    signatures = [
        'VkResult Vulkan_wDx12::hk_vkBeginCommandBuffer(',
        'VkResult Vulkan_wDx12::hk_vkEndCommandBuffer(',
        'VkResult Vulkan_wDx12::hk_vkResetCommandBuffer(',
        'void Vulkan_wDx12::hk_vkFreeCommandBuffers(',
        'VkResult Vulkan_wDx12::hk_vkAllocateCommandBuffers(',
        'void Vulkan_wDx12::hk_vkDestroyCommandPool(',
        'VkResult Vulkan_wDx12::hk_vkResetCommandPool(',
    ]
    production = '\n\n'.join(function(bridge, signature) for signature in signatures)
    production += '\n\n' + function(host, 'VkCommandBuffer AllocateCommandBuffer(')
    production += '\n\n' + function(host, 'void FreeCommandBuffer(')
    production += '\n\n' + function(host, 'bool Begin(DeviceTransport&')
    if 'bool End(DeviceTransport&' in host:
        production += '\n\n' + function(host, 'bool End(DeviceTransport&')
    production += '\n\n' + function(host, 'bool RecordHandOver(')
    production += '\n\n' + function(host, 'bool RecordRecovery(')
    # Exercise the actual work-recording tail without reproducing its image/model body. The two cross-family
    # functions above are extracted whole. This boundary is present both before and after the lifecycle fix.
    before_handover = host[:host.index('// Cross-family, on the presenting queue: the release')]
    end_tail = re.findall(r'^\s*return [^\n]+;', before_handover, re.MULTILINE)[-1].strip()
    production += '\n\nRecorded EndRecordedTail(DeviceTransport& dev, VkCommandBuffer buffer)\n{\n'
    production += '    ' + end_tail + '\n}\n'

    text = (HERE / 'hooks_cases.cpp').read_text().replace('// @production@', production)
    with tempfile.TemporaryDirectory(prefix='nr-vk-hook-lifecycle-') as directory:
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
    print('PASS: production Vulkan lifecycle wrapper boundaries (host only)')


if __name__ == '__main__':
    main()

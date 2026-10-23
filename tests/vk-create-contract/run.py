#!/usr/bin/env python3
"""Compile OptiScaler's production vkCreateDevice path -- hkvkCreateDevice, the Vulkan spoofing's device
create and feature state, the NR extension block -- against a fake driver that records the exact request it
is handed, and check the create contract: the game's request unchanged, every pointer alive until the driver
returns, no cross-talk between two creates on two threads, no capability the device lacks requested.

The mutation control (default on; --no-mutations skips it) puts back the function-static extension lists that
d391a944 made per thread, and requires the interleaving cases to fail with them."""
from pathlib import Path
import argparse
import os
import re
import subprocess
import sys
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--no-mutations', action='store_true', help='skip the regression controls')
parser.add_argument('case', nargs='*', help='run only these cases')
args = parser.parse_args()

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
VULKAN = ROOT / 'external/vulkan/include'
assert (VULKAN / 'vulkan/vulkan.h').exists(), 'external/vulkan is not checked out'


def block(source, anchor, terminator=''):
    """The declaration starting at anchor through its matching closing brace."""
    start = source.index(anchor)
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    if terminator:
        assert source[end:end + len(terminator)] == terminator, anchor
        end += len(terminator)
    return source[start:end]


def between(source, start_anchor, end_anchor):
    start = source.index(start_anchor)
    return source[start:source.index(end_anchor, start)]


def line(source, pattern):
    found = re.search(pattern, source, re.M)
    assert found, pattern
    return found.group(0)


def without_includes(text):
    return '\n'.join(l for l in text.splitlines() if not l.startswith(('#include ', '#pragma once'))) + '\n'


spoofing = (ROOT / 'OptiScaler/spoofing/Vulkan_Spoofing.cpp').read_text()
spoofing_h = (ROOT / 'OptiScaler/spoofing/Vulkan_Spoofing.h').read_text()
hooks = (ROOT / 'OptiScaler/hooks/Vulkan_Hooks.cpp').read_text()
state = (ROOT / 'OptiScaler/State.h').read_text()

fakes = (HERE / 'fakes.h').read_text()
fakes = fakes.replace('    // @creatingD3DDevice@ (the production declaration from State.h, put here by run.py)',
                      line(state, r'^    inline static thread_local bool creatingD3DDevice = false;$'))
assert 'creatingD3DDevice = false' in fakes

production = []
# State.h: the two per-thread scopes the path depends on, as they are.
production.append(block(state, 'class ScopedSkipVulkanHooks', ';'))
production.append(block(state, 'class ScopedCreatingD3DDevice', ';'))
# Vulkan_Spoofing: its function pointers, the feature helpers, the feature state and the device create.
production.append(without_includes(spoofing_h))
production.extend(re.findall(r'^static PFN_vk\w+ o_vk\w+ = nullptr;$', spoofing, re.M))
production.append(between(spoofing, 'static bool HasExtension(size_t extensionCount',
                          'inline static void hkvkGetPhysicalDeviceMemoryProperties('))
production.append(between(spoofing, 'struct VulkanDeviceFeatureState::Impl',
                          'VkResult VulkanSpoofing::hkvkCreateDevice('))
production.append(block(spoofing, 'VkResult VulkanSpoofing::hkvkCreateDevice('))
# Vulkan_Hooks: the globals the create touches, the API version tracking, and the hook itself.
for pattern in (r'^static VkDevice _device = VK_NULL_HANDLE;$',
                r'^static VkPhysicalDevice _PD = VK_NULL_HANDLE;$',
                r'^static std::mutex _vkApiVersionMutex;$',
                r'^static std::unordered_map<VkInstance, uint32_t> _instanceApiVersions;$',
                r'^static std::unordered_map<VkPhysicalDevice, VkInstance> _physicalDeviceInstances;$',
                r'^PFN_vkCreateDevice o_vkCreateDevice = nullptr;$',
                r'^static PFN_vkGetInstanceProcAddr o_vkGetInstanceProcAddr = nullptr;$',
                r'^static PFN_vkGetDeviceProcAddr o_vkGetDeviceProcAddr = nullptr;$',
                r'^static PFN_vkGetPhysicalDeviceFeatures2 o_vkGetPhysicalDeviceFeatures2 = nullptr;$',
                r'^static VkDevice _lowLatency2Device = VK_NULL_HANDLE;$'):
    production.append(line(hooks, pattern))
production.append(block(hooks, 'static bool TimelineSemaphoresEnabled('))
production.append(block(hooks, 'static void TrackPhysicalDevice('))
production.append(block(hooks, 'static uint32_t GetPhysicalDeviceApiVersion('))
production.append(block(hooks, 'static VkResult hkvkCreateDevice('))
production = '\n'.join(production) + '\n'

# The production code reads the NR helpers through its own include root.
unit = fakes + '#include <dlssnr/DlssNr_VkExtensions.h>\n' + production + (HERE / 'cases.cpp').read_text()

CASES = [
    'game-request-unchanged',
    'game-request-unchanged-fg',
    'pointers-live-until-return',
    'interleaved-native',
    'interleaved-d3d',
    'interleaved-mixed',
    'antilag-crosstalk',
    'capabilities-native',
    'capabilities-nr',
    'push-descriptor-off',
    'push-descriptor-supported',
    'bda-never-both',
    'memory-model-device-scope',
    'api-versions',
    'enumeration-fails-native',
    'enumeration-incomplete-native',
    'enumeration-fails-d3d',
    'enumeration-incomplete-d3d',
    'nr-off-on',
    'skip-scopes-nested',
    'extension-spoofing-off-on',
    'd3d-path',
]
# Probes: run and reported, never failing the suite. nested-create-in-driver shows the boundary of the per-thread
# lists (a create nested on the same thread inside the driver call reuses the outer call's storage); see README.
PROBES = ['nested-create-in-driver']
# The cases that must fail once the extension lists are function-static again (A-01).
INTERLEAVED = ['interleaved-native', 'interleaved-d3d', 'interleaved-mixed']

selected = args.case or CASES
unknown = [c for c in selected if c not in CASES + PROBES]
if unknown:
    raise SystemExit('unknown case(s): ' + ', '.join(unknown))

FLAGS = ['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function',
         '-Wno-unused-parameter', '-Wno-unused-variable', '-Wno-missing-field-initializers', '-fsanitize=address,undefined',
         '-fno-sanitize-recover=undefined', '-fno-omit-frame-pointer', '-pthread',
         '-isystem', str(VULKAN), '-I', str(ROOT / 'retoque')]
env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0:detect_stack_use_after_return=1')

failed = []
with tempfile.TemporaryDirectory(prefix='vk-create-contract-') as folder:
    folder = Path(folder)

    def build(name, source):
        cpp = folder / f'{name}.cpp'
        cpp.write_text(source)
        binary = folder / name
        subprocess.run(FLAGS + [str(cpp), '-o', str(binary)], check=True)
        return binary

    binary = build('contract', unit)
    for case in selected:
        result = subprocess.run([str(binary), case], env=env, capture_output=True, text=True)
        sys.stdout.write(result.stdout)
        if result.returncode:
            sys.stdout.write(result.stderr[-4000:])
        print(f'{"PASS" if result.returncode == 0 else "FAIL"} {case}', flush=True)
        if result.returncode:
            failed.append(case)

    if not args.case:
        for probe in PROBES:
            result = subprocess.run([str(binary), probe], env=env, capture_output=True, text=True)
            print(f'PROBE {probe}: ' + ('reproduced (the outer request changed under the driver)' if result.returncode
                                        else 'no longer reproduces -- update the README'), flush=True)

    if not args.no_mutations and not args.case:
        # A-01 control: the lists back to function-static, as before d391a944.
        # The device create holds two of them: filteredExtensions (dxvk/vkd3d-proton) and newExtensionList.
        assert unit.count('static thread_local std::vector<const char*>') == 2, 'the per-thread lists moved'
        mutated = unit.replace('static thread_local std::vector<const char*>', 'static std::vector<const char*>')
        control = build('control-static-lists', mutated)
        for case in INTERLEAVED:
            result = subprocess.run([str(control), case], env=env, capture_output=True, text=True)
            red = result.returncode != 0
            why = next((l.strip() for l in result.stderr.splitlines() if l.startswith(('FAIL:', '==')) and
                        ('FAIL:' in l or 'ERROR: AddressSanitizer' in l)), '')
            print(f'{"PASS" if red else "FAIL"} control: {case} fails with function-static lists'
                  + (f' ({why[:160]})' if why else ''), flush=True)
            if not red:
                failed.append('control ' + case)

if failed:
    raise SystemExit('Failed: ' + ', '.join(failed))

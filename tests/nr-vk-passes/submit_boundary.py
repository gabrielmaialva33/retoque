#!/usr/bin/env python3
"""Characterize real submit wrappers; no Vulkan loader, device, Wine or NGX."""
from pathlib import Path
import os
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / 'tests/nr-vk-recording'))
from hooks_test import function

source = (ROOT / 'retoque/dlssnr/DlssNr_VkPresentHost.cpp').read_text()
production = '\n\n'.join(function(source, signature) for signature in (
    'template <typename Pfn, typename... Args>\nVkResult LockedQueueCall(',
    'template <size_t S>\nVkResult VKAPI_CALL hkQueueSubmit(',
    'template <size_t S>\nVkResult VKAPI_CALL hkQueueSubmit2(',
))
# The shared callback is tested via both slots; real Detours alias installation is NOT exercised here.
assert 'AttachQueueCall(submit2Khr, g_submit2, kSubmit2Hooks, "vkQueueSubmit2KHR")' in source
with tempfile.TemporaryDirectory(prefix='nr-vk-submit-boundary-') as temporary:
    directory = Path(temporary)
    cpp = directory / 'cases.cpp'
    cpp.write_text((HERE / 'submit_boundary.cpp').read_text().replace('// @production@', production))
    binary = directory / 'cases'
    subprocess.run(['g++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-pthread',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie', '-g',
                    '-I', str(HERE), '-I', str(ROOT / 'retoque/dlssnr'),
                    '-I', str(ROOT / 'external/vulkan/include'), str(cpp), '-o', str(binary)], check=True)
    env = dict(os.environ)
    env.setdefault('ASAN_OPTIONS', 'detect_leaks=1:abort_on_error=1')
    env.setdefault('UBSAN_OPTIONS', 'halt_on_error=1:print_stacktrace=1')
    subprocess.run([str(binary)], env=env, check=True, timeout=30)

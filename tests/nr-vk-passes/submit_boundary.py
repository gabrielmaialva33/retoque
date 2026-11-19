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
production = function(source, 'struct ChainState') + ';\nstd::unique_ptr<ChainState> g_chainOwner;\nstd::atomic<ChainState*> g_chainState { nullptr };\n'
production += 'thread_local unsigned g_creationSubmitDepth = 0;\nthread_local VkQueue g_creationSubmitQueue = VK_NULL_HANDLE;\n' + '\n\n'.join(function(source, signature) for signature in (
    'ChainState* EnsureChainState()',
    'CreationObserver* CreationObserverFor(',
    'void RegisterCreationObserver(',
    'template <size_t S>\nVkResult VKAPI_CALL hkChainCreateCommandPool(',
    'void ForgetChainPool(',
    'template <typename Pfn, typename... Args>\nVkResult LockedQueueCall(',
    'template <typename Pfn, typename Info>\nVkResult ObservedQueueSubmit(',
    'template <size_t S>\nVkResult VKAPI_CALL hkQueueSubmit(',
    'template <size_t S>\nVkResult VKAPI_CALL hkQueueSubmit2(',
))
for signature, qualified, local in (
    ('bool DlssNr::VkPresentHost::ChainRecordingAllowed(',
     'DlssNr::VkPresentHost::ChainRecordingAllowed', 'ChainRecordingAllowed'),
    ('DlssNr::VkChain::BudgetDecision\nDlssNr::VkPresentHost::AdmitChainBudget(',
     'DlssNr::VkPresentHost::AdmitChainBudget', 'AdmitChainBudget'),
    ('DlssNr::VkChain::BudgetSample DlssNr::VkPresentHost::SampleChainBudget(',
     'DlssNr::VkPresentHost::SampleChainBudget', 'SampleChainBudget'),
):
    production += '\n' + function(source, signature).replace(qualified, local, 1)
# One frozen decision precedes the real creation call and feeds every new startup action.
create = function(source, 'VkResult VKAPI_CALL hkCreateDevice(')
assert create.index('const bool chainRequested') < create.index('o_CreateDevice(')
assert create.count('DlssNrPasses.value_or_default()') == 1
assert 'EnableBudgetExtension(chainRequested,' in create
assert 'HookDevice(*device, chainRequested)' in create
assert 'if (chainRequested)\n            RegisterCreationObserver' in create
hook = function(source, 'DlssNr::VkChain::HookCoverage HookDevice(')
assert 'Config::Instance()' not in hook
assert 'chainRequested ? EnsureChainState() : nullptr' in hook
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

    creation = directory / 'creation'
    subprocess.run(['g++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-pthread',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie', '-g',
                    '-I', str(HERE), '-I', str(ROOT / 'retoque/dlssnr'),
                    '-I', str(ROOT / 'external/vulkan/include'), str(HERE / 'creation_cases.cpp'),
                    '-o', str(creation)], check=True)
    subprocess.run([str(creation)], env=env, check=True, timeout=30)

#!/usr/bin/env python3
"""Vulkan after-upscale NR runs once per frame on a bridged upscaler.

A Vulkan upscaler backend whose feature derives from IFeature_VkwDx12 does its work on a D3D12 device, and
IFeature_VkwDx12::Evaluate already runs NR there (DlssNr::EvaluateAfterUpscale, on the D3D12 copies). The Vulkan
input (NVSDK_NGX_VULKAN_EvaluateFeature) must therefore skip its own DlssNr::EvaluateAfterUpscaleVk for every such
backend, or the model runs twice per frame, the second pass over a frame the first already edited. DLSS_on12 was
missing from that list (janblade issue #58 found the same omission in that fork).

Read off the source, no compiler: the backends FeatureProvider_Vk creates a bridged feature for must all be in the
Vulkan input's `bridged` list, and nothing native may be in it (that would silently drop NR for a Vulkan-native
upscaler). The premise is checked too: the bridge's Evaluate still runs NR on the D3D12 side. Each rule first fails
on a planted violation."""
from pathlib import Path
import re
import sys

root = Path(__file__).resolve().parents[2]
PROVIDER = root / 'OptiScaler/upscalers/FeatureProvider_Vk.cpp'
INPUT = root / 'OptiScaler/inputs/NVNGX_DLSS_Vk.cpp'
BRIDGE = root / 'OptiScaler/upscalers/IFeature_VkwDx12.cpp'
UPSCALERS = root / 'OptiScaler/upscalers'


def code(text):
    """Without comments, so a name in a comment never counts."""
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    return re.sub(r'//[^\n]*', '', text)


def created(provider):
    """{backend: {feature classes FeatureProvider_Vk makes for it}}, from its `case Upscaler::X:` blocks."""
    text = code(provider)
    found = {}
    cases = list(re.finditer(r'case\s+Upscaler::(\w+)\s*:', text))
    for i, case in enumerate(cases):
        end = cases[i + 1].start() if i + 1 < len(cases) else len(text)
        block = text[case.end():end]
        block = block[:m.start()] if (m := re.search(r'\bdefault\s*:', block)) else block
        found[case.group(1)] = set(re.findall(r'make_unique<(\w+)>', block))
    return found


def bridged_classes(headers):
    """Feature classes that derive from IFeature_VkwDx12."""
    return {m.group(1) for text in headers for m in re.finditer(r'class\s+(\w+)\s*(?:final\s*)?:\s*public\s+'
                                                                  r'IFeature_VkwDx12\b', code(text))}


def skipped(vk_input):
    """The backends the Vulkan input's `bridged` expression names."""
    m = re.search(r'const\s+bool\s+bridged\s*=(.*?);', code(vk_input), re.S)
    if m is None:
        return None
    return set(re.findall(r'Upscaler::(\w+)', m.group(1)))


def check(provider, vk_input, bridge, headers):
    problems = []
    classes = bridged_classes(headers)
    made = created(provider)
    expected = {backend for backend, types in made.items() if types & classes}
    names = skipped(vk_input)
    if not classes or not expected:
        problems.append('no bridged feature class or backend found: the parse no longer matches the source')
    if names is None:
        problems.append('no `const bool bridged = ...;` in the Vulkan input')
        return problems
    for backend in sorted(expected - names):
        problems.append(f'{backend} creates a bridged feature ({", ".join(sorted(made[backend] & classes))}) but is '
                        'not in the Vulkan input\'s bridged list: NR would run twice per frame')
    native = {backend for backend, types in made.items() if types and not types & classes}
    for backend in sorted(names & native):
        problems.append(f'{backend} is a Vulkan-native backend in the bridged list: its NR would never run')
    evaluate = code(bridge)
    start = evaluate.find('bool IFeature_VkwDx12::Evaluate(')
    body = evaluate[start:evaluate.find('\nbool IFeature_VkwDx12::', start + 1)] if start >= 0 else ''
    if 'DlssNr::EvaluateAfterUpscale(' not in body:
        problems.append('IFeature_VkwDx12::Evaluate no longer runs NR on the D3D12 side: the bridged list\'s premise '
                        'changed, revisit it')
    return problems


def self_test(provider, vk_input, bridge, headers):
    """Each rule fails on a planted violation of the real files."""
    problems = []
    names = skipped(vk_input) or set()
    planted = []
    if names:
        victim = sorted(names & {b for b, t in created(provider).items() if t & bridged_classes(headers)})
        if victim:
            planted.append(('a bridged backend dropped from the list',
                            re.sub(r'\s*(\|\|\s*)?backend\s*==\s*Upscaler::' + victim[0] + r'\b(\s*\|\|)?',
                                   lambda m: ' ||' if m.group(1) and m.group(2) else '', vk_input, count=1),
                            bridge, 'not in the Vulkan input'))
    planted.append(('a native backend added to the list',
                    re.sub(r'const\s+bool\s+bridged\s*=', 'const bool bridged = backend == Upscaler::FSR22 ||',
                           vk_input, count=1), bridge, 'Vulkan-native backend'))
    planted.append(('the bridge no longer running NR', vk_input,
                    bridge.replace('DlssNr::EvaluateAfterUpscale(', 'DlssNr::Removed('), 'premise'))
    for what, planted_input, planted_bridge, expect in planted:
        found = check(provider, planted_input, planted_bridge, headers)
        if not any(expect in p for p in found):
            problems.append(f'self-test: {what} was not caught ({found})')
    return problems


def main():
    provider, vk_input, bridge = PROVIDER.read_text(), INPUT.read_text(), BRIDGE.read_text()
    headers = [p.read_text(errors='replace') for p in UPSCALERS.rglob('*.h')]
    problems = self_test(provider, vk_input, bridge, headers)
    if not problems:
        print('PASS 0: every rule fails on its planted violation')
    problems += check(provider, vk_input, bridge, headers)
    for problem in problems:
        print(f'FAIL: {problem}')
    if problems:
        return 1
    expected = sorted(b for b, t in created(provider).items() if t & bridged_classes(headers))
    print(f'PASS: the Vulkan input skips its own NR for every bridged backend ({", ".join(expected)}), '
          'for no native one, and the bridge still runs NR on the D3D12 side')
    return 0


if __name__ == '__main__':
    sys.exit(main())

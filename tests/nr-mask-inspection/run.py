#!/usr/bin/env python3
"""Mask inspection, slice 0 ([DlssNr] MaskInspection=log): off is inert, the key round-trips, and what is logged is bounded.

Three parts, each broken on purpose afterwards:

1. The production header (retoque/dlssnr/DlssNr_MaskInspection.h) through its own Scope with a fake config, clock and
   logger, under ASan/UBSan, with every heap allocation of the process counted.
2. The key through the production CustomOptional, Config::readString and its own read and save statements, against
   the real SimpleIni and the shipped OptiScaler.ini.
3. Where the D3D12 pass and the Vulkan routes use it, read off the source.

No D3D12, no Vulkan, no shader, no game: see README.md for what that leaves unproven.
"""
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / '_support'))
from cpp_source import function  # noqa: E402
from nr_sources import renderer_source  # noqa: E402

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
ARTIFACTS = HERE / 'artifacts'
ARTIFACTS.mkdir(exist_ok=True)

HEADER = 'retoque/dlssnr/DlssNr_MaskInspection.h'
DX12 = 'retoque/shaders/dlssnr/DlssNr_Dx12.cpp'
VK = 'retoque/dlssnr/DlssNrFeature_Vk.cpp'
MENU = 'retoque/dlssnr/DlssNr_Menu.cpp'
HLSL = 'retoque/shaders/dlssnr/precompile/dlssnr.hlsl'
CONFIG_H = 'OptiScaler/Config.h'
CONFIG_CPP = 'OptiScaler/Config.cpp'
INI = 'OptiScaler.ini'
CONFIG_MD = 'Config.md'

STRICT = ['-std=c++20', '-Wall', '-Wextra', '-Werror']
SANITIZED = ['g++', *STRICT, '-O1', '-g', '-fsanitize=address,undefined', '-fno-omit-frame-pointer']
# Mutants only have to build: a rule removed leaves unused names behind.
QUICK = ['g++', '-std=c++20', '-O0', '-w']


def read(relative):
    return (ROOT / relative).read_text(encoding='utf-8-sig')


def sources():
    return {name: read(name) for name in (HEADER, DX12, VK, MENU, HLSL, CONFIG_H, CONFIG_CPP, INI, CONFIG_MD)}


def uncommented(text):
    return re.sub(r'//[^\n]*', '', text)


def ordered(text, *parts):
    """Every part exactly once, in this order."""
    at = -1
    for part in parts:
        assert text.count(part) == 1, f'expected once, found {text.count(part)}: {part!r}'
        assert text.index(part) > at, f'out of order: {part!r}'
        at = text.index(part)


# --- 3. Wiring ---------------------------------------------------------------------------------------------------

SCOPE = ('DlssNr::MaskInspection::Scope maskInspection(g_maskInspection, DlssNr::MaskInspection::Requested(cfg),\n'
         '                                                 &MaskInspectionNowMs, &MaskInspectionSay);')
GUARD = re.compile(r'^\s*if \(maskInspection(?: && [^()]*(?:\([^()]*\))?[^()]*)?\)$')

# What a guarded statement may be: a value written into the snapshot, or the restart. Nothing else.
ALLOWED_STATEMENT = re.compile(
    r'^(?:maskInspection->\w+ =(?: |$)|maskInspection\.Restart\(\);$|seen\.\w+(?:\[i\])? =(?: |$)|'
    r'DlssNr::MaskInspection::Snapshot& seen = \*maskInspection;$|for \(uint32_t i = 0; )')
# And what none of them may name: the command list, a device, a resource, a log, an allocation, the pass's own state
# as a target, a way out of the function.
FORBIDDEN = ('cmdList', 'device', 'ID3D12', 'GetDesc', 'Barrier(', 'DispatchPass(', '->Dispatch(', 'Evaluate', 'Create',
             'new ', 'LOG_', 'Release', 'return', 'g_nr.uiMask', 'ReadRange', 'Map(')


def guarded_regions(dispatch):
    """The statements under each `if (maskInspection ...)`, and every line naming the inspector outside them."""
    lines = dispatch.split('\n')
    regions, covered = [], set()
    i = 0
    while i < len(lines):
        if GUARD.match(lines[i]):
            indent = len(lines[i]) - len(lines[i].lstrip())
            start = i + 1
            if lines[start].strip() == '{':
                end = next(j for j in range(start + 1, len(lines)) if lines[j] == ' ' * indent + '}')
            else:
                end = next(j for j in range(start, len(lines)) if lines[j].rstrip().endswith(';'))
            regions.append('\n'.join(lines[i:end + 1]))
            covered.update(range(i, end + 1))
            i = end + 1
        else:
            i += 1
    stray = [line.strip() for n, line in enumerate(lines)
             if n not in covered and re.search(r'\bmaskInspection\b|\bseen\.', uncommented(line))]
    return regions, stray


def check_wiring(src):
    dx12, vk, header = src[DX12], src[VK], src[HEADER]
    dispatch = function(dx12, 'DlssNr_Dx12::Dispatch')
    code = uncommented(dispatch)

    # The gate is the first thing after the lock and the config, before anything can return and before the scopes
    # whose destructors time the pass; the request is the one production test.
    ordered(dispatch, 'std::lock_guard<std::mutex> nrLock(g_nrMutex);', 'const Config& cfg = *Config::Instance();',
            SCOPE, 'DlssNr::AdvisorRuntime::EvaluationScope advisorScope;')
    assert code.index(SCOPE) < code.index('return false;'), 'a return precedes the gate'

    # Everything else that names the inspector is behind `if (maskInspection ...)`: off, none of it runs.
    regions, stray = guarded_regions(dispatch)
    assert stray == [SCOPE.split('\n')[0]], f'inspector used outside its gate: {stray}'
    assert len(regions) == 5, f'{len(regions)} guarded uses; review the new one and this guard together'
    for region in regions:
        clean = uncommented(region.split('\n', 1)[1])
        for token in FORBIDDEN:
            assert token not in clean, f'guarded inspector code names {token!r}: {region}'
        inner = '\n'.join(line for line in clean.split('\n') if line.strip() not in ('{', '}', ''))
        for statement in (part.strip() for part in re.split(r'(?<=;)\n', inner)):
            lines = [line.strip() for line in statement.split('\n')]
            assert ALLOWED_STATEMENT.match(lines[0]), f'not a snapshot write: {lines[0]!r}'
            if lines[0].startswith('for ('):
                assert lines[1].startswith('seen.passes[i] = '), f'not a snapshot write: {lines[1]!r}'
    assert 'if (maskInspection && wrote)' in dispatch, 'a frame the resolve did not record must stay unrecorded'

    # Where each fact is taken: the route with the gate, the UI alpha right after its decision, the driver proxy at
    # its return, the rest right after the resolve.
    ordered(dispatch, SCOPE, 'maskInspection->route = DlssNr::MaskInspection::RouteOf(',
            'maskInspection->sequence = State::Instance().frameCount;',
            'if (maskInspection && g_resetOnReturn.load(std::memory_order_relaxed))\n        maskInspection.Restart();\n\n'
            '    if (g_resetOnReturn.exchange(false))\n        g_nr.reset = true;',
            'uiAlpha = g_nr.uiMask->Dispatch(cmdList, modelInput);', 'SetExtras(cfg, uiAlpha,',
            'maskInspection->ui = uiAlpha != nullptr', 'if (useProxy)',
            'maskInspection->outcome = DlssNr::MaskInspection::Outcome::DriverProxy;',
            'wrote = DispatchPass(cmdList, resolveParams,', 'if (maskInspection && wrote)',
            'seen.evaluatedPasses = carried ? g_nr.cadencePasses : chain.completed;',
            'seen.highlightRecovery = resolveParams.HighlightRecovery != 0;', 'return wrote;')
    assert dispatch.count('g_resetOnReturn') == 3, 'the reset flag is taken once, by the pass'
    for sent in ('TransferStrength', 'ResidualLimit', 'MinRatio', 'CompareMode', 'DebugView', 'ReversibleMode',
                 'ApplyModel', 'Passthrough', 'HighlightRecovery', 'MaxRatio'):
        assert re.search(r'seen\.\w+ = .*resolveParams\.' + sent + r'\b', dispatch), f'not what the resolve was sent: {sent}'

    # One observer, constant-initialized, under the pass's lock; one clock and one line function, used once.
    renderer = src.get('renderer', dx12)
    assert renderer.count('g_maskInspection') == 2
    assert 'static DlssNr::MaskInspection::Observer g_maskInspection;' in dx12
    assert 'static_assert(DlssNr::MaskInspection::kMaxPasses == DlssNr::PassConfig::MaxPasses);' in dx12
    assert 'static uint64_t MaskInspectionNowMs() { return static_cast<uint64_t>(NowMs()); }' in dx12
    assert 'static void MaskInspectionSay(const char* line) { LOG_INFO("{}", line); }' in dx12
    assert renderer.count('MaskInspectionNowMs') == 2 and renderer.count('MaskInspectionSay') == 2
    assert uncommented(renderer).count('MaskInspection::') == dispatch.count('MaskInspection::') + 2

    # Nothing else reads the key: it cannot rebuild the model, move the cadence or change a pass.
    for name in (DX12, VK, MENU):
        assert 'DlssNrMaskInspection' not in src[name], f'{name} reads the key itself'
    assert 'MaskInspection' not in src[MENU], 'slice 0 has no menu control'

    # Vulkan has no producer: it says so once, behind the same request, and observes nothing. The notice is said
    # from the one helper all three routes call per encoded frame, so no route's own body changes for it (the
    # after-upscale body is held to a reviewed hash by tests/nr-vk-pre-upscale).
    notice = function(vk, 'SayMaskInspectionUnavailable')
    assert 'if (g_vkMaskInspectionNotice.Take(DlssNr::MaskInspection::Requested(cfg)))\n' \
           '        LOG_INFO("{}", DlssNr::MaskInspection::kVulkanUnavailable);' in notice
    assert notice.count(';') == 1
    assert 'DlssNr::MaskInspection::RouteNotice g_vkMaskInspectionNotice;' in vk
    assert vk.count('SayMaskInspectionUnavailable(cfg);') == 1 and vk.count('g_vkMaskInspectionNotice') == 2
    assert uncommented(vk).count('MaskInspection::') == 3
    assert function(vk, 'SetVulkanTransfer').count('SayMaskInspectionUnavailable(cfg);') == 1
    assert vk.count('SetVulkanTransfer(encode, cfg);') == 3
    for name in ('EvaluateAfterUpscaleVk', 'RecordPreUpscaleVk', 'VkPresentModel::Record'):
        route = function(vk, name)
        assert route.count('SetVulkanTransfer(encode, cfg);') == 1 and 'MaskInspection' not in route, name

    # The header can only compute: no lock, thread, file, logger, graphics API or growing container.
    includes = set(re.findall(r'^#include (\S+)', header, re.M))
    assert includes == {'<algorithm>', '<array>', '<atomic>', '<charconv>', '<cmath>', '<cstddef>', '<cstdint>',
                        '<cstring>', '<optional>', '<string_view>', '<type_traits>'}, includes
    plain = uncommented(header)
    for token in ('mutex', 'thread', 'condition_variable', 'std::vector', 'std::map', 'fopen', 'stream', 'filesystem',
                  'spdlog', 'LOG_', 'ID3D12', 'Vk', 'sleep', 'malloc', 'printf', 'pch.h'):
        assert token not in plain, f'the header names {token}'
    assert not re.search(r'std::string\b(?!_view)', plain) and not re.search(r'\bnew\b', plain)

    # What Compose mirrors is still what the resolve does, in the shader's order, and what the pass sends it.
    ordered(src[HLSL], 'if (gCompareMode == 0 && gDebugView == 0 && gTransferStrength <= 0.0)', 'if (gApplyModel == 0)',
            'if (gDebugView == 1)', 'if (gDebugView == 2)', 'if (gDebugView == 3)',
            'const float guard = max(gMaxRatio, 1.0);', 'if (gResidualLimit > 0.0)', 'if (gReversibleMode == 2)',
            'else if (gReversibleMode == 4)', 'if (gHighlightRecovery != 0 && gPassthrough == 0)',
            'if (gMinRatio > 0.0 && isfinite(gMinRatio))')
    assert 'resolveParams.Passthrough = isHdrBuffer ? 0u : 1u;' in dispatch
    assert 'resolveParams.HighlightRecovery = isHdrBuffer && cfg.DlssNrHighlightRecovery.value_or_default() ? 1u : 0u;' \
        in dispatch

    # The key: declared off, read lowercased, saved as asked, shipped and documented as auto.
    assert 'CustomOptional<std::string> DlssNrMaskInspection { std::string("off") };' in src[CONFIG_H]
    assert 'DlssNrMaskInspection.set_from_config(readString("DlssNr", "MaskInspection", true));' in src[CONFIG_CPP]
    assert 'ini.SetValue("DlssNr", "MaskInspection", Instance()->DlssNrMaskInspection.value_for_config_or("auto").c_str());' \
        in src[CONFIG_CPP]
    assert src[CONFIG_CPP].count('MaskInspection') == 4
    for name in (INI, CONFIG_MD):
        assert len(re.findall(r'^MaskInspection=auto$', src[name], re.M)) == 1, name
        assert '; off or log - Default (auto) is off\nMaskInspection=auto\n' in src[name], name


WIRING_MUTANTS = {
    'request replaced by true': (DX12, 'Scope maskInspection(g_maskInspection, DlssNr::MaskInspection::Requested(cfg),',
                                 'Scope maskInspection(g_maskInspection, true,'),
    'gate after the advisor scope': (DX12, '    DlssNr::AdvisorRuntime::EvaluationScope advisorScope;\n',
                                     '', 'prepend-advisor'),
    'route written with the key off': (DX12, '    if (maskInspection)\n    {\n        maskInspection->route',
                                       '    {\n        maskInspection->route'),
    'resolve facts written with the key off': (DX12, 'if (maskInspection && wrote)', 'if (wrote)'),
    'unrecorded resolve said as recorded': (DX12, 'if (maskInspection && wrote)', 'if (maskInspection)'),
    'reads a resource': (DX12, 'seen.width = workWidth;', 'seen.width = static_cast<uint32_t>(target->GetDesc().Width);'),
    'records a command': (DX12, 'seen.height = workHeight;', 'seen.height = (cmdList->ClearState(nullptr), workHeight);'),
    'writes the pass state': (DX12, 'seen.compare = resolveParams.CompareMode;', 'g_nr.reset = true;'),
    'logs from the frame': (DX12, 'seen.debugView = resolveParams.DebugView;', 'LOG_INFO("mask");'),
    'returns from the frame': (DX12, 'maskInspection->outcome = DlssNr::MaskInspection::Outcome::DriverProxy;',
                               'return maskInspection->outcome == DlssNr::MaskInspection::Outcome::DriverProxy;'),
    'takes the reset flag': (DX12, 'if (maskInspection && g_resetOnReturn.load(std::memory_order_relaxed))',
                             'if (maskInspection && g_resetOnReturn.exchange(false))'),
    'ui read before its decision': (DX12, '    SetExtras(cfg, uiAlpha, frame.PresentSource ? modelInput : nullptr,',
                                    '    SetExtras(cfg, nullptr, frame.PresentSource ? modelInput : nullptr,', 'ui-early'),
    'observer on the heap': (DX12, 'static DlssNr::MaskInspection::Observer g_maskInspection;',
                             'static DlssNr::MaskInspection::Observer& g_maskInspection = *new DlssNr::MaskInspection::Observer;'),
    'a second reader of the key': (DX12, 'const bool chainEnabled = g_tracked && !useProxy;',
                                   'const bool chainEnabled = g_tracked && !useProxy && '
                                   '!cfg.DlssNrMaskInspection.has_value();'),
    'a menu control': (MENU, '#include "pch.h"', '#include "pch.h"\n// ImGui::Checkbox("MaskInspection", &value);'),
    'vulkan says it unasked': (VK, 'g_vkMaskInspectionNotice.Take(DlssNr::MaskInspection::Requested(cfg))',
                               'g_vkMaskInspectionNotice.Take(true)'),
    'vulkan left silent': (VK, '    SayMaskInspectionUnavailable(cfg);\n}', '}'),
    'vulkan route said from its own body': (VK, '    if (g_vk.failed)\n        return;\n\n    // The game\'s own NR through Streamline',
                                            '    if (g_vk.failed)\n        return;\n    SayMaskInspectionUnavailable(cfg);\n\n'
                                            '    // The game\'s own NR through Streamline'),
    'vulkan observes': (VK, 'DlssNr::MaskInspection::RouteNotice g_vkMaskInspectionNotice;',
                        'DlssNr::MaskInspection::RouteNotice g_vkMaskInspectionNotice;\nDlssNr::MaskInspection::Observer g_vkObserver;'),
    'header takes a lock': (HEADER, '#include <atomic>', '#include <atomic>\n#include <mutex>'),
    'header keeps a string': (HEADER, '    uint32_t _lines = 0;', '    uint32_t _lines = 0;\n    std::string _last;'),
    'shader branch changed': (HLSL, 'if (gHighlightRecovery != 0 && gPassthrough == 0)', 'if (gHighlightRecovery != 0)'),
    'highlight recovery sent on a tone-mapped frame': (
        DX12, 'resolveParams.HighlightRecovery = isHdrBuffer && cfg.DlssNrHighlightRecovery.value_or_default() ? 1u : 0u;',
        'resolveParams.HighlightRecovery = cfg.DlssNrHighlightRecovery.value_or_default() ? 1u : 0u;'),
    'declared on': (CONFIG_H, 'DlssNrMaskInspection { std::string("off") };', 'DlssNrMaskInspection { std::string("log") };'),
    'never saved': (CONFIG_CPP, '        ini.SetValue("DlssNr", "MaskInspection", '
                                'Instance()->DlssNrMaskInspection.value_for_config_or("auto").c_str());\n', ''),
    'not shipped': (INI, 'MaskInspection=auto\n', ''),
    'shipped on': (INI, 'MaskInspection=auto\n', 'MaskInspection=log\n'),
}


def mutated_sources(src, name, spec):
    target, old, new = spec[:3]
    text = src[target]
    assert text.count(old) == 1, f'wiring mutant {name!r}: anchor found {text.count(old)} times'
    changed = dict(src)
    if len(spec) == 4 and spec[3] == 'prepend-advisor':
        # Move the advisor's scope above the gate: its destructor would then time the inspector's line.
        text = text.replace(old, '')
        gate = '    // Mask inspection (docs/dlssnr/design/character-inspector.md, slice 0)'
        assert text.count(gate) == 1
        changed[target] = text.replace(gate, old + gate)
    elif len(spec) == 4 and spec[3] == 'ui-early':
        # Move the UI snapshot above the decision it reports.
        block = text[text.index('    // Mask inspection: the decision just made'):text.index('    // The proxy path, when asked for.')]
        text = text.replace(block, '')
        anchor = '    ID3D12Resource* uiAlpha = nullptr;\n'
        assert text.count(anchor) == 1
        changed[target] = text.replace(anchor, anchor + block)
    else:
        changed[target] = text.replace(old, new)
    if target == DX12:
        changed['renderer'] = src['renderer'].replace(src[DX12], changed[DX12])
    return changed


# --- 1. Behaviour ------------------------------------------------------------------------------------------------

# One rule of the header at a time. The cases must notice each.
HEADER_MUTANTS = {
    'off gate removed': ('        if (!requested)\n        {\n            if (_live)',
                         '        if (false)\n        {\n            if (_live)'),
    'NR disabled still asks': ('    return cfg.DlssNrEnabled.value_or_default() && cfg.DlssNrMaskInspection.has_value() &&',
                               '    return cfg.DlssNrMaskInspection.has_value() &&'),
    'anything but off asks': ('inline bool IsLog(std::string_view value) { return value == "log"; }',
                              'inline bool IsLog(std::string_view value) { return value != "off"; }'),
    'scope observes without the gate': ('        if (!observer.Gate(requested))\n            return;\n',
                                        '        observer.Gate(requested);\n'),
    'off notice spent': ('        return requested && !_said.load(std::memory_order_relaxed) && !_said.exchange(true, std::memory_order_relaxed);',
                         '        return !_said.exchange(true, std::memory_order_relaxed) && requested;'),
    'notice every frame': ('        return requested && !_said.load(std::memory_order_relaxed) && !_said.exchange(true, std::memory_order_relaxed);',
                           '        return requested;'),
    'no gap': ('            if (nowMs - _saidAtMs < kMinGapMs)\n                return false;\n', ''),
    'unchanged status said again': ('        if (_haveSaid && SameStatus(*status, _said))\n            return false;\n', ''),
    'frame number is news': ('    a.sequence = b.sequence = 0;\n', ''),
    'carried frame is news': ('    if (a.outcome == Outcome::Carried)\n        a.outcome = Outcome::Recorded;\n', ''),
    'clock rollback said early': ('            if (nowMs < _saidAtMs)\n            {\n                _saidAtMs = nowMs;\n                return false;\n            }\n',
                                  ''),
    'clock rollback goes silent': ('                _saidAtMs = nowMs;\n                return false;', '                return false;'),
    'switch off continues the generation': ('                _live = false;\n                Restart();', '                _live = false;'),
    'new generation keeps the held recording': ('            _haveHeld = false;\n            _haveContext = false;', '            _haveContext = false;'),
    'restart keeps the baseline': ('            _restart = false;\n            _haveSaid = false;', '            _restart = false;'),
    'restart is not a generation': ('        if (_restart || !_haveRoute ||', '        if (!_haveRoute ||'),
    'route change is not a generation': ('!_haveRoute || s.route != _route ||', '!_haveRoute ||'),
    'context change is not a generation': ('(recording && _haveContext && !SameContext(s, _context))',
                                           '(recording && _haveContext && !SameContext(s, s))'),
    'resize is not a new context': ('a.width == b.width && a.height == b.height &&', ''),
    'rebuild is not a new context': ('    return a.modelGeneration == b.modelGeneration && a.width', '    return a.width'),
    'pass count is not a new context': (' &&\n           a.requestedPasses == b.requestedPasses;', ';'),
    'every route observed': ('inline constexpr bool Observed(Route route) { return route == Route::Present; }',
                             'inline constexpr bool Observed(Route route) { return route == route; }'),
    'bridge taken for present': ('        return bridgeHost ? Route::Bridge : Route::Present;', '        return Route::Present;'),
    'decline outranks a fresh recording': ('nowMs - _heldAtMs < kMinGapMs)\n        {\n            status = &_held;',
                                           'nowMs - _heldAtMs < kMinGapMs)\n        {\n            status = &s;'),
    'held recording never expires': ('nowMs >= _heldAtMs &&\n                 nowMs - _heldAtMs < kMinGapMs)', 'nowMs >= _heldAtMs)'),
    'proxy described by a recording': ('Observed(s.route) && s.outcome == Outcome::NotRecorded && nowMs', 'Observed(s.route) && nowMs'),
    'declined frame keeps what was written': ('    if (!IsRecording(raw))\n        return true;\n', ''),
    'unsafe metadata accepted': ('raw.evaluatedPasses == 0 || raw.evaluatedPasses > raw.requestedPasses || raw.ui == Ui::NotDecided)\n        return false;',
                                 'raw.ui == Ui::NotDecided)\n        return false;'),
    'undecided ui accepted': (' || raw.ui == Ui::NotDecided)', ')'),
    'zero extent accepted': ('    if (raw.width == 0 || raw.height == 0 || raw.requestedPasses == 0', '    if (raw.requestedPasses == 0'),
    'unevaluated passes are news': ('    for (uint32_t i = raw.evaluatedPasses; i < kMaxPasses; ++i)\n        out.passes[i] = {};\n', ''),
    'no line budget': ('        if (_lines >= kMaxLines)\n            return false;\n        _live = true;', '        _live = true;'),
    'budget not announced': ('Format(*status, _generation, _lines >= kMaxLines, line);', 'Format(*status, _generation, false, line);'),
    'text written past the end': ('        const size_t count = std::min(text.size(), room);', '        const size_t count = text.size();'),
    'cut text not reported': ('        _cut = _cut || count != text.size();\n', ''),
    'ApplyModel off still guarded': ('    if (!s.applyModel)\n    {\n        c.notShown = "ApplyModel is off";\n        return c;\n    }\n', ''),
    'strength zero still guarded': ('    if (s.compare == 0 && s.debugView == 0 && s.detail <= 0 && s.detail != kNotFinite)',
                                    '    if (false)'),
    'strength zero ignores compare': ('    if (s.compare == 0 && s.debugView == 0 && s.detail <= 0 && s.detail != kNotFinite)',
                                      '    if (s.detail <= 0 && s.detail != kNotFinite)'),
    'debug view still guarded': ('    if (s.debugView >= 1 && s.debugView <= 3)', '    if (s.debugView >= 1 && s.debugView <= 2)'),
    'debug view 4 hides the edit': ('    if (s.debugView >= 1 && s.debugView <= 3)', '    if (s.debugView >= 1)'),
    'replace mode guarded': ('    c.guard = !c.replace;', '    c.guard = true;'),
    'replace mode limited': ('    c.residualLimit = !c.replace && s.residualLimit > 0;', '    c.residualLimit = s.residualLimit > 0;'),
    'only one replace mode': ('    c.replace = s.reversibleMode == 2 || s.reversibleMode == 4;', '    c.replace = s.reversibleMode == 2;'),
    'recovery on a tone-mapped frame': ('    c.highlightRecovery = s.highlightRecovery && !s.passthrough;', '    c.highlightRecovery = s.highlightRecovery;'),
    'floor always on': ('    c.darkeningFloor = s.minRatio > 0;', '    c.darkeningFloor = true;'),
    'ui off said as not produced': ('        case Ui::NotProduced:\n', '        case Ui::NotProduced:\n        case Ui::Off:\n'),
    'handed said for the model on a carry': ('w.Text(carried ? "handed to the carry (coverage unknown)"', 'w.Text(false ? "handed to the carry (coverage unknown)"'),
    'follows LocalStructure said as a number': ('            if (s.passes[first].skinStructure < 0 && s.passes[first].skinStructure != kNotFinite)', '            if (false)'),
    'passes not grouped': ('            while (next < s.evaluatedPasses && s.passes[next] == s.passes[first])\n                ++next;\n', ''),
    'skin mask claimed': ('        w.Text("the model\'s skin mask is internal and not exposed");\n', ''),
    'non-finite never equal': ('    if (!std::isfinite(value))\n        return kNotFinite;\n', '    if (!std::isfinite(value))\n        return static_cast<int32_t>(reinterpret_cast<uintptr_t>(&value));\n'),
    'logger throw escapes': ('        catch (...)\n        {\n        }', '        catch (int)\n        {\n        }'),
}

# What breaks the key's round trip, applied to the production text before it is spliced into the harness.
CONFIG_MUTANTS = {
    'default is log': ('declaration', 'std::string("off")', 'std::string("log")'),
    'read as written, not lowercased': ('read', '"MaskInspection", true)', '"MaskInspection")'),
    'read from another key': ('read', '"MaskInspection"', '"MaskInspect"'),
    'saves the resolved value': ('save', 'value_for_config_or("auto")', 'value_or_default()'),
    'saves under another key': ('save', '"MaskInspection"', '"MaskInspect"'),
    'saves off for auto': ('save', 'value_for_config_or("auto")', 'value_for_config_or("off")'),
}


def statement(text, needle):
    """The one whole statement of Config.cpp that contains `needle`."""
    assert text.count(needle) == 1, f'expected one statement with {needle!r}'
    at = text.index(needle)
    return text[text.rfind('\n', 0, text.rfind(';', 0, at)) + 1:text.index(';', at) + 1].strip()


def config_harness(src, mutant=None):
    config_h, config_cpp = src[CONFIG_H], src[CONFIG_CPP]
    parts = {
        'optional': config_h[config_h.index('enum HasDefaultValue'):config_h.index('constexpr inline int UnboundKey')],
        'declaration': statement(config_h, 'DlssNrMaskInspection {').split('\n')[-1].strip(),
        'read': statement(config_cpp, 'DlssNrMaskInspection.set_from_config('),
        'save': statement(config_cpp, 'Instance()->DlssNrMaskInspection.'),
        'read_string': function(config_cpp, 'Config::readString'),
    }
    assert parts['declaration'].startswith('CustomOptional<std::string> DlssNrMaskInspection'), parts['declaration']
    if mutant is not None:
        key, old, new = mutant
        assert parts[key].count(old) == 1, f'config mutant anchor drift: {old!r}'
        parts[key] = parts[key].replace(old, new)
    harness = (HERE / 'config_cases.cpp').read_text(encoding='utf-8')
    for marker, key in (('// @@CUSTOM_OPTIONAL@@', 'optional'), ('// @@DECLARATION@@', 'declaration'),
                        ('// @@READ@@', 'read'), ('// @@SAVE@@', 'save'), ('// @@READ_STRING@@', 'read_string')):
        assert harness.count(marker) == 1, marker
        harness = harness.replace(marker, parts[key])
    return harness


def wiring_harness(src):
    """wiring_cases.cpp with the statements Dispatch gained, exactly as they are written there."""
    dx12 = src[DX12]
    regions, _ = guarded_regions(function(dx12, 'DlssNr_Dx12::Dispatch'))
    assert len(regions) == 5
    globals_start = dx12.index('static DlssNr::MaskInspection::Observer g_maskInspection;')
    say = 'static void MaskInspectionSay(const char* line) { LOG_INFO("{}", line); }\n'
    parts = {
        '// @@GLOBALS@@': dx12[globals_start:dx12.index(say) + len(say)],
        '// @@GATE@@': '    ' + SCOPE + '\n' + regions[0],
        '// @@RESTART@@': regions[1],
        '// @@UI@@': regions[2],
        '// @@PROXY@@': regions[3],
        '// @@RESOLVE@@': regions[4],
    }
    harness = (HERE / 'wiring_cases.cpp').read_text(encoding='utf-8')
    for marker, text in parts.items():
        assert harness.count(marker) == 1, marker
        harness = re.sub(r'^[ ]*' + re.escape(marker) + '$', lambda _: text, harness, flags=re.M)  # its own indentation
        assert marker not in harness, marker
    return harness


# The same statements with one of them changed: the harness must notice what the source guards above describe.
SPLICE_MUTANTS = {
    'request replaced by true': ('DlssNr::MaskInspection::Requested(cfg)', 'true'),
    'resolve facts without the gate': ('if (maskInspection && wrote)', 'if (wrote)'),
    'unrecorded resolve said as recorded': ('if (maskInspection && wrote)', 'if (maskInspection)'),
    'restart on every frame': ('if (maskInspection && g_resetOnReturn.load(std::memory_order_relaxed))', 'if (maskInspection)'),
    'restart never': ('if (maskInspection && g_resetOnReturn.load(std::memory_order_relaxed))', 'if (false)'),
    'reset flag taken by the inspector': ('g_resetOnReturn.load(std::memory_order_relaxed)', 'g_resetOnReturn.exchange(false)'),
    'UiProtection not consulted': (': cfg.DlssNrUiProtection.value_or_default() ? DlssNr::MaskInspection::Ui::NotProduced',
                                   ': true ? DlssNr::MaskInspection::Ui::NotProduced'),
    'proxy not said': ('maskInspection->outcome = DlssNr::MaskInspection::Outcome::DriverProxy;',
                       'maskInspection->outcome = DlssNr::MaskInspection::Outcome::NotRecorded;'),
    'bridge taken for present': ('RouteOf(frame.PresentSource, frame.BridgeHost,', 'RouteOf(frame.PresentSource, false,'),
    'requested passes said as evaluated': ('seen.evaluatedPasses = carried ? g_nr.cadencePasses : chain.completed;',
                                           'seen.evaluatedPasses = passSnapshot.Count;'),
    'tone-mapped frame not said': ('seen.passthrough = resolveParams.Passthrough != 0;', 'seen.passthrough = false;'),
    'frame number not taken': ('maskInspection->sequence = State::Instance().frameCount;', 'maskInspection->sequence = 0;'),
    'model generation not taken': ('seen.modelGeneration = g_nr.featureGeneration;', 'seen.modelGeneration = 0;'),
}


def build(flags, source, output, includes, quiet=False):
    command = flags + [f'-I{path}' for path in includes] + ['-isystem', str(ROOT / 'external/simpleini'), str(source),
                                                              '-o', str(output)]
    return subprocess.run(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT if quiet else None, text=True)


def survives(binary, *arguments):
    return subprocess.run([str(binary), *arguments], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0


def main():
    src = sources()
    src['renderer'] = renderer_source()

    check_wiring(src)
    for name, spec in WIRING_MUTANTS.items():
        broken = mutated_sources(src, name, spec)  # a drifted anchor is this suite's failure, not a rejection
        try:
            check_wiring(broken)
        except (AssertionError, StopIteration):
            continue
        raise SystemExit(f'FAIL: wiring mutant survived: {name}')
    print(f'PASS: gate, five guarded value-only uses, Vulkan notice, header purity, shader branches and the key\'s four '
          f'points; {len(WIRING_MUTANTS)} wiring mutants rejected', flush=True)

    with tempfile.TemporaryDirectory(prefix='host-', dir=ARTIFACTS) as folder:
        folder = Path(folder)

        cases = folder / 'cases'
        assert build(SANITIZED, HERE / 'cases.cpp', cases, [ROOT]).returncode == 0
        subprocess.run([str(cases)], check=True)

        def header_mutant(item):
            index, (name, (old, new)) = item
            assert src[HEADER].count(old) == 1, f'header mutant {name!r}: anchor found {src[HEADER].count(old)} times'
            shadow = folder / f'h{index}'
            (shadow / HEADER).parent.mkdir(parents=True)
            (shadow / HEADER).write_text(src[HEADER].replace(old, new), encoding='utf-8')
            result = build(QUICK, HERE / 'cases.cpp', shadow / 'cases', [shadow, ROOT], quiet=True)
            assert result.returncode == 0, f'header mutant {name!r} does not compile:\n{result.stdout}'
            return None if not survives(shadow / 'cases') else name

        with ThreadPoolExecutor(max_workers=4) as pool:
            survivors = [name for name in pool.map(header_mutant, enumerate(HEADER_MUTANTS.items())) if name]
        assert not survivors, f'header mutants survived: {survivors}'
        print(f'PASS: {len(HEADER_MUTANTS)} behavioural mutants of the header rejected', flush=True)

        generated = folder / 'wiring_cases.cpp'
        wiring_text = wiring_harness(src)
        generated.write_text(wiring_text, encoding='utf-8')
        wiring = folder / 'wiring'
        assert build(SANITIZED, generated, wiring, [ROOT, ROOT / 'retoque']).returncode == 0
        subprocess.run([str(wiring)], check=True)
        for index, (name, (old, new)) in enumerate(SPLICE_MUTANTS.items()):
            assert wiring_text.count(old) == 1, f'splice mutant {name!r}: anchor found {wiring_text.count(old)} times'
            generated = folder / f'wiring_m{index}.cpp'
            generated.write_text(wiring_text.replace(old, new), encoding='utf-8')
            binary = folder / f'wiring_m{index}'
            result = build(QUICK, generated, binary, [ROOT, ROOT / 'retoque'], quiet=True)
            assert result.returncode == 0, f'splice mutant {name!r} does not compile:\n{result.stdout}'
            assert not survives(binary), f'splice mutant survived: {name}'
        print(f'PASS: {len(SPLICE_MUTANTS)} mutants of the statements in Dispatch rejected', flush=True)

        generated = folder / 'config_cases.cpp'
        generated.write_text(config_harness(src), encoding='utf-8')
        config = folder / 'config'
        assert build(SANITIZED, generated, config, [ROOT]).returncode == 0
        subprocess.run([str(config), str(ROOT / INI)], check=True)

        for index, (name, mutant) in enumerate(CONFIG_MUTANTS.items()):
            generated = folder / f'config_m{index}.cpp'
            generated.write_text(config_harness(src, mutant), encoding='utf-8')
            binary = folder / f'config_m{index}'
            result = build(QUICK, generated, binary, [ROOT], quiet=True)
            assert result.returncode == 0, f'config mutant {name!r} does not compile:\n{result.stdout}'
            assert not survives(binary, str(ROOT / INI)), f'config mutant survived: {name}'
        print(f'PASS: {len(CONFIG_MUTANTS)} config round-trip mutants rejected', flush=True)

        # The DLL is built by MSVC as C++ latest, which this suite cannot run. Two other front ends at least read
        # the header and its cases the same way; neither is that build.
        if shutil.which('clang++'):
            other = folder / 'cases-clang'
            subprocess.run(['clang++', '-std=c++23', '-Wall', '-Wextra', '-Werror', '-O1', '-fsanitize=address,undefined',
                            f'-I{ROOT}', str(HERE / 'cases.cpp'), '-o', str(other)], check=True)
            subprocess.run([str(other)], check=True, stdout=subprocess.DEVNULL)
            print('PASS: the header and its cases under clang++ as C++23', flush=True)
        else:
            print('NOT_RUN: clang++ is not installed', flush=True)
        if shutil.which('x86_64-w64-mingw32-g++'):
            subprocess.run(['x86_64-w64-mingw32-g++', '-std=c++23', '-fsyntax-only', '-Wall', '-Wextra', '-Werror',
                            f'-I{ROOT}', str(HERE / 'cases.cpp')], check=True)
            print('PASS: the header and its cases parse for a Windows target (MinGW, syntax only; not the MSVC build)',
                  flush=True)
        else:
            print('NOT_RUN: MinGW is not installed', flush=True)


if __name__ == '__main__':
    main()

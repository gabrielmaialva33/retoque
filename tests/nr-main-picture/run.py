#!/usr/bin/env python3
"""The main-picture rule: which of the game's DLSS features NR follows, and that every NR seam asks first.

The rule header and its glue are compiled under ASan/UBSan against fake parameters and broken one rule at a time. The
four seams (D3D12 before and after the upscaler, Vulkan, native D3D11) and the release hook are read off the source and
each guard is broken once. No D3D12, no Vulkan, no NGX, no game.
"""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
RULE = 'retoque/dlssnr/DlssNr_Pictures.h'
GATE = 'retoque/dlssnr/DlssNr_PictureGate.h'
FLAGS = ['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
         '-fno-omit-frame-pointer']


def build(root, out):
    return subprocess.run(FLAGS + ['-I', str(root / 'retoque'), '-I', str(ROOT / 'OptiScaler'),
                                   '-I', str(ROOT / 'external/nvngx_dlss_sdk'), str(HERE / 'cases.cpp'), '-o', str(out)],
                          capture_output=True)


RULE_MUTANTS = {
    'views are not views': ('if (call.colour != 0 && call.colour == _mainColour)',
                            'if (call.colour != 0 && call.colour == _mainColour && false)'),
    'unknown colour is a view': ('if (call.colour != 0 && call.colour == _mainColour)',
                                 'if (call.colour == _mainColour)'),
    'larger never takes': ('if (ClearlyLarger(call.area, _mainArea))', 'if (false)'),
    'any larger takes': ('area * 4 > mainArea * 5', 'area > mainArea'),
    'unknown area is larger': ('return mainArea != 0 && area * 4 > mainArea * 5;', 'return area * 4 > mainArea * 5;'),
    'never quiet': ('if (++_quiet >= kQuietCalls)', 'if (++_quiet >= kQuietCalls && false)'),
    'main does not keep alive': ('            _mainColour = call.colour;\n            _quiet = 0;\n            return { Role::Main',
                                 '            _mainColour = call.colour;\n            return { Role::Main'),
    'views do not keep alive': ('        {\n            _quiet = 0;\n            return { Role::ViewOfMain',
                                '        {\n            return { Role::ViewOfMain'),
    'known other does not wait': ('            if (_others.count(call.key) == 0)\n', '            if (true)\n'),
    'known other waits forever': ('            if (++_vacantFor >= kQuietCalls)\n', '            if (false)\n'),
    'waited is the same picture': ('                return Take(call, Took::Waited);', '                return Take(call, Took::Vacant);'),
    'vacant is a new picture': ('                return Take(call, Took::Vacant);', '                return Take(call, Took::Waited);'),
    'quiet keeps the picture': ('took == Took::Larger || took == Took::Quiet || took == Took::Waited; }',
                                'took == Took::Larger || took == Took::Waited; }'),
    'epoch never advances': ('        if (NewPicture(took))\n            ++_epoch;\n', ''),
    'aside has an epoch': ('        Judgement judgement { Role::Aside, Took::No, 0, false, 0 };',
                           '        Judgement judgement { Role::Aside, Took::No, 0, false, _epoch };'),
    'stale frame runs': ('        if (epoch < _seen)\n            return Consumed::Stale;', '        if (epoch < _seen)\n            return Consumed::Same;'),
    'stale frame restarts': ('        if (epoch < _seen)\n            return Consumed::Stale;\n', ''),
    'first epoch restarts': ('            _seen = epoch;\n            return Consumed::Same;', '            _seen = epoch;\n            return Consumed::Restart;'),
    'unjudged frame counts': ('        if (epoch == 0 || epoch == _seen)', '        if (epoch == _seen)'),
    'release keeps main': ('        _main = 0;\n        _mainArea = 0;', '        _mainArea = 0;'),
    'release forgets others': ('        _others.erase(key);\n        if (key == 0', '        if (key == 0'),
    'first aside every time': ('judgement.firstAside = _others.insert(call.key).second;',
                               '_others.insert(call.key);\n        judgement.firstAside = true;'),
    'old main not another picture': ('        if (judgement.previous != 0)\n            _others.insert(judgement.previous);\n', ''),
    'no previous': ("(took == Took::Larger || took == Took::Quiet) ? _main : 0", '0'),
    'release after vacancy is first': ('        _released = true;\n        return true;', '        return true;'),
}
GATE_MUTANTS = {
    'judged twice': ('    if (binding->judged)\n        return', '    if (binding->judged && false)\n        return'),
    'foreign block judged': ('binding->params != params || binding->key == 0', 'binding->key == 0'),
    'epoch not cached': ('    binding->epoch = judgement.epoch;\n', ''),
    'epoch of a foreign block': ('if (binding == nullptr || params == nullptr || binding->params != params || !binding->judged)\n        return 0;',
                                 'if (binding == nullptr || params == nullptr || !binding->judged)\n        return 0;'),
    'aside not counted': ('        const auto count = g_asideEvaluates.fetch_add(1, std::memory_order_relaxed) + 1;',
                          '        const auto count = g_asideEvaluates.load(std::memory_order_relaxed) + 1;'),
}

with tempfile.TemporaryDirectory(prefix='nr-main-picture-') as folder:
    folder = Path(folder)
    built = build(ROOT, folder / 'cases')
    assert built.returncode == 0, built.stderr.decode()
    subprocess.run([str(folder / 'cases')], check=True)
    killed = 0
    for path, mutants in ((RULE, RULE_MUTANTS), (GATE, GATE_MUTANTS)):
        text = (ROOT / path).read_text()
        for index, (name, (old, new)) in enumerate(mutants.items()):
            assert text.count(old) == 1, f'mutant {name!r} no longer applies'
            root = folder / f'm{killed}'
            shutil.copytree(ROOT / 'retoque/dlssnr', root / 'retoque/dlssnr')
            (root / path).write_text(text.replace(old, new))
            built = build(root, root / 'cases')
            assert built.returncode == 0, f'mutant {name!r} does not compile:\n{built.stderr.decode()}'
            assert subprocess.run([str(root / 'cases')], capture_output=True).returncode != 0, \
                f'mutant {name!r} escaped the cases'
            killed += 1
    print(f'PASS: {killed} rule and glue mutants fail the cases')


def read(path):
    return (ROOT / path).read_text(encoding='utf-8-sig').replace('\r\n', '\n')


def body(text, signature):
    assert text.count(signature) == 1, f'expected one definition: {signature}'
    start = text.index(signature)
    brace = text.index('{', start)
    depth = 0
    for i in range(brace, len(text)):
        depth += (text[i] == '{') - (text[i] == '}')
        if depth == 0:
            return text[start:i + 1]
    raise AssertionError('unterminated: ' + signature)


def gate(fn, start, before, *, aside, asker='Pictures::Ask('):
    """From `start` (its NR-off way out), the seam asks the rule before any of `before`, and an Aside picture returns
    there and then, touching nothing."""
    ask = fn.index(asker)
    begin = fn.index(start)
    assert begin < ask, f'the seam asks before {start!r}'
    for token in before:
        assert token in fn[ask:] and token not in fn[begin:ask], f'the seam asks after {token!r}'
    block = fn[fn.rindex('\n', 0, ask):fn.index('\n\n', ask)]
    assert re.search(r'(Pictures::Ask\([^;]*\)\s*\.role|PictureRole\(params\)) == \S*Pictures::Role::Aside\)', block), \
        'Aside not tested'
    assert aside in block, f'the Aside branch does not {aside!r}'
    for poison in ('_declined', 'g_preStatus', 'coverage.sample', 'g_upscale', 'ResetOnReturn', 'ReportSkip'):
        assert poison not in block, f'an Aside picture touches {poison}'


def consumer(fn, locked, *, restart, stale):
    """The holder of the history consumes the frame's epoch under its lock: Restart restarts, Stale refuses."""
    at = fn.index('PictureEpoch.Consume(') if 'PictureEpoch.Consume(' in fn else fn.index('pictureEpoch.Consume(')
    assert fn.index(locked) < at, 'the epoch is consumed outside the lock'
    block = fn[at:fn.index('\n    }\n', at)]
    case_restart = block[block.index('case DlssNr::Pictures::Consumed::Restart:'):]
    case_restart = case_restart[:case_restart.index('break;')]
    for effect in restart:
        assert effect in case_restart, f'a new picture does not {effect!r}'
    case_stale = block[block.index('case DlssNr::Pictures::Consumed::Stale:'):]
    case_stale = case_stale[:case_stale.index('case DlssNr::Pictures::Consumed::Restart:')]
    assert re.search(stale, case_stale), 'a stale frame is not refused'
    return at


def check(texts):
    dx12, vk, dx11, geometry, size, funnel = (texts[k] for k in ('dx12', 'vk', 'dx11', 'geometry', 'size', 'funnel'))
    after = body(dx12, 'void EvaluateAfterUpscale(ID3D12GraphicsCommandList* cmdList, NVSDK_NGX_Parameter* params,')
    gate(after, 'Funnel::Evaluation funnel(', ('CoverageScope ', 'ApplyAfterRR', 'ReportSkipOnce(', 'GatherFrame('),
         aside='funnel.Declined(Funnel::Decline::OtherPicture);\n        return;', asker='PictureRole(params)')
    assert after.index('if (!dlssNrEnabled)') < after.index('PictureRole(params)'), 'NR off must not ask'
    role = body(dx12, 'static Pictures::Role PictureRole(NVSDK_NGX_Parameter* params)')
    assert re.search(r'Pictures::Ask\(\s*params,\s*\[params\]\s*\{[^}]*NVSDK_NGX_Parameter_Color', role), 'the colour is not read lazily'
    assert role.count('GetResource(') == 1 and role.count('->Get(') == 0, 'the colour is read outside the reader'
    pre = body(dx12, 'ScopedPreUpscale::ScopedPreUpscale(')
    gate(pre, 'if (!enabled || stage != 1)', ('CoverageScope ', 'DlssNrHookMethod', '_declined = true;', 'ReportSkipOnce(', 'MultiViewStandDown(', '_lock = '),
         aside='Role::Aside)\n        return;', asker='PictureRole(params)')
    assert pre.index('if (!enabled || stage != 1)') < pre.index('PictureRole(params)')
    vkfn = body(vk, 'void EvaluateAfterUpscaleVk(VkCommandBuffer cmdBuffer, NVSDK_NGX_Parameter* params,')
    gate(vkfn, 'physicalDevice == VK_NULL_HANDLE)', ('g_vkMutex', 'AskInto(', 'g_vk.'), aside='Role::Aside)\n        return;')
    assert 'NVSDK_NGX_Parameter_Color' not in vkfn[:vkfn.index('const auto pictureColour = [params]')], 'the colour is read eagerly'
    assert vkfn.index("if (!cfg.DlssNrEnabled.value_or_default())") < vkfn.index('Pictures::Ask(')
    at = consumer(vkfn, 'std::lock_guard<std::mutex> lock(g_vkMutex);',
                  restart=('g_vk.reset = true;', 'g_vk.gameExposure = 0.0f;', 'g_vk.meterFrames = 0;',
                           'g_vk.meterPending.Invalidate();', 'g_vk.exposureLatch.Reset();'), stale=r'\n\s+return;')
    assert 'g_vkPictureEpoch.Consume(DlssNr::Pictures::EvaluateEpoch(params))' in vkfn
    assert at < vkfn.index('ClaimModelForAfterUpscale(device)'), 'the model is reached before the epoch'
    dispatch = body(dx12, 'bool DlssNr_Dx12::Dispatch(')
    at = consumer(dispatch, 'std::lock_guard<std::mutex> nrLock(g_nrMutex);',
                  restart=('g_nr.reset = true;', 'InvalidateExposureMeter();'), stale=r'reportSkip\([^;]*\);\s*return false;')
    assert 'g_pictureEpoch.Consume(frame.PictureEpoch)' in dispatch
    assert at < dispatch.index('g_resetOnReturn.exchange(false)'), 'the epoch is consumed after the reset request'
    inputs = texts['inputs']
    guide = body(inputs, 'DlssNrFrameInfo GatherGuideFrame(NVSDK_NGX_Parameter* params)')
    assert 'frame.PictureEpoch = DlssNr::Pictures::EvaluateEpoch(params);' in guide, 'the frame does not carry its picture'
    assert 'StartsHistoryOver' not in dx12 + vk, 'a seam restarts the history outside the holder\'s lock'
    carry = body(dx11, 'void CarryNativeDx11Guides(ID3D11DeviceContext* context, NVSDK_NGX_Parameter* params,')
    ask = carry.index('Pictures::Ask(')
    assert carry.index('if (Dx11Guides::Decide(in) == Reason::NotWanted') < ask, 'asked on a route that carries nothing'
    for touched in ('in.evaluated = evaluated;', 'context->GetType()', 'Describe(', 'lock_guard'):
        assert ask < carry.index(touched), f'{touched} is reached before the picture is judged'
    assert re.search(r'Pictures::Role::Aside\)\n\s+return;', carry[ask:ask + 300]), 'an aside picture is carried'
    assert '#include <dlssnr/DlssNr_PictureGate.h>' in dx12 and '#include <dlssnr/DlssNr_PictureGate.h>' in vk
    assert '#include "DlssNr_PictureGate.h"' in dx11
    forget = body(geometry, 'inline void Forget(uint64_t key)')
    assert 'Pictures::Instance().Release(key);' in forget and forget.index('records.erase(key);') < forget.index(
        'Pictures::Instance().Release(key);')
    assert 'binding = { params, record->creation.render, record->creation.display, &record->stale, key };' in geometry
    assert re.search(r'uint64_t key = 0;\n\s+mutable bool judged = false;\n\s+mutable uint8_t role = 0;\n\s+mutable uint64_t epoch = 0;', size)
    assert 'OtherPicture,' in funnel and 'case Decline::OtherPicture:\n        return "other_game_picture";' in funnel
    decline = funnel[funnel.index('enum class Decline : uint8_t'):]
    decline = decline[:decline.index('};')]
    assert re.search(r'\n    GameOwnNr, [^\n]*\n    OtherPicture, [^\n]*\n    Count\n', decline), \
        'the decline is not appended before Count'


TEXTS = {
    'dx12': read('retoque/shaders/dlssnr/DlssNr_Dx12.cpp'),
    'vk': read('retoque/dlssnr/DlssNrFeature_Vk.cpp'),
    'dx11': read('retoque/dlssnr/DlssNr_Dx11Guides.cpp'),
    'geometry': read('retoque/dlssnr/DlssNr_RenderGeometry.h'),
    'size': read('retoque/dlssnr/DlssNr_RenderSize.h'),
    'funnel': read('retoque/dlssnr/DlssNr_Funnel.h'),
    'inputs': read('retoque/shaders/dlssnr/DlssNr_Dx12_Inputs.cpp'),
}
check(TEXTS)
print('PASS: the four NR seams ask the rule before any NR state and leave another picture as it is; release tells it')

SOURCE_MUTANTS = {
    'after: asks after RR': ('dx12', "    // Another of the game's DLSS pictures (DlssNr_PictureGate.h): left as it is before any other way out",
                             "    if (cfg.DlssNrApplyAfterRR.value_or_default()) {}\n    // Another of the game's DLSS pictures (DlssNr_PictureGate.h): left as it is before any other way out"),
    'after: asks late': ('dx12', "    // Another of the game's DLSS pictures (DlssNr_PictureGate.h): left as it is before any other way out",
                         "    CoverageScope early(0);\n    // Another of the game's DLSS pictures (DlssNr_PictureGate.h): left as it is before any other way out"),
    'pre: asks after the hook method': ('dx12', "    // Another of the game's DLSS pictures (DlssNr_PictureGate.h): left as it is, and not declined",
                                        "    (void) cfg.DlssNrHookMethod;\n    // Another of the game's DLSS pictures (DlssNr_PictureGate.h): left as it is, and not declined"),
    'vk: asks under the lock': ('vk', "    // Another of the game's DLSS pictures (DlssNr_PictureGate.h): left as it is before the recordings",
                                "    std::lock_guard<std::mutex> early(g_vkMutex);\n    // Another of the game's DLSS pictures (DlssNr_PictureGate.h): left as it is before the recordings"),
    'after: aside runs': ('dx12', '        funnel.Declined(Funnel::Decline::OtherPicture);\n        return;\n',
                          '        funnel.Declined(Funnel::Decline::OtherPicture);\n'),
    'pre: aside declines': ('dx12', 'if (PictureRole(params) == Pictures::Role::Aside)\n        return;\n\n    // Present owns',
                            'if (PictureRole(params) == Pictures::Role::Aside)\n    {\n        _declined = true;\n        return;\n    }\n\n    // Present owns'),
    'colour read eagerly': ('dx12', '    return Pictures::Ask(\n', '    (void) GetResource(params, NVSDK_NGX_Parameter_Color, "DLSSD.Color");\n    return Pictures::Ask(\n'),
    'vk: aside resets the world': ('vk', '.role == DlssNr::Pictures::Role::Aside)\n        return;',
                                   '.role == DlssNr::Pictures::Role::Aside)\n    {\n        g_vkResetOnReturn = true;\n        return;\n    }'),
    'vk: gate removed': ('vk', 'DlssNr::Pictures::Ask(params, pictureColour)', 'DlssNr::Pictures::AskNot(params, pictureColour)'),
    'dispatch: no restart': ('dx12', '    case DlssNr::Pictures::Consumed::Restart:\n        g_nr.reset = true;\n',
                             '    case DlssNr::Pictures::Consumed::Restart:\n'),
    'dispatch: meter kept': ('dx12', '        g_nr.reset = true;\n        InvalidateExposureMeter();\n        break;',
                             '        g_nr.reset = true;\n        break;'),
    'dispatch: stale runs': ('dx12', '        reportSkip("a frame of a DLSS picture the game has since replaced as the one NR follows");\n        return false;\n',
                             '        reportSkip("a frame of a DLSS picture the game has since replaced as the one NR follows");\n        break;\n'),
    'dispatch: before the lock': ('dx12', '    std::lock_guard<std::mutex> nrLock(g_nrMutex);\n    const Config& cfg = *Config::Instance();\n    const auto reportSkip',
                                  '    const Config& cfg = *Config::Instance();\n    (void) g_pictureEpoch.Consume(frame.PictureEpoch);\n    std::lock_guard<std::mutex> nrLock(g_nrMutex);\n    const auto reportSkip'),
    'vk: meter kept': ('vk', '        g_vk.reset = true;\n        g_vk.gameExposure = 0.0f;\n        g_vk.meterFrames = 0;\n        g_vk.meterPending.Invalidate();\n        g_vk.exposureLatch.Reset();\n        break;',
                       '        g_vk.reset = true;\n        break;'),
    'vk: stale runs': ('vk', '    case DlssNr::Pictures::Consumed::Stale:\n        return;\n', '    case DlssNr::Pictures::Consumed::Stale:\n        break;\n'),
    'frame without picture': ('inputs', '    frame.PictureEpoch = DlssNr::Pictures::EvaluateEpoch(params);\n', ''),
    'dx11: guides carried': ('dx11', '.role == Pictures::Role::Aside)\n        return;\n', '.role == Pictures::Role::Aside)\n        (void) 0;\n'),
    'dx11: asks after the carry starts': ('dx11', "    // Another of the game's DLSS pictures (DlssNr_PictureGate.h): its guides",
                                          "    in.depth = Describe(D3D11Resource(params, NVSDK_NGX_Parameter_Depth));\n    // Another of the game's DLSS pictures (DlssNr_PictureGate.h): its guides"),
    'release not told': ('geometry', '    Pictures::Instance().Release(key);\n', ''),
    'binding without key': ('geometry', '&record->stale, key };', '&record->stale };'),
    'decline not appended': ('funnel', "    GameOwnNr,            // the game runs its own NR through Streamline (DlssNr_SlNativeNr.h)\n    OtherPicture,         // another of the game's DLSS pictures, left as it is (DlssNr_Pictures.h)\n",
                             "    OtherPicture,         // another of the game's DLSS pictures, left as it is (DlssNr_Pictures.h)\n    GameOwnNr,            // the game runs its own NR through Streamline (DlssNr_SlNativeNr.h)\n"),
}
for name, (which, old, new) in SOURCE_MUTANTS.items():
    texts = dict(TEXTS)
    assert texts[which].count(old) == 1, f'source mutant {name!r} no longer applies'
    texts[which] = texts[which].replace(old, new)
    try:
        check(texts)
    except (AssertionError, ValueError):
        continue
    raise SystemExit(f'FAIL: the source guards accepted the mutant: {name}')
print(f'PASS: {len(SOURCE_MUTANTS)} source mutants rejected')

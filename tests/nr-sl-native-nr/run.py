#!/usr/bin/env python3
"""The game's own Streamline Neural Rendering: the detector on synthetic Streamline calls, and where it is fed and asked.

The detector header is compiled under ASan/UBSan and broken one rule at a time. The Streamline hooks, the D3D12 routes,
the Vulkan routes and the panel are read off the source, and each guard is broken once. No Streamline, no GPU, no game.
"""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / '_support'))
from nr_sources import renderer_source  # noqa: E402

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]

DETECTOR = ROOT / 'retoque/dlssnr/DlssNr_SlNativeNr.h'
FLAGS = ['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
         '-fno-omit-frame-pointer', '-pthread']

# Each rule of the detector, broken one at a time: the cases must catch every one. The first is the behaviour before
# the detector existed (NR runs on top of the game's).
DETECTOR_MUTANTS = {
    'never stands down': ('    a.standDown = recent;\n', '    a.standDown = false;\n'),
    'any feature': ('        if (feature != kFeatureDlssNr)\n            return;\n        if (!accepted)',
                    '        (void) feature;\n        if (!accepted)'),
    'refused evaluate runs': ('        if (!accepted)\n        {', '        if (!accepted && feature == 0)\n        {'),
    'requested stands down': ('_seen.fetch_or(kSeenRequested);', '_seen.fetch_or(kSeenRequested);\n'
                              '                _evaluates.fetch_add(1);'),
    'tags stand down': ('            _seen.fetch_or(kSeenColourTagged);',
                        '            { _seen.fetch_or(kSeenColourTagged); _evaluates.fetch_add(1); }'),
    'mask as colour': ('        else if (type == kBufferTypeUpliftControlMask)\n            _seen.fetch_or(kSeenControlMask);',
                       '        else if (type == kBufferTypeUpliftControlMask)\n            _seen.fetch_or(kSeenColourTagged);'),
    'tag without a resource': ('        if (!hasResource)\n            return;\n', '        (void) hasResource;\n'),
    'never releases': ('    const bool recent = evaluates != 0 && since <= kQuietMs;',
                       '    const bool recent = evaluates != 0 && (_latched || since <= kQuietMs);'),
    'releases early': ('inline constexpr uint64_t kQuietMs = 5000;', 'inline constexpr uint64_t kQuietMs = 4999;'),
    'clock skew': ('const uint64_t since = nowMs > last ? nowMs - last : 0;', 'const uint64_t since = nowMs - last;'),
    'says twice': ('    _reported |= a.newlySeen;\n', ''),
    'not counted': ('        ++_standDowns;\n', ''),
    'wrong feature id': ('inline constexpr uint32_t kFeatureDlssNr = 1004;', 'inline constexpr uint32_t kFeatureDlssNr = 1003;'),
}

COMPILE_CAUGHT = {'releases early', 'wrong feature id'}

with tempfile.TemporaryDirectory(prefix='retoque-nr-sl-native-nr-') as folder:
    folder = Path(folder)
    binary = folder / 'cases'
    subprocess.run(FLAGS + ['-I' + str(ROOT), str(HERE / 'cases.cpp'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

    detector = DETECTOR.read_text()
    for index, (name, (old, new)) in enumerate(DETECTOR_MUTANTS.items()):
        assert detector.count(old) == 1, f'detector mutant {name!r} no longer applies'
        root = folder / f'mutant{index}'
        target = root / 'retoque/dlssnr/DlssNr_SlNativeNr.h'
        target.parent.mkdir(parents=True)
        target.write_text(detector.replace(old, new))
        mutant = root / 'cases'
        built = subprocess.run(FLAGS + ['-I' + str(root), str(HERE / 'cases.cpp'), '-o', str(mutant)],
                               capture_output=True)
        if built.returncode != 0:
            # Only a changed constant may be caught by the compiler (the cases' static_asserts); anything else must
            # build and fail at run time.
            assert name in COMPILE_CAUGHT, f'detector mutant {name!r} does not compile:\n{built.stderr.decode()}'
            continue
        result = subprocess.run([str(mutant)], capture_output=True)
        assert result.returncode != 0, f'detector mutant {name!r} escaped the cases'
    print(f'PASS: {len(DETECTOR_MUTANTS)} detector mutants fail the cases')


def read(relative):
    return (ROOT / relative).read_text(encoding='utf-8-sig')


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


def branch(text, call):
    """The block an `if (const char* why = <call>)` guards."""
    start = text.index(call)
    return body(text[start:], call)


GAME_NR = 'if (const char* why = GameNrStandDown())'
VENDORED = read('external/streamline/sl_core_types.h')

sources = {
    'hooks': read('OptiScaler/hooks/Streamline_Hooks.cpp'),
    'renderer': renderer_source(),
    'vk': read('retoque/dlssnr/DlssNrFeature_Vk.cpp'),
    'host': read('retoque/dlssnr/DlssNr_VkPresentHost.cpp'),
    'skips': read('retoque/dlssnr/DlssNr_VkPresentNr.h'),
    'menu': read('retoque/dlssnr/DlssNr_Menu.cpp'),
    'header': DETECTOR.read_text(),
    'pack': read('retoque/translations/optional/pt-BR.lang'),
    'note': read('docs/dlssnr/design/streamline-native-nr.md'),
    'index': read('docs/dlssnr/README.md'),
    'rules': read('docs/dlssnr/design/DEVELOPMENT.md'),
}


def check(s):
    header = s['header']
    # The constants are Streamline 2.14.1's, cited; the vendored 2.11.1 headers are not edited to carry them.
    assert 'tag v2.14.1, commit 2122257e0f' in header and 'include/sl_core_types.h' in header, \
        'the constants carry no source'
    assert 'kFeatureDLSS_NR' not in VENDORED and 'kBufferTypeUpliftInputColor' not in VENDORED
    assert 'LOG_' not in header and '#include <Config.h>' not in header, 'the detector is pure'

    # --- The Streamline hooks: fed with atomics only, never deciding, logging or reading the config.
    hooks = s['hooks']
    assert '#include <dlssnr/DlssNr_SlNativeNr.h>' in hooks
    init = body(hooks, 'sl::Result StreamlineHooks::hkslInit(const sl::Preferences& pref, uint64_t sdkVersion)')
    noted = init.index('DlssNr::SlNativeNr::OnInit(pref.featuresToLoad, pref.numFeaturesToLoad);')
    assert noted < init.index('sl::Preferences localPref = pref;'), 'the game request, before OptiScaler edits it'
    evaluate = body(hooks, 'sl::Result StreamlineHooks::hkslEvaluateFeature(')
    call = evaluate.index('auto result = o_slEvaluateFeature(feature, frame, inputs, numInputs, cmdBuffer);')
    after = evaluate.index('DlssNr::SlNativeNr::OnEvaluate(feature, result == sl::Result::eOk);')
    assert call < after < evaluate.index('return result;'), 'accepted means Streamline answered eOk'
    local = evaluate.index('if (feature == DlssNr::SlNativeNr::kFeatureDlssNr && inputs != nullptr)')
    assert local < call and 'DlssNr::SlNativeNr::OnTags(' in evaluate[local:call]
    for name, signature in (('slSetTag', 'sl::Result StreamlineHooks::hkslSetTag('),
                            ('slSetTagForFrame', 'sl::Result StreamlineHooks::hkslSetTagForFrame(')):
        hook = body(hooks, signature)
        tags = hook.index('DlssNr::SlNativeNr::OnTags(')
        assert tags < hook.index('return o_'), f'{name} returns before noting its tags'
    for name, part in (('init', init), ('evaluate', evaluate)):
        assert 'AskInto(' not in part and 'StandDown' not in part, f'the {name} hook decides'
    # The two hooks the signal rests on are installed whatever the frame-generation input (slSetTag is not).
    attach = body(hooks, 'void StreamlineHooks::hookInterposer(HMODULE slInterposer)')
    fg_gate = attach.index('if (State::Instance().activeFgInput == FGInput::NvngxFG ||')
    assert attach.index('DetourAttach(&(PVOID&) o_slInit, hkslInit);') < fg_gate
    assert attach.index('DetourAttach(&(PVOID&) o_slEvaluateFeature, hkslEvaluateFeature);') < fg_gate

    # --- D3D12: one helper, asked by every route before any recording, and by Dispatch as the backstop.
    renderer = s['renderer']
    assert '#include <dlssnr/DlssNr_SlNativeNr.h>' in renderer
    helper = body(renderer, 'static const char* GameNrStandDown()')
    assert 'AskInto(DlssNr::SlNativeNr::Instance(), lines, DlssNr::SlNativeNr::NowMs())' in helper
    assert 'LOG_INFO(' in helper, 'the stand-down is not said'
    assert renderer.index('static const char* GameNrStandDown()') < renderer.index('bool DlssNr_Dx12::Dispatch(')

    dispatch = body(renderer, 'bool DlssNr_Dx12::Dispatch(')
    asked = dispatch.index(GAME_NR)
    assert dispatch.index('if (PausedWithoutFrameGen(cfg))') < asked
    assert asked < dispatch.index('g_activePasses = 0;') and asked < dispatch.index('TrackNrRecording(cmdList')
    stand = branch(dispatch, GAME_NR)
    assert 'return false;' in stand and 'g_resetOnReturn = true;' in stand and 'reportSkip(why);' in stand

    after = body(renderer, 'void EvaluateAfterUpscale(')
    assert after.count(GAME_NR) == 1, 'the after-upscale route does not ask'
    asked = after.index(GAME_NR)
    assert after.index('UpscaledDefersToPresent(') < asked and after.index('++g_upscaleRenders;') < asked
    assert asked < after.index('EnsureCompose(') < after.index('g_compose->Dispatch(')
    stand = branch(after, GAME_NR)
    assert 'return;' in stand and 'g_resetOnReturn = true;' in stand and 'g_upscaleReason = why;' in stand

    scope = body(renderer, 'ScopedPreUpscale::ScopedPreUpscale(')
    assert scope.count(GAME_NR) == 1, 'the pre-upscale scope does not ask'
    asked = scope.index(GAME_NR)
    assert asked < scope.index('_lock = std::unique_lock<std::mutex>(g_preMutex, std::try_to_lock);')
    assert asked < scope.index('_color.Read(')
    stand = branch(scope, GAME_NR)
    assert 'return;' in stand and 'g_resetOnReturn = true;' in stand and 'g_preStatus = why;' in stand
    assert '_declined' not in stand and 'decline(' not in stand, 'a stand-down must not hand the frame on'

    present = body(renderer, 'void RunPresentPass(IDXGISwapChain3* swapchain, ID3D12CommandQueue* queue, bool fgHook, bool boundary)')
    assert present.count(GAME_NR) == 1, 'the present route does not ask'
    asked = present.index(GAME_NR)
    assert present.index('g_presentRouteAtMs = NowMs();') < asked, 'the route stays live while it stands down'
    assert asked < present.index('EvaluateAtPresent(') and asked < present.index('GetDesc1(&scDesc)')
    stand = branch(present, GAME_NR)
    assert 'return;' in stand and 'g_resetOnReturn = true;' in stand and 'ReportRuntimeStatus(false, why);' in stand

    # --- Vulkan after the upscaler: before the model is asked for, the history owed on the way back.
    vk = body(s['vk'], 'void EvaluateAfterUpscaleVk(')
    asked = vk.index('DlssNr::SlNativeNr::AskInto(DlssNr::SlNativeNr::Instance(), lines, DlssNr::SlNativeNr::NowMs())')
    assert vk.index('if (g_vk.failed)') < asked < vk.index('if (g_vkResetOnReturn.exchange(false))')
    assert asked < vk.index('ClaimModelForAfterUpscale(device)') and asked < vk.index('params->Get(')
    stand = vk[asked:vk.index('if (g_vkResetOnReturn.exchange(false))')]
    assert re.search(r'if \(why != nullptr\)\s*\{\s*g_vkResetOnReturn = true;\s*return;\s*\}', stand), \
        'the Vulkan route does not return on the stand-down'
    assert 'LOG_INFO(' in stand

    # --- The Vulkan present host: refused with its own counted skip, lines kept for after the lock, nothing freed.
    decide = body(s['host'], 'NrDecision DecideNr(')
    asked = decide.index('DlssNr::SlNativeNr::AskInto(DlssNr::SlNativeNr::Instance(), dev.lines,')
    assert decide.index('DlssNrEnabled') < asked < decide.index('ClaimModelForPresentHost') < decide.index('->Admit(')
    assert asked < decide.index('PresentHostMustYield')
    stand = decide[asked:decide.index('PresentHostMustYield')]
    assert 'return refuse(NrSkip::GameNr, why);' in stand and 'StandDown(dev)' not in stand and 'LOG_' not in stand
    skips = s['skips']
    enum = skips[skips.index('enum class NrSkip'):skips.index('Count,', skips.index('enum class NrSkip'))]
    assert 'GameNr,' in enum and enum.index('FrameGeneration,') < enum.index('GameNr,'), 'appended, before Count'
    assert re.search(r'case NrSkip::GameNr:\s*return "game\'s own NR";', skips)

    # --- The panel: one statement while NR is on and the stand-down holds; never a control.
    menu = s['menu']
    shown = menu.index('if (enabled && DlssNr::SlNativeNr::Instance().Active())')
    line = menu[shown:menu.index('PopStyleColor', shown)]
    assert 'Localization::Tr(DlssNr::SlNativeNr::kMenuLine)' in line
    assert 'Button' not in line and 'Checkbox' not in line
    menu_line = re.search(r'kMenuLine =\s*((?:"(?:\\.|[^"\\])*"\s*)+);', header)
    assert menu_line, 'kMenuLine is not a literal'
    text = ''.join(re.findall(r'"((?:\\.|[^"\\])*)"', menu_line.group(1)))
    assert 'the game runs its own nr' in text.lower()
    assert any(l.split('\t')[0] == text for l in s['pack'].splitlines() if l and not l.startswith('#')), \
        'the pt-BR pack lacks the panel line'

    # --- The record: a dated design note, indexed, and the panel's row in the rules.
    note = s['note']
    assert '2026-10-30' in note and 'v2.14.1' in note and 'NBA 2K27' in note
    assert 'design/streamline-native-nr.md' in s['index']
    assert 'kMenuLine' in s['rules'] and 'streamline-native-nr.md' in s['rules']
    return True


check(sources)


def mutate(key, old, new, count=1):
    changed = dict(sources)
    assert changed[key].count(old) >= 1, f'mutant anchor missing in {key}: {old!r}'
    changed[key] = changed[key].replace(old, new, count)
    return changed


def block_at(text, start):
    """From `start` to the brace closing the first block opened after it."""
    brace = text.index('{', start)
    depth = 0
    for i in range(brace, len(text)):
        depth += (text[i] == '{') - (text[i] == '}')
        if depth == 0:
            return text[start:i + 1]
    raise AssertionError('unterminated block')


def remove_branch(key, anchor):
    """The stand-down's whole `if` taken out of the function whose anchor is given."""
    text = sources[key]
    asked = text.index(GAME_NR, text.index(anchor))
    changed = dict(sources)
    changed[key] = text[:asked] + text[asked + len(block_at(text, asked)):]
    return changed


mutants = {
    'vendored edited': None,
    'init after the edit': mutate('hooks', '    DlssNr::SlNativeNr::OnInit(pref.featuresToLoad, pref.numFeaturesToLoad);\n', ''),
    'evaluate noted before the answer': mutate(
        'hooks', 'DlssNr::SlNativeNr::OnEvaluate(feature, result == sl::Result::eOk);',
        'DlssNr::SlNativeNr::OnEvaluate(feature, true);'),
    'tags not noted': mutate('hooks', 'DlssNr::SlNativeNr::OnTags(tags, numTags);', ''),
    'hook decides': mutate('hooks', 'DlssNr::SlNativeNr::OnEvaluate(feature, result == sl::Result::eOk);',
                           'DlssNr::SlNativeNr::OnEvaluate(feature, result == sl::Result::eOk);\n    '
                           'DlssNr::SlNativeNr::AskInto(DlssNr::SlNativeNr::Instance(), lines, 0);'),
    'dispatch backstop removed': remove_branch('renderer', 'bool DlssNr_Dx12::Dispatch('),
    'after-upscale removed': remove_branch('renderer', 'void EvaluateAfterUpscale('),
    'pre-upscale removed': remove_branch('renderer', 'ScopedPreUpscale::ScopedPreUpscale('),
    'present removed': remove_branch('renderer', 'void RunPresentPass('),
    'pre-upscale declines': mutate('renderer', '        g_preStatus = why;\n        g_resetOnReturn = true;\n        '
                                   'ReportSkipOnce(why);\n        return;\n    }\n\n    _lock',
                                   '        g_preStatus = why;\n        g_resetOnReturn = true;\n        _declined = true;\n'
                                   '        ReportSkipOnce(why);\n        return;\n    }\n\n    _lock'),
    'not said': mutate('renderer', '        LOG_INFO("{}", line);\n    return why;', '    return why;'),
    'vk claims first': mutate('vk', '        if (why != nullptr)\n        {\n            g_vkResetOnReturn = true;',
                              '        if (false)\n        {\n            g_vkResetOnReturn = true;'),
    'host frees the model': mutate('host', 'return refuse(NrSkip::GameNr, why);',
                                   '{\n            StandDown(dev);\n            return refuse(NrSkip::GameNr, why);\n        }'),
    'menu control': mutate('menu', 'ImGui::TextWrapped("%s", Localization::Tr(DlssNr::SlNativeNr::kMenuLine));',
                           'ImGui::Checkbox(Localization::Tr(DlssNr::SlNativeNr::kMenuLine), &shown);'),
    'menu while off': mutate('menu', 'if (enabled && DlssNr::SlNativeNr::Instance().Active())',
                             'if (DlssNr::SlNativeNr::Instance().Active())'),
    'uncited constants': mutate('header', 'tag v2.14.1, commit 2122257e0f', 'tag v2.14'),
}
VENDORED_ORIGINAL = VENDORED
for name, mutant in mutants.items():
    if mutant is None:
        VENDORED = VENDORED_ORIGINAL + '\nconstexpr Feature kFeatureDLSS_NR = 1004;\n'
        mutant = sources
    assert mutant is sources or mutant != sources, f'mutant {name!r} did not apply'
    try:
        check(mutant)
    except (AssertionError, ValueError):
        continue
    finally:
        VENDORED = VENDORED_ORIGINAL
    raise AssertionError(f'mutant {name!r} escaped the source guard')

print(f'PASS: hooks fed with atomics; every D3D12 and Vulkan route asks before the model; panel line; '
      f'{len(mutants)} source mutants rejected')

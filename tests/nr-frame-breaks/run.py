#!/usr/bin/env python3
"""Frame breaks (docs/dlssnr/design/frame-breaks.md): the production skip tracker and reset counter under ASan/UBSan,
mutants of the key lines (each must fail the cases), and source guards on the key and where the renderer calls them.
No GPU: this checks the counting and the decision, not the picture."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / '_support'))
from nr_sources import renderer_source  # noqa: E402

here = Path(__file__).resolve().parent
root = here.parents[1]
retoque = root / 'retoque'
header_path = retoque / 'dlssnr/DlssNr_FrameBreaks.h'
FLAGS = ['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
         '-fno-omit-frame-pointer']
MUTANT_FLAGS = [f for f in FLAGS if f not in ('-Werror', '-Wall', '-Wextra')] + ['-w']


def build_and_run(include_dir, flags):
    binary = Path(include_dir) / 'cases'
    subprocess.run(flags + ['-I', str(include_dir), '-I', str(retoque), str(here / 'cases.cpp'), '-o', str(binary)],
                   check=True)
    return subprocess.run([str(binary)], capture_output=True, text=True)


with tempfile.TemporaryDirectory(prefix='optiscaler-nr-frame-breaks-') as directory:
    clean = build_and_run(directory, FLAGS)
    sys.stdout.write(clean.stdout)
    assert clean.returncode == 0, clean.stderr

    source = header_path.read_text()
    mutants = {
        'a trailing gap is a break': ('        else if (seenRan)\n        {\n            inGap = true;\n        }\n    }\n    return breaks;',
                                      '        else if (seenRan)\n        {\n            inGap = true;\n        }\n    }\n    return breaks + (inGap ? 1u : 0u);'),
        'gaps counted per frame': ('            seenRan = true;\n            inGap = false;', '            seenRan = true;'),
        'one break holds': ('constexpr unsigned kMinBreaks = 3;', 'constexpr unsigned kMinBreaks = 1;'),
        'hold forever': ('if (nowMs < _holdUntilMs)', 'if (_holdUntilMs != 0)'),
        'hold never ends by time': ('constexpr int64_t kHoldMs = 1000;', 'constexpr int64_t kHoldMs = 0;'),
        'held when not enabled': ('if (!enabled || Breaks(_bits, _count) < kMinBreaks)\n            return false;',
                                  'if (Breaks(_bits, _count) < kMinBreaks)\n            return false;'),
        'off keeps the hold': ('        if (!enabled)\n        {\n            _holdUntilMs = 0;\n            return false;\n        }',
                               '        if (!enabled)\n            return false;'),
        'window not cleared after a hold': ('        ++_standDowns;\n        Clear();', '        ++_standDowns;'),
        'ran not counted': ('ran ? ++_ran : ++_skipped;', 'ran ? 0 : ++_skipped;'),
        'reset window ignored': ('if (nowMs - e.ms > windowMs || e.ms > nowMs)', 'if (e.ms > nowMs)'),
        'reset causes merged': ('++s.byCause[static_cast<size_t>(e.cause)];', '++s.byCause[0];'),
        'reset report every call': ('if (_reported && nowMs - _reportedAtMs < kReportMs)\n            return false;', ''),
        'reset report when empty': ('        if (Summarize(nowMs, kReportMs).total == 0)\n            return false;\n', ''),
        'peak counted over the window': ('if (times[b] >= times[a] && times[b] - times[a] < 1000)', 'if (times[b] >= times[a])'),
    }
    for name, (old, new) in mutants.items():
        assert source.count(old) == 1, f'mutant anchor missing or repeated: {name}'
        mutant_dir = Path(directory) / re.sub(r'\W+', '_', name)
        (mutant_dir / 'dlssnr').mkdir(parents=True)
        (mutant_dir / 'dlssnr/DlssNr_FrameBreaks.h').write_text(source.replace(old, new))
        result = build_and_run(mutant_dir, MUTANT_FLAGS)
        assert result.returncode != 0, f'mutant survived: {name}'
    print(f'{len(mutants)} mutants killed')

# --- source guards --------------------------------------------------------------------------------------------------
config_h = (root / 'OptiScaler/Config.h').read_text()
config_cpp = (root / 'OptiScaler/Config.cpp').read_text()
ini = (root / 'OptiScaler.ini').read_text()
config_md = (root / 'Config.md').read_text()
menu = (retoque / 'dlssnr/DlssNr_Menu.cpp').read_text()
renderer = renderer_source()
diagnostics = (retoque / 'shaders/dlssnr/DlssNr_Dx12_Diagnostics.cpp').read_text()
internal = (retoque / 'shaders/dlssnr/DlssNr_Dx12_Internal.h').read_text()
vulkan = (retoque / 'dlssnr/DlssNrFeature_Vk.cpp').read_text()
vcxproj = (root / 'OptiScaler/OptiScaler.vcxproj').read_text()

assert re.search(r'CustomOptional<bool>\s+DlssNrSkipStandDown\s*\{\s*false\s*\}', config_h), 'off by default'
assert 'DlssNrSkipStandDown.set_from_config(readBool("DlssNr", "SkipStandDown"))' in config_cpp
assert re.search(r'ini\.SetValue\("DlssNr", "SkipStandDown",\s*GetBoolValue\(Instance\(\)->DlssNrSkipStandDown\.'
                 r'value_for_config\(\)\)\.c_str\(\)\)', config_cpp)
assert re.search(r'^SkipStandDown=auto$', ini, re.M) and re.search(r'^SkipStandDown=auto$', config_md, re.M)
assert 'SkipStandDown' not in vulkan, 'the Vulkan route never reads the key'
assert '<ClInclude Include="..\\retoque\\dlssnr\\DlssNr_FrameBreaks.h" />' in vcxproj

# The state: one tracker and one counter for the stream, in the renderer's state.
assert internal.count('DlssNr::FrameBreaks::SkipTracker skips;') == 1
assert internal.count('DlssNr::FrameBreaks::ResetRate resetRate;') == 1

# A skip is noted by reportSkip, a run by the end of Dispatch, and never a stand-down (it was not asked).
skip = renderer[renderer.index('const auto reportSkip = ['):]
skip = skip[:skip.index('};')]
assert 'NoteFrameOutcome(false, cfg.DlssNrSkipStandDown.value_or_default(), why);' in skip
dispatch = renderer[renderer.index('bool DlssNr_Dx12::Dispatch('):]
dispatch = dispatch[:dispatch.index('\nnamespace DlssNr\n')]
assert dispatch.count('NoteFrameOutcome(') == 2
assert diagnostics.count('void NoteFrameOutcome(') == 1 and diagnostics.count('void NoteResetSent(') == 1
assert 'bool FrameBreakHeld(bool enabled) { return g_nr.skips.StandDown(enabled, BreaksNowMs()); }' in diagnostics
assert 'NoteFrameOutcome(wrote, cfg.DlssNrSkipStandDown.value_or_default(),' in dispatch
hold = dispatch[dispatch.index('FrameBreakHeld('):]
hold = hold[:hold.index('return false;')]
assert 'NoteFrameOutcome' not in hold and 'reportSkip(' not in hold, 'a hold is not a skip'
assert 'g_nr.reset = true;' in hold, 'the history is owed a reset for when it resumes'
# After the pulse sequence and the capture's ticket (those must come before the first return), before anything else.
assert dispatch.index('NotePulsePresent(') < dispatch.index('PairCapture::Poll();') < dispatch.index('FrameBreakHeld(')
assert dispatch.index('FrameBreakHeld(') < dispatch.index('this != g_compose.get()')
# The calls of one present are one outcome: committed when the next present arrives, merged by `ran`.
helpers = diagnostics[diagnostics.index('static void CommitFrameOutcome('):diagnostics.index('void NoteResetSent(')]
assert 'g_nr.skipPresentRan = g_nr.skipPresentRan || ran;' in helpers
assert 'present != 0 && g_nr.skipPresentValid && present == g_nr.skipPresent' in helpers
# The reset is counted where it is sent to the model, once a model frame, with the frame's own cause.
assert dispatch.count('NoteResetSent(frameReset, frameResetIsPolicy, frameResetIsSceneCut);') == 1
sent = dispatch[dispatch.index('if (g_nr.reset)\n            NoteResetSent('):]
assert sent.index('NoteResetSent(') < sent.index('g_nr.evaluate(')
assert 'if (!carried)\n        g_nr.reset = false;' in dispatch
# The menu: D3D12 only, never under the proxy, with its own ID.
fn = menu[menu.index('static void RenderSkipStandDown('):]
fn = fn[:fn.index('\n}\n')]
assert 'if (nativeVulkan || config->DlssNrUseProxy.value_or_default())\n        return;' in fn
assert 'Hold NR off when it alternates###nrSkipStandDown' in fn
assert menu.count('RenderSkipStandDown(config, nativeVulkan);') == 1
print('frame breaks: source guards ok')

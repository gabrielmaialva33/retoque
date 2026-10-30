#!/usr/bin/env python3
"""The multi-view stand-down: the detector on synthetic sub-rectangles, and where the D3D12 seams ask it.

The detector header is compiled under ASan/UBSan. The seams are read off the source; nr-before-upscale runs the same
production seams against host fakes. No D3D12, no NGX, no game.
"""
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / '_support'))
from nr_sources import renderer_source  # noqa: E402

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]

DETECTOR = ROOT / 'retoque/dlssnr/DlssNr_MultiView.h'
FLAGS = ['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
         '-fno-omit-frame-pointer']

# Each rule of the detector, broken one at a time: the cases must catch every one. The first is the behaviour before
# the detector existed (every evaluate runs).
DETECTOR_MUTANTS = {
    'never stands down': ('            result.verdict = Verdict::StandDown;\n', ''),
    'latches on one frame': ('constexpr uint32_t kLatchEpochs = 2;', 'constexpr uint32_t kLatchEpochs = 1;'),
    'judges before a present': ('        if (epoch == 0)\n', '        if (false)\n'),
    'two seams count twice': ('known = known || _epochViews[i].SameView(seen);', 'known = false;'),
    'any allocation': ('return allocWidth == o.allocWidth && allocHeight == o.allocHeight && allocFormat == o.allocFormat;',
                       '(void) o;\n        return true;'),
    'forgets too soon': ('if (++_quiet < kQuietEpochs)', 'if (++_quiet < kQuietEpochs / 2)'),
    'never forgets': ('        _latched = false;\n        _quiet = 0;\n        return true;',
                      '        return false;'),
}

with tempfile.TemporaryDirectory(prefix='retoque-nr-multi-view-') as folder:
    folder = Path(folder)
    binary = folder / 'cases'
    subprocess.run(FLAGS + ['-I' + str(ROOT), str(HERE / 'cases.cpp'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

    detector = DETECTOR.read_text()
    for index, (name, (old, new)) in enumerate(DETECTOR_MUTANTS.items()):
        assert detector.count(old) == 1, f'detector mutant {name!r} no longer applies'
        root = folder / f'mutant{index}'
        target = root / 'retoque/dlssnr/DlssNr_MultiView.h'
        target.parent.mkdir(parents=True)
        target.write_text(detector.replace(old, new))
        mutant = root / 'cases'
        subprocess.run(FLAGS + ['-I' + str(root), str(HERE / 'cases.cpp'), '-o', str(mutant)], check=True)
        result = subprocess.run([str(mutant)], capture_output=True)
        assert result.returncode != 0, f'detector mutant {name!r} escaped the cases'
    print(f'PASS: {len(DETECTOR_MUTANTS)} detector mutants fail the cases')

renderer = renderer_source()
header = (ROOT / 'retoque/dlssnr/DlssNr_MultiView.h').read_text()
config_h = (ROOT / 'OptiScaler/Config.h').read_text(encoding='utf-8-sig')
config_cpp = (ROOT / 'OptiScaler/Config.cpp').read_text(encoding='utf-8-sig')
ini = (ROOT / 'OptiScaler.ini').read_text(encoding='utf-8-sig', errors='replace')

JUDGE = 'static const char* MultiViewStandDown(NVSDK_NGX_Parameter* params, ID3D12Resource* colour)\n{'
CALL = 'MultiViewStandDown(params, GetResource(params, NVSDK_NGX_Parameter_Color, "DLSSD.Color"))'


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


def check(text):
    assert '#include <dlssnr/DlssNr_MultiView.h>' in text, 'the renderer does not include the detector'
    judge = body(text, JUDGE)
    assert 'static DlssNr::MultiView::Detector s_multiViews;' in text
    assert text.index('static DlssNr::MultiView::Detector s_multiViews;') < text.index(JUDGE)
    assert 's_multiViews.Note(State::Instance().frameCount, seen)' in judge, 'the frame is the observed present'
    for said in ('result.latchedNow', 'result.unlatchedNow', 'LOG_WARN(', 'result.standDowns'):
        assert said in judge, f'the stand-down is not counted and said: {said}'
    assert 'Verdict::StandDown' in judge and 'return nullptr;' in judge

    # Before the upscaler: after the scope owns the frame and before any scratch loan, read or barrier; a stand-down,
    # not a decline, so the after-upscale pass does not serve the frame over every screen.
    scope = body(text, 'ScopedPreUpscale::ScopedPreUpscale(')
    assert scope.count(CALL) == 1, 'the pre-upscale scope does not ask the detector'
    asked = scope.index(CALL)
    assert scope.index('coverage.sample.reason = "no command list or parameter block";') < asked
    assert asked < scope.index('_lock = std::unique_lock<std::mutex>(g_preMutex, std::try_to_lock);')
    assert asked < scope.index('_color.Read(')
    stand = branch(scope, CALL)
    assert 'return;' in stand and 'g_resetOnReturn = true;' in stand and 'ReportSkipOnce(' in stand
    assert '_declined' not in stand and 'decline(' not in stand, 'a stand-down must not hand the frame on'

    # After the upscaler: the frame is this route's (counted as a render with its reason) and nothing is recorded.
    after = body(text, 'void EvaluateAfterUpscale(')
    assert after.count(CALL) == 1, 'the after-upscale pass does not ask the detector'
    asked = after.index(CALL)
    assert after.index('UpscaledDefersToPresent(') < asked
    assert after.index('++g_upscaleRenders;') < asked
    assert asked < after.index('EnsureCompose(') < after.index('g_compose->Dispatch(')
    stand = branch(after, CALL)
    assert 'return;' in stand and 'g_resetOnReturn = true;' in stand and 'g_upscaleReason' in stand

    # Only the detector and the default stand-down: no view pick, no per-view model, no setting.
    for where, name in ((text, 'renderer'), (header, 'detector'), (config_h, 'Config.h'), (config_cpp, 'Config.cpp'),
                        (ini, 'OptiScaler.ini')):
        assert 'MultiViewTarget' not in where, f'a view pick reached the {name}'
    assert 'Pick' not in header and 'Select(' not in header
    return True


check(renderer)

# Each guard must fail on the change it is there for.
mutants = {
    'no include': renderer.replace('#include <dlssnr/DlssNr_MultiView.h>', '', 1),
    'pre seam removed': renderer.replace('if (const char* why = ' + CALL + ')\n    {\n        coverage.sample.reason = why;\n        g_preStatus',
                                         'if (const char* why = nullptr)\n    {\n        coverage.sample.reason = why;\n        g_preStatus', 1),
    'after seam removed': renderer.replace('if (const char* why = ' + CALL + ')\n    {\n        coverage.sample.reason = why;\n        g_upscaleReason',
                                           'if (const char* why = nullptr)\n    {\n        coverage.sample.reason = why;\n        g_upscaleReason', 1),
    'declined instead': renderer.replace('        g_preStatus = why;\n', '        g_preStatus = why;\n        _declined = true;\n', 1),
    'before the deferral': renderer.replace('++g_upscaleRenders;\n    g_upscaleApplied = false;',
                                            '++g_upscaleRendersMoved;\n    g_upscaleApplied = false;', 1),
    'not counted': renderer.replace('result.standDowns', 'result.viewsInEpoch'),
    'a pick': renderer.replace('s_multiViews.Note(', 's_multiViews.Note(/* MultiViewTarget */ ', 1),
}
for name, mutant in mutants.items():
    assert mutant != renderer, f'mutant {name!r} did not apply'
    try:
        check(mutant)
    except (AssertionError, ValueError):
        continue
    raise AssertionError(f'mutant {name!r} escaped the source guard')

print(f'PASS: detector cases; both D3D12 seams stand down before any loan or recording; {len(mutants)} mutants rejected')

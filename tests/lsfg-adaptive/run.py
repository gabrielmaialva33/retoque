#!/usr/bin/env python3
"""The MFG presenter's log-only adaptive multiplier adviser (phase 1, docs/dlssnr/design/lsfg-adaptive-shadow.md).

Two parts:

1. test.cpp, under ASan/UBSan: retoque/lsfg/presenter/MfgAdaptive.h with the real XeFG_DynamicCount.h. MAKO's refresh
   budget vectors (63 and 100 fps at x2 on 120 Hz), the 250 ms gap and one-output carry, 165 Hz V-Sync non-divisor
   traces, the cadence estimator on an alternating 8/32 ms trace, bursts, a hitch against a sustained drop and stalls,
   the 5% delivery rule, MAKO's 40->30 / 80->90 at 120 rejection, the adviser's choices and reasons, and the shadow's
   windows (refresh sampling cadence, dropped entries as misses, evidence only from steady windows, 60 s expiry).
2. Source guards: the adviser only reads. Nothing it computes reaches the generated-frame count or the pacing:
   - setGeneratedFrames, the one writer of generatedFramesWanted, and the count dispatchInterpolationCommands reads are
     unchanged, and the router still hands the live multiplier to setGeneratedFrames;
   - the shadow is touched only on the presenter thread (after an entry's presents) and in the 10 s summary, never in
     the interpolation thread, presentInterpolated, dispatchInterpolationCommands or the router;
   - presentQpcDelta is still set only from PresentDelta in the interpolation thread;
   - MfgAdaptive.h stays host-buildable (no <windows.h>, no Config), takes the target from XeFG_DynamicTargetFps, and
     carries MAKO's attribution, as does Licenses/MAKO_ATTRIBUTION.txt.
"""

import re
import subprocess
import sys
import tempfile
from pathlib import Path

root = Path(__file__).resolve().parent
repo = root.parents[1]
failures = []


def check(cond, what):
    if not cond:
        failures.append(what)


def body(text, signature):
    """The body of the first function whose definition starts with `signature` (brace matched)."""
    start = text.find(signature)
    if start < 0:
        return None
    open_brace = text.find('{', start)
    depth = 0
    for i in range(open_brace, len(text)):
        if text[i] == '{':
            depth += 1
        elif text[i] == '}':
            depth -= 1
            if depth == 0:
                return text[open_brace + 1:i]
    return None


def code_only(text):
    return '\n'.join(line.split('//', 1)[0] for line in text.splitlines())


# 1. The adviser, compiled unmodified.
with tempfile.TemporaryDirectory(prefix='retoque-lsfg-adaptive-') as directory:
    binary = str(Path(directory) / 'test')
    build = subprocess.run(['g++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                            '-fno-sanitize-recover=undefined', '-fno-omit-frame-pointer', '-g',
                            '-I', str(repo / 'retoque'), '-I', str(repo / 'OptiScaler'), str(root / 'test.cpp'), '-o',
                            binary])
    check(build.returncode == 0, 'test.cpp does not build')
    if build.returncode == 0:
        check(subprocess.run([binary]).returncode == 0, 'test.cpp failed')

presenter = repo / 'retoque/lsfg/presenter'
swapchain = (presenter / 'MfgSwapChain_Dx12.cpp').read_text()
swapchain_h = (presenter / 'MfgSwapChain_Dx12.h').read_text()
router = (presenter / 'LsfgPresenter.cpp').read_text()
adaptive_path = presenter / 'MfgAdaptive.h'
adaptive = adaptive_path.read_text() if adaptive_path.is_file() else ''
check(adaptive != '', 'retoque/lsfg/presenter/MfgAdaptive.h is missing')

# 2a. The generated-frame count path, byte for byte.
setter = body(swapchain, 'void MfgSwapChainDX12::setGeneratedFrames(uint32_t generatedFrames)')
check(setter is not None and setter.strip() ==
      'generatedFramesWanted = std::clamp<uint32_t>(generatedFrames, 1, kMaxGeneratedFrames);',
      'setGeneratedFrames changed')
check(len(re.findall(r'\bgeneratedFramesWanted\s*=(?!=)', code_only(swapchain))) == 1,
      'generatedFramesWanted is written outside setGeneratedFrames')
dispatch = body(swapchain, 'InterpolationResult MfgSwapChainDX12::dispatchInterpolationCommands()')
check(dispatch is not None and
      'const uint32_t wanted = std::clamp<uint32_t>(generatedFramesWanted.load(), 1, kMaxGeneratedFrames);' in dispatch,
      'dispatchInterpolationCommands no longer reads the count as before')
# Its only callers: the router at creation (the configured multiplier) and at every FG configure (the live one).
callers = sorted(line.strip() for line in code_only(router).splitlines() if 'setGeneratedFrames(' in line)
check(callers == ['sc->setGeneratedFrames(generated);', 'swapChain->setGeneratedFrames(generated);'],
      f'setGeneratedFrames callers changed: {callers}')
check(re.search(r'const uint32_t generated = LsfgMfg::GeneratedFramesFor\(Multiplier\(\)\);\s*\n\s*'
                r'sc->setGeneratedFrames\(generated\);', router), 'the creation count is not the configured multiplier')
check(not any('setGeneratedFrames(' in code_only(t) for t in (swapchain_h.replace(
      'void setGeneratedFrames(uint32_t generatedFrames);', ''), swapchain.replace(
      'void MfgSwapChainDX12::setGeneratedFrames(uint32_t generatedFrames)', ''))),
      'the swapchain calls setGeneratedFrames on itself')
check(re.search(r'const uint32_t generated =\s*\n\s*LsfgMfg::GeneratedFramesLive\('
                r'Config::Instance\(\)->LsfgEnabled\.value_or_default\(\), Multiplier\(\)\);', router),
      'the router no longer takes the count from GeneratedFramesLive')

# 2b. Where the shadow is touched.
interp = body(swapchain, 'DWORD WINAPI interpolationThread(LPVOID param)')
present = body(swapchain, 'void MfgSwapChainDX12::presentInterpolated(UINT SyncInterval, UINT)')
pthread = body(swapchain, 'DWORD WINAPI presenterThread(LPVOID param)')
logwin = body(swapchain, 'void LogPresentWindow(PresentInfo* presenter, int64_t nowQpc, int64_t qpcFrequency)')
for name, text in (('interpolationThread', interp), ('presentInterpolated', present),
                   ('dispatchInterpolationCommands', dispatch), ('setGeneratedFrames', setter)):
    check(text is not None and 'adaptive' not in code_only(text).lower() and 'Adaptive::' not in text,
          f'{name} touches the adaptive shadow')
check('Adaptive' not in code_only(router), 'the router touches the adaptive shadow')
check(pthread is not None and 'presenter->adaptive.OnEntry(' in pthread, 'the presenter thread does not feed the shadow')
if pthread:
    # Fed after the entry's presents, never between them.
    check(pthread.find('presentToSwapChain(presenter, &entry, frameIndex);') < pthread.find('presenter->adaptive.OnEntry('),
          'the shadow is fed before the entry\'s presents')
    check(code_only(pthread).count('adaptive') == code_only(pthread).count('presenter->adaptive.OnEntry('),
          'the presenter thread uses the shadow beyond OnEntry')
check(logwin is not None and 'presenter->adaptive.CloseWindow(' in logwin, 'the 10 s summary does not close the shadow')
allowed = [(swapchain.find(t), swapchain.find(t) + len(t)) for t in (pthread or '', logwin or '') if t]
sample = body(swapchain, 'void SampleRefresh(PresentInfo* presenter, double nowSec, bool summary)')
check(sample is not None and 'Util::GetActiveRefreshRate(presenter->outputWindow)' in sample,
      'the refresh is not sampled through Util::GetActiveRefreshRate, as XeFG does')
if sample:
    allowed.append((swapchain.find(sample), swapchain.find(sample) + len(sample)))
code = code_only(swapchain)
for use in re.finditer(r'adaptive\.', swapchain):
    line = swapchain[swapchain.rfind('\n', 0, use.start()) + 1:use.start()]
    if '//' in line:
        continue
    check(any(a <= use.start() < b for a, b in allowed), f'the shadow is used outside the presenter thread/summary '
          f'at offset {use.start()}')
check(len(re.findall(r'presentQpcDelta\s*=(?!=)', code)) == 1 and
      'entry.frames[i].presentQpcDelta = deltaToUse;' in (interp or ''), 'presentQpcDelta has a new writer')
check(re.search(r'Adaptive::Shadow adaptive;', swapchain_h) is not None, 'PresentInfo does not own the shadow')

# 2c. The header: host-buildable, XeFG's target, attribution.
lowered = code_only(adaptive).lower()
check('<windows.h>' not in lowered and 'config::' not in lowered and 'util::' not in lowered,
      'MfgAdaptive.h depends on Windows, Config or Util')
check('XeFG_DynamicTargetFps(' in adaptive, 'MfgAdaptive.h does not take the target from XeFG_DynamicTargetFps')
for line in ('MAKO', 'eugeniosegala/MAKO', 'GPL-3.0-or-later', 'Eugenio Segala'):
    check(line in adaptive, f'MfgAdaptive.h lacks the attribution {line!r}')
attribution = repo / 'Licenses/MAKO_ATTRIBUTION.txt'
check(attribution.is_file() and 'GPL-3.0-or-later' in attribution.read_text() and
      'MfgAdaptive.h' in attribution.read_text(), 'Licenses/MAKO_ATTRIBUTION.txt missing or incomplete')

if failures:
    for f in failures:
        print('FAIL:', f)
    sys.exit(1)
print('source guards: ok')

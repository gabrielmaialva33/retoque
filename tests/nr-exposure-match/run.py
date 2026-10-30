#!/usr/bin/env python3
"""The exposure scan's match ([DlssNr] ScanMatch, docs/dlssnr/design/exposure-scan-match.md): the pure rule
(retoque/dlssnr/DlssNr_ExposureMatch.h) compiled under ASan/UBSan and driven over synthetic traces and janblade's
trimmed recordings in data/, then source guards on its wiring: stdlib only, off by default with the four-point config
round trip, the meter read off the game's frame and never the pass's output, and the default path left on widest
travel. The scan's own behaviour with the key on and off is tests/nr-exposure-scan (match-*). No GPU, no game."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

here = Path(__file__).resolve().parent
root = here.parents[1]
retoque = root / 'retoque'
failures = []


def read(relative):
    return (root / relative).read_text(encoding='utf-8-sig')


def guard(ok, what):
    print(('PASS ' if ok else 'FAIL ') + 'guard: ' + what, flush=True)
    if not ok:
        failures.append(what)


header = read('retoque/dlssnr/DlssNr_ExposureMatch.h')
includes = re.findall(r'#include\s*[<"]([^>"]+)[>"]', header)
guard(includes and all('/' not in n and '.' not in n for n in includes), 'the rule is stdlib only')
guard('janblade' in header and 'GPL-3.0' in header, 'the rule credits its source and licence')

pulse = read('retoque/dlssnr/DlssNr_Pulse.h')
reader = pulse[pulse.index('inline double MeanInputLuma('):]
reader = reader[:reader.index('\n}\n')]
guard(re.search(r'tile\[1\]', reader) and re.search(r'tile\[2\]', reader) and not re.search(r'tile\[0\]', reader),
      "the meter reads the game's frame (luminance in / pixels) and never the pass's output")

config_h = read('OptiScaler/Config.h')
config_cpp = read('OptiScaler/Config.cpp')
ini = read('OptiScaler.ini')
guard('CustomOptional<bool> DlssNrScanMatch { false };' in config_h, 'ScanMatch is declared, off by default')
guard('DlssNrScanMatch.set_from_config(readBool("DlssNr", "ScanMatch"));' in config_cpp, 'ScanMatch is read')
guard(re.search(r'SetValue\("DlssNr", "ScanMatch",', config_cpp) is not None, 'ScanMatch is saved')
section = ini[ini.index('[DlssNr]'):]
section = section[:re.search(r'^\[(?!DlssNr\])', section[1:], re.M).start() + 1] if re.search(
    r'^\[(?!DlssNr\])', section[1:], re.M) else section
guard(re.search(r'^ScanMatch=auto$', section, re.M) is not None and 'Default (auto) is false\nScanMatch=auto' in section,
      'ScanMatch ships in [DlssNr] as auto, documented as false')

scan = read('retoque/dlssnr/DlssNr_ExposureScan.cpp')
record = scan[scan.index('void RecordRowLocked()'):]
guard(record.index('if (!MatchOnLocked())') < record.index('AddRow('), 'no row is recorded with the key off')
best = scan[scan.index('float BestValue(int* outIndex'):]
best = best[:best.index('\n}\n')]
guard(best.index('if (MatchOnLocked())') < best.index('BestLocked()'),
      'BestValue keeps widest travel when the key is off, and only then')
widest = scan[scan.index('const Tracked* BestLocked()'):]
widest = widest[:widest.index('\n}\n')]
guard('MatchOnLocked' not in widest and 'bestRatio' in widest, 'the widest-travel rule itself is untouched')
due = scan[scan.index('std::optional<DlssNr::ExposureMatch::History> MatchDueLocked()'):]
guard(due.index('if (!MatchOnLocked())') < due.index('history.Keep('), 'nothing is judged with the key off')
wanted = scan[scan.index('bool MeterWanted()'):]
wanted = wanted[:wanted.index('\n}\n')]
guard('DlssNrEnabled' in wanted and 'Wanted()' in wanted and 'DlssNrScanMatch' in wanted,
      'the meter is wanted only with NR on, the scan wanted and the key on')

renderer = read('retoque/shaders/dlssnr/DlssNr_Dx12.cpp')
guard('UpdatePulse(cmdList, device, cfg.DlssNrPulseMetric.value_or_default(), DlssNr::ExposureScan::MeterWanted(),'
      in renderer, 'the pulse pass is asked for the meter only when the scan wants it')
diagnostics = read('retoque/shaders/dlssnr/DlssNr_Dx12_Diagnostics.cpp')
update = diagnostics[diagnostics.index('void UpdatePulse('):]
update = update[:update.index('\n}\n')]
guard('if ((!enabled && !meter) || !DlssNr_Pulse_Dx12::Available())' in update,
      'with the metric and the meter off the pulse pass stops, as before')
guard('if (enabled && g_pulse.tracker.Add(' in update, 'the meter alone files nothing under the pulse metric')

menu = read('retoque/dlssnr/DlssNr_Menu.cpp')
control = menu[menu.index('bool match = config->DlssNrScanMatch.value_or_default();') - 400:]
control = control[:control.index('Choose the buffer by matching it to the light') + 200]
guard('DlssNrWhitePointSource.value_or_default() == 2 && DlssNr::PulseAvailable()' in control and
      '!config->DlssNrUseProxy.value_or_default()' in control,
      'the checkbox shows only where it acts: the scan is the source and its meter can run')

project = read('OptiScaler/OptiScaler.vcxproj')
filters = read('OptiScaler/OptiScaler.vcxproj.filters')
guard(project.count('<ClInclude Include="..\\retoque\\dlssnr\\DlssNr_ExposureMatch.h" />') == 1 and
      filters.count('<ClInclude Include="..\\retoque\\dlssnr\\DlssNr_ExposureMatch.h">') == 1,
      'the header is registered in the project, once')

with tempfile.TemporaryDirectory(prefix='nr-exposure-match-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-fno-sanitize-recover=undefined', '-fno-omit-frame-pointer', '-I', str(retoque),
                    str(here / 'test.cpp'), '-o', binary], check=True)
    result = subprocess.run([binary, str(here / 'data')])
    if result.returncode != 0:
        failures.append('test.cpp')

if failures:
    print(f'FAIL: {len(failures)}: ' + ', '.join(failures))
    sys.exit(1)
print('PASS: the match rule, its recordings and its wiring')

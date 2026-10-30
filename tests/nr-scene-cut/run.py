#!/usr/bin/env python3
"""Scene-cut detection (docs/dlssnr/design/scene-cut-detection.md): the pure rule compiled under ASan/UBSan and driven
over synthetic sequences (static, pans, cuts, flashes, an explosion, fades, a dissolve), its hysteresis and its reset
request, plus source guards: stdlib only, the pulse grid's layout, registered in the project. No GPU, no shader."""
from pathlib import Path
import re
import subprocess
import tempfile

here = Path(__file__).resolve().parent
root = here.parents[1]
opti = root / 'OptiScaler'
retoque = root / 'retoque'

header = (retoque / 'dlssnr/DlssNr_SceneCut.h').read_text()

# Pure: the standard library and nothing else, so the host can hold the production rule.
includes = re.findall(r'#include\s*[<"]([^>"]+)[>"]', header)
assert includes and all('/' not in name and '.' not in name for name in includes), includes

# Only the game's frame is read off the pulse grid: luminance in (channel 1) over pixels read (channel 2), never the
# pass's own output (channel 0) -- a statistic of what this pass writes is a feedback loop.
pulse_reader = header[header.index('inline Frame FromPulseGrid('):]
pulse_reader = pulse_reader[:pulse_reader.index('\n}\n')]
assert re.search(r'tile\[1\]', pulse_reader) and re.search(r'tile\[2\]', pulse_reader), 'reads luminance in / pixels'
assert not re.search(r'tile\[0\]', pulse_reader), 'reads the pass output'

# Registered in the project, once each.
project = (opti / 'OptiScaler.vcxproj').read_text(encoding='utf-8-sig')
filters = (opti / 'OptiScaler.vcxproj.filters').read_text(encoding='utf-8-sig')
assert project.count('<ClInclude Include="..\\retoque\\dlssnr\\DlssNr_SceneCut.h" />') == 1
assert filters.count('<ClInclude Include="..\\retoque\\dlssnr\\DlssNr_SceneCut.h">') == 1

print('PASS: stdlib only, reads the game\'s frame off the pulse grid and never the output, registered', flush=True)

with tempfile.TemporaryDirectory(prefix='optiscaler-nr-scene-cut-') as directory:
    binary = str(Path(directory) / 'test')
    subprocess.run(['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-fno-sanitize-recover=undefined', '-fno-omit-frame-pointer', '-I', str(retoque),
                    str(here / 'test.cpp'), '-o', binary], check=True)
    subprocess.run([binary], check=True)

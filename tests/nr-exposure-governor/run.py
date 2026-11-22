#!/usr/bin/env python3
"""The exposure governor (docs/dlssnr/design/exposure-governor.md): the pure rule compiled under ASan/UBSan, mutants of
its key lines (each must make the cases fail), and source guards on the key, its wiring and its menu control.
No GPU: this checks the rule and where it is called, not the picture."""
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / '_support'))
from nr_sources import renderer_source  # noqa: E402

here = Path(__file__).resolve().parent
root = here.parents[1]
retoque = root / 'retoque'
header = retoque / 'dlssnr/DlssNr_ExposureGovernor.h'
FLAGS = ['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
         '-fno-omit-frame-pointer']

# A mutant may leave a parameter unused; it still has to build and must still fail the cases.
MUTANT_FLAGS = [f for f in FLAGS if f not in ('-Werror', '-Wall', '-Wextra')] + ['-w']


def build_and_run(include_dir, name, flags=FLAGS):
    binary = Path(include_dir) / name
    subprocess.run(flags + ['-I', str(include_dir), '-I', str(retoque), str(here / 'cases.cpp'), '-o', str(binary)],
                   check=True)
    return subprocess.run([str(binary)], capture_output=True, text=True)


with tempfile.TemporaryDirectory(prefix='optiscaler-nr-exposure-governor-') as directory:
    clean = build_and_run(directory, 'cases')
    sys.stdout.write(clean.stdout)
    assert clean.returncode == 0, clean.stderr

    # Mutants: the rule with one line changed must not pass the cases. (The include dir is searched before retoque.)
    source = header.read_text()
    mutants = {
        'attack and release swapped': ('std::clamp(error, -kReleaseStopsPerSecond * step, kAttackStopsPerSecond * step)',
                                       'std::clamp(error, -kAttackStopsPerSecond * step, kReleaseStopsPerSecond * step)'),
        'no hysteresis': ('_tracking = std::fabs(error) > (_tracking ? kSettleStops : kEngageStops);',
                          '_tracking = std::fabs(error) > 0.0f;'),
        'settle as engage': ('(_tracking ? kSettleStops : kEngageStops)', 'kEngageStops'),
        'no hold on unreadable': ('if (unreadable)\n        {', 'if (unreadable && false)\n        {'),
        'no re-prime on reset': ('const bool requested = reprime || gap || _pending;',
                                 'const bool requested = gap || _pending;'),
        'no gap re-prime': ('const bool requested = reprime || gap || _pending;',
                            'const bool requested = reprime || _pending;'),
        'no step time bound': ('const float step = std::min(dt, kMaxStepSeconds);', 'const float step = dt;'),
        'reset forgets nothing': ('void Reset() { *this = State {}; }', 'void Reset() {}'),
        'pending dropped on hold': ('_pending = requested;\n                return { _applied, Outcome::Held };',
                                    '_pending = false;\n                return { _applied, Outcome::Held };'),
        'step by frame not by time': ('const float step = std::min(dt, kMaxStepSeconds);',
                                      'const float step = 1.0f / 60.0f;'),
    }
    for name, (old, new) in mutants.items():
        assert source.count(old) == 1, f'mutant anchor missing or repeated: {name}'
        mutant_dir = Path(directory) / re.sub(r'\W+', '_', name)
        (mutant_dir / 'dlssnr').mkdir(parents=True)
        (mutant_dir / 'dlssnr/DlssNr_ExposureGovernor.h').write_text(source.replace(old, new))
        shutil.copy(retoque / 'dlssnr/DlssNr_Exposure.h', mutant_dir / 'dlssnr/DlssNr_Exposure.h')
        result = build_and_run(mutant_dir, 'cases', MUTANT_FLAGS)
        assert result.returncode != 0, f'mutant survived: {name}'
    print(f'{len(mutants)} mutants killed')

# --- source guards --------------------------------------------------------------------------------------------------
config_h = (root / 'OptiScaler/Config.h').read_text()
config_cpp = (root / 'OptiScaler/Config.cpp').read_text()
ini = (root / 'OptiScaler.ini').read_text()
config_md = (root / 'Config.md').read_text()
menu = (retoque / 'dlssnr/DlssNr_Menu.cpp').read_text()
renderer = renderer_source()
internal = (retoque / 'shaders/dlssnr/DlssNr_Dx12_Internal.h').read_text()

assert re.search(r'CustomOptional<bool>\s+DlssNrExposureGovernor\s*\{\s*false\s*\}', config_h), 'off by default'
assert 'DlssNrExposureGovernor.set_from_config(readBool("DlssNr", "ExposureGovernor"))' in config_cpp
assert re.search(r'ini\.SetValue\("DlssNr", "ExposureGovernor",\s*GetBoolValue\(Instance\(\)->DlssNrExposureGovernor\.'
                 r'value_for_config\(\)\)\.c_str\(\)\)', config_cpp)
assert re.search(r'^ExposureGovernor=auto$', ini, re.M) and re.search(r'^ExposureGovernor=auto$', config_md, re.M)

# One state for the stream, in the renderer's state; the governor sits after the white point is resolved and before the
# shader's live read is decided, and the live read is off while it is wanted.
assert internal.count('DlssNr::ExposureGovernor::State exposureGovernor;') == 1
resolved = renderer.index('ResolveWhitePoint(whitePointSource, whitePointScale, whitePointTrim, isHdrBuffer')
governed = renderer.index('g_nr.exposureGovernor.Update(')
live = renderer.index('if (!exposureGovernorWanted && DlssNr::Exposure::LiveWanted(')
assert resolved < governed < live
wanted = renderer[renderer.index('const bool exposureGovernorWanted'):governed]
assert 'cfg.DlssNrExposureGovernor.value_or_default()' in wanted and 'whitePointSource == 1' in wanted
assert 'isHdrBuffer' in wanted
update = renderer[governed - 400:governed + 600]
assert 'frameReset && !frameResetIsPolicy' in update, 'the game\'s reset re-primes; the zero-guide policy does not'
assert 'PlausibleImpliedWhite(impliedWhite)' in update and 'g_nr.gamePreExposure / g_nr.gameExposure' in update
assert 'DlssNr::Exposure::ValidSample(g_nr.gameExposure)' in renderer[renderer.index('exposureGovernorWanted &&'):governed]
assert renderer.count('g_nr.exposureGovernor.Update(') == 1
# Off, the white point is ResolveWhitePoint's, byte for byte: the only writes to whitePoint before the live decision
# are inside the branch that needs the key.
between = renderer[resolved:live]
assert between.count('whitePoint =') == 1 and between.index('whitePoint =') > between.index('exposureGovernorWanted &&')

# The menu control: D3D12 only, under the game's-exposure source, with its own ID.
assert re.search(r'if \(source == 1 && !vk\)\s*\{\s*bool governor = config->DlssNrExposureGovernor\.value_or_default\(\);',
                 menu)
assert 'Steady exposure###nrExposureGovernor' in menu
# The Vulkan route does not read the key.
vulkan = (retoque / 'dlssnr/DlssNrFeature_Vk.cpp').read_text()
assert 'ExposureGovernor' not in vulkan

print('exposure governor: source guards ok')

#!/usr/bin/env python3
"""Build the production portable codec and exact Config transaction methods under sanitizers."""
import pathlib
import subprocess
import sys
import tempfile

root = pathlib.Path(__file__).resolve().parents[2]
config_h = (root / 'OptiScaler/Config.h').read_text(encoding='utf-8-sig')
config_cpp = (root / 'OptiScaler/Config.cpp').read_text()
optional = config_h[config_h.index('enum HasDefaultValue'):config_h.index('constexpr inline int UnboundKey')]
methods = config_cpp[config_cpp.index('DlssNrResolvedPassSettings Config::DlssNrMasterSettingsUnlocked'):config_cpp.index('bool Config::Reload(')]
api = config_h[config_h.index('    DlssNrPassSnapshot GetDlssNrPassSnapshot'):config_h.index('    // Multiplies the (auto or manual) white point')]
fields = '\n'.join(line for line in config_h.splitlines() if any(f' {name} ' in line for name in
    ('DlssNrIntensity', 'DlssNrLocalStructure', 'DlssNrLocalTone', 'DlssNrSkinStructure', 'DlssNrStyle',
     'DlssNrPreset', 'DlssNrAutoMask', 'DlssNrPasses', 'DlssNrIndividualPassSettings')) and 'CustomOptional<' in line)
source = '''#include <concepts>
#include <optional>
#include <mutex>
#include "DlssNr_PassSettings.h"
#define SI_NO_CONVERSION
#include <SimpleIni.h>
''' + optional + '\nclass Config { public:\n' + fields + '\n' + api + '''
private:
mutable std::mutex _dlssNrPassSettingsMutex;
DlssNr::PassConfig::Overrides _dlssNrPassOverrides;
DlssNrResolvedPassSettings DlssNrMasterSettingsUnlocked() const;
};
''' + methods + (root / 'tests/nr-pass-config/cases.cpp').read_text()
with tempfile.TemporaryDirectory(prefix='nr-pass-config-') as temp:
    temp = pathlib.Path(temp)
    (temp / 'test.cpp').write_text(source)
    subprocess.run(['g++', '-std=c++20', '-O1', '-g', '-pthread', '-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer', '-Wall', '-Wextra', '-Werror',
                    '-isystem', str(root / 'external/simpleini'), '-I' + str(root / 'retoque/dlssnr'),
                    str(temp / 'test.cpp'), '-o', str(temp / 'test')], check=True)
    subprocess.run([str(temp / 'test')], cwd=temp, check=True)
# Four config points and scoped parsing: tests may not substitute generic parser rewrites.
for key in ('Passes', 'IndividualPassSettings'):
    assert f'DlssNr{key} {{ ' in config_h
    assert f'"DlssNr", "{key}"' in config_cpp[:config_cpp.index('bool Config::SaveIni()')]
    assert f'ini.SetValue("DlssNr", "{key}"' in config_cpp
    assert f'{key}=auto' in (root / 'OptiScaler.ini').read_text()
print('PASS: both master keys have declaration/default, read, save and shipped INI entries')
# The round trip in cases.cpp replays these two lines of Config.cpp; keep it pinned to them.
assert 'DlssNrPasses.set_from_config(DlssNr::PassConfig::Count(ini.GetValue("DlssNr", "Passes")));' in config_cpp
save = config_cpp[config_cpp.index('ini.SetValue("DlssNr", "Passes",'):]
assert 'return DlssNr::PassConfig::BoundCount(count);' in save[:save.index(';\n')]
ini = (root / 'OptiScaler.ini').read_text()
passes_comment = ini[:ini.index('\nPasses=auto')]
passes_comment = passes_comment[passes_comment.rindex('\n\n'):]
assert 'Sequential D3D12 model passes: 1..10, default (auto) 1.' in passes_comment
assert 'video memory grow with the' in passes_comment and 'Values outside 1..10 are bounded' in passes_comment
# The RR route keeps its own bound of three (DlssNr_PassSettings.h, MaxRRPasses).
sys.path.insert(0, str(root / 'tests/_support'))
from nr_sources import renderer_source  # noqa: E402
renderer = renderer_source()
assert renderer.count('DlssNr::PassConfig::BoundRRCount(cfg.DlssNrRRPasses.value_or_default())') == 2
assert 'BoundCount(cfg.DlssNrRRPasses' not in renderer
assert 'RRPasses' in ini and 'running after native Ray Reconstruction: 1..3' in ini
print('PASS: the read/save lines the round trip replays, the shipped 1..10 comment, and RR kept at three')

#!/usr/bin/env python3
"""DlssNr_GameProfiles.h: the table, the exe match, the apply rules and the Proton nvapi64.dll search, plus the
wiring that keeps it inert with NR off and the four config points of [DlssNr] GameProfiles. See README.md."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

here = Path(__file__).resolve().parent
root = here.parents[1]


def fail(message):
    print(f'FAIL: {message}', file=sys.stderr)
    sys.exit(1)


with tempfile.TemporaryDirectory(prefix='optiscaler-nr-game-profiles-') as directory:
    binary = str(Path(directory) / 'cases')
    subprocess.run(['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer', '-I', str(root / 'retoque'), str(here / 'cases.cpp'), '-o', binary],
                   check=True)
    subprocess.run([binary], check=True)

source = (root / 'retoque/dlssnr/DlssNr_GameProfiles.cpp').read_text()
apply_body = source[source.index('void Apply()'):]
gate = apply_body.find('DlssNrEnabled.value_or_default()')
first_write = min(i for i in (apply_body.find('ApplyProfile('), apply_body.find('FindNvapi()')) if i >= 0)
if gate < 0 or gate > first_write:
    fail('Apply touches settings before checking that NR is on')
if 'DlssNrGameProfiles.value_or_default()' not in apply_body[:first_write]:
    fail('Apply does not honour [DlssNr] GameProfiles before touching settings')
if 'set_volatile_value' not in source or re.search(r'NvapiDllPath\s*=', source):
    fail('NvapiPath must be set volatile, never assigned (an assignment is written back by a menu save)')

dllmain = (root / 'OptiScaler/dllmain.cpp').read_text()
quirks = dllmain.index('ApplyGameConfigs(State::Instance().gameExe')
call = dllmain.find('DlssNr::GameProfiles::Apply();')
# Right after upstream's game configs, inside CheckQuirks, which runs before CheckWorkingMode loads NVAPI.
if call < 0 or not quirks < call < dllmain.index('\n}', quirks):
    fail('Apply is not called inside CheckQuirks after upstream ApplyGameConfigs')
if dllmain.count('DlssNr::GameProfiles::Apply();') != 1:
    fail('Apply must be called exactly once')

config_h = (root / 'OptiScaler/Config.h').read_text()
config_cpp = (root / 'OptiScaler/Config.cpp').read_text()
ini = (root / 'OptiScaler.ini').read_text()
checks = [
    ('Config.h declaration', 'CustomOptional<bool> DlssNrGameProfiles { true };' in config_h),
    ('Config.cpp read', 'DlssNrGameProfiles.set_from_config(readBool("DlssNr", "GameProfiles"));' in config_cpp),
    ('Config.cpp save', 'ini.SetValue("DlssNr", "GameProfiles"' in config_cpp),
    ('shipped ini', re.search(r'^\[DlssNr\][\s\S]*?^GameProfiles=auto$', ini, re.M) is not None),
]
for name, ok in checks:
    if not ok:
        fail(f'[DlssNr] GameProfiles is missing its {name}')
print('PASS: wiring -- gated on NR and GameProfiles, volatile NvapiPath, one call after ApplyGameConfigs, '
      'four config points')

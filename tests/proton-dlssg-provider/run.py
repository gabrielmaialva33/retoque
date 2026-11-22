#!/usr/bin/env python3
"""Under Proton, the driver's Wine DLSS-G is answered with the game's newer copy when the Ada MFG unlock is on.

retoque/framegen/dlssg/ProtonDlssgProvider.h is compiled under ASan/UBSan and checked on real paths and every input of
its decision; each rule is broken once and must fail. The load hook's wiring in LibraryLoad_Hooks.cpp and the config
key's round trip are read off the source. No Wine, no NGX, no game.
"""
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
RULE = ROOT / 'retoque/framegen/dlssg/ProtonDlssgProvider.h'
HOOK = ROOT / 'OptiScaler/hooks/LibraryLoad_Hooks.cpp'
FLAGS = ['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
         '-fno-omit-frame-pointer']

UNIT = r'''
#include <framegen/dlssg/ProtonDlssgProvider.h>
#include <cstdio>
using namespace ProtonDlssgProvider;
static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x); ++failures; } } while (0)
int main()
{
    // Paths as the hook sees them: lexically normalised and lower-cased.
    CHECK(IsDriverWineCopy(L"z:\\run\\host\\usr\\lib\\nvidia\\wine\\nvngx_dlssg.dll"));   // pressure-vessel
    CHECK(IsDriverWineCopy(L"z:\\usr\\lib\\nvidia\\wine\\nvngx_dlssg.dll"));              // plain Proton/Wine
    CHECK(!IsDriverWineCopy(L"s:\\steamapps\\common\\the witcher 3\\bin\\x64_dx12\\nvngx_dlssg.dll"));
    CHECK(!IsDriverWineCopy(L"z:\\run\\host\\usr\\lib\\nvidia\\wine\\nvngx_dlssd.dll"));  // another feature
    CHECK(!IsDriverWineCopy(L"z:\\run\\host\\usr\\lib\\nvidia\\wine\\_nvngx.dll"));
    CHECK(!IsDriverWineCopy(L"nvngx_dlssg.dll"));                                         // bare name
    CHECK(!IsDriverWineCopy(L"c:\\games\\wine\\nvngx_dlssg.dll"));                        // a "wine" folder, not NVIDIA's
    CHECK(!IsDriverWineCopy(L"z:\\run\\host\\usr\\lib\\nvidia\\wine\\nvngx_dlssg.dll.bak"));

    const Version game { 310, 9, 16, 0 }, driver { 310, 2, 1, 0 }, same { 310, 2, 1, 0 }, older { 310, 1, 0, 0 };
    CHECK(UseGameCopy(true, true, true, true, true, game, driver));    // The Witcher 3, RTX 4090, 2026-11-22
    CHECK(!UseGameCopy(false, true, true, true, true, game, driver));  // unlock off: NGX's choice stands
    CHECK(!UseGameCopy(true, false, true, true, true, game, driver));  // opted out
    CHECK(!UseGameCopy(true, true, false, true, true, game, driver));  // not Wine (Windows)
    CHECK(!UseGameCopy(true, true, true, false, true, game, driver));  // not the driver's copy
    CHECK(!UseGameCopy(true, true, true, true, false, game, driver));  // versions unknown
    CHECK(!UseGameCopy(true, true, true, true, true, same, driver));   // not newer
    CHECK(!UseGameCopy(true, true, true, true, true, older, driver));  // older
    CHECK(UseGameCopy(true, true, true, true, true, Version { 310, 2, 1, 1 }, driver)); // build number counts
    if (failures)
        return 1;
    std::puts("PASS: driver Wine copy recognised on real paths; decision over every input");
    return 0;
}
'''

MUTANTS = {
    'any folder': ('lowered.find(kDir) != std::wstring_view::npos', 'true'),
    'any file': ('lowered.substr(lowered.size() - kFile.size()) == kFile', 'true'),
    'unlock ignored': ('return unlockOn && preferGame', 'return preferGame'),
    'opt-out ignored': ('unlockOn && preferGame && wine', 'unlockOn && wine'),
    'Windows too': ('&& wine && driverCopy', '&& driverCopy'),
    'unknown versions': ('&& gameCopyKnown && game > driver', '&& game > driver'),
    'equal counts as newer': ('game > driver', 'game >= driver'),
}


def run(include_root, folder, expect_pass):
    (folder / 'unit.cpp').write_text(UNIT)
    built = subprocess.run(FLAGS + ['-I' + str(include_root), str(folder / 'unit.cpp'), '-o', str(folder / 'unit')],
                           capture_output=True, text=True)
    if built.returncode != 0:
        if expect_pass:
            raise SystemExit(built.stderr)
        return False
    ran = subprocess.run([str(folder / 'unit')], capture_output=True, text=True)
    if expect_pass and ran.returncode != 0:
        raise SystemExit(ran.stdout + ran.stderr)
    return ran.returncode == 0


hook = HOOK.read_text()
branch = hook[hook.index('if (libName.contains(L"nvngx_dlssg"))'):]
branch = branch[:branch.index('// NGX OTA')]
for guard in ('std::filesystem::path(dlssgPath).lexically_normal()',      # the full path, not libName
              'ProtonDlssgProvider::IsDriverWineCopy(fullLowered)',
              'UseGameCopy(Config::Instance()->FGDLSSGAdaMfgUnlock.value_or_default(),',
              'FGDLSSGAdaPreferGameProvider.value_or_default()',
              'State::Instance().isRunningOnLinux',
              'dlssgPath = gamePath;',
              'NtdllProxy::LoadLibraryExW_Ldr(dlssgPath.c_str(), NULL, 0)',
              'MfgUnlock::TryApply(dlssgSnippet)'):
    assert branch.count(guard) == 1, f'hook wiring: {guard!r} not found once'
assert branch.index('dlssgPath = gamePath;') < branch.index('LoadLibraryExW_Ldr(dlssgPath'), 'redirect after the load'
assert branch.index('LoadLibraryExW_Ldr(dlssgPath') < branch.index('MfgUnlock::TryApply(dlssgSnippet)')

config_h = (ROOT / 'OptiScaler/Config.h').read_text()
config_cpp = (ROOT / 'OptiScaler/Config.cpp').read_text()
ini = (ROOT / 'OptiScaler.ini').read_text()
assert 'CustomOptional<bool> FGDLSSGAdaPreferGameProvider { true };' in config_h
assert 'FGDLSSGAdaPreferGameProvider.set_from_config(readBool("DLSSG", "AdaPreferGameProvider"));' in config_cpp
assert 'ini.SetValue("DLSSG", "AdaPreferGameProvider",' in config_cpp
assert '\nAdaPreferGameProvider=auto\n' in ini.replace('\r\n', '\n')

with tempfile.TemporaryDirectory(prefix='retoque-proton-dlssg-') as folder:
    folder = Path(folder)
    run(ROOT / 'retoque', folder, True)
    rule = RULE.read_text()
    survivors = []
    for index, (name, (old, new)) in enumerate(MUTANTS.items()):
        assert rule.count(old) == 1, f'mutant {name!r} no longer applies'
        root = folder / f'm{index}'
        (root / 'framegen/dlssg').mkdir(parents=True)
        (root / 'framegen/dlssg/ProtonDlssgProvider.h').write_text(rule.replace(old, new))
        if run(root, root, False):
            survivors.append(name)
    if survivors:
        raise SystemExit(f'mutants survived: {survivors}')
print(f'PASS: {len(MUTANTS)} rule mutants fail; load hook wiring and AdaPreferGameProvider round trip present')

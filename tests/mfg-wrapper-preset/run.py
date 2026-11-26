#!/usr/bin/env python3
"""Production MFG wrapper/preset policies on synthetic memory, ASan/UBSan."""
from pathlib import Path
import subprocess
import re
import argparse
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--mutations', action='store_true')
args = parser.parse_args()

root = Path(__file__).resolve().parent
repo = root.parents[1]
with tempfile.TemporaryDirectory(prefix='mfg-wrapper-preset-') as tmp:
    binary = str(Path(tmp) / 'test')
    subprocess.run(['g++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g',
                    '-I', str(repo / 'retoque/framegen/dlssg'),
                    str(root / 'test.cpp'), '-o', binary], check=True)
    subprocess.run([binary], check=True)

# Thin Windows glue is compiled verbatim with fake modules, so passing the actual loaded handle,
# both loaded copies and the version gate are behavior-tested, not inferred from a helper test.
source = (repo / 'retoque/framegen/dlssg/MfgUnlock.cpp').read_text()
a = source.index('bool PlanWrapperClamp(')
b = source.index('// Plans removing the driver', a)
plan = source[a:b]
a = source.index('    // Actual load handle first:')
b = source.index('    // Publish only after', a)
apply = source[a:b]
harness = (root / 'wrapper.cpp').read_text().replace('// @@PLAN@@', plan).replace('// @@APPLY@@', apply)
with tempfile.TemporaryDirectory(prefix='mfg-wrapper-glue-') as tmp:
    generated = Path(tmp) / 'wrapper.cpp'
    generated.write_text(harness)
    binary = str(Path(tmp) / 'wrapper')
    subprocess.run(['g++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g',
                    '-I', str(repo / 'retoque/framegen/dlssg'), str(generated), '-o', binary], check=True)
    subprocess.run([binary], check=True)

# Keep the few cross-module wiring and configuration points attached to the tested policies.
load = (repo / 'OptiScaler/hooks/LibraryLoad_Hooks.cpp').read_text()
proxy = (repo / 'OptiScaler/proxies/Streamline_Proxy.h').read_text()
assert 'MfgUnlock::TryApply(nullptr, dlssgModule);' in load
assert 'MfgUnlock::TryApply(nullptr, dlssg);' in proxy
assert 'MfgUnlock::TryApply(State::Instance().optiDLSSG);' in proxy
hook = (repo / 'OptiScaler/nvapi/NvApiHooks.cpp').read_text()
assert 'if (settingId == DlssgPresetPolicy::kSettingId)' in hook
assert 'return DlssgPresetPolicy::AnswerRead(result, pSetting);' in hook
config = (repo / 'OptiScaler/Config.cpp').read_text()
assert 'readString("DLSSG", "AdaMfgPreset", true)' in config
assert 'ini.SetValue("DLSSG", "AdaMfgPreset",' in config
assert 'FGDLSSGAdaMfgPreset.value_for_config()' in config
assert 'if (!DlssgPresetPolicy::Parse(FGDLSSGAdaMfgPreset.value_or_default()))' in config
assert 'FGDLSSGAdaMfgPreset { "auto" }' in (repo / 'OptiScaler/Config.h').read_text()
assert 'AdaMfgPreset=auto' in (repo / 'OptiScaler.ini').read_text()
assert 'AdaMfgPreset=auto' in (repo / 'Config.md').read_text()
print('Load-hook, private-loader, DRS-hook and config wiring: PASS')

# Compile both new Windows adapters unchanged against explicit, minimal Config/NVAPI/ImGui fakes.
with tempfile.TemporaryDirectory(prefix='mfg-preset-glue-') as tmp:
    binary = str(Path(tmp) / 'preset')
    subprocess.run(['g++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g',
                    '-I', str(root / 'mocks'), '-I', str(repo / 'retoque/framegen/dlssg'),
                    str(root / 'preset_glue.cpp'), '-o', binary], check=True)
    for preset in ('auto', 'a', 'c'):
        subprocess.run([binary, preset], check=True)

# Actual CustomOptional, INI parser, declaration and read/save statements round-trip through SimpleIni.
header = (repo / 'OptiScaler/Config.h').read_text()
def function(text, anchor):
    start = text.index(anchor)
    end = text.index('{', start) + 1
    depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}')
        end += 1
    return text[start:end]
optional = header[header.index('enum HasDefaultValue'):header.index('constexpr inline int UnboundKey')]
declaration = re.search(r'CustomOptional<std::string> FGDLSSGAdaMfgPreset[^;]*;', header).group()
reads = config[config.index('            FGDLSSGAdaMfgPreset.set_from_config('):
               config.index('            FGDLSSGAdaBlackwellKernels.set_from_config(')]
save = re.search(r'ini.SetValue\("DLSSG", "AdaMfgPreset",.*?;', config, re.S).group()
harness = (root / 'config.cpp').read_text()
for marker, text in (('OPTIONAL', optional), ('DECLARATION', declaration), ('READ', reads), ('SAVE', save),
                     ('READSTRING', function(config, 'std::optional<std::string> Config::readString('))):
    harness = harness.replace('// @@' + marker + '@@', text)
with tempfile.TemporaryDirectory(prefix='mfg-preset-config-') as tmp:
    source = Path(tmp) / 'config.cpp'
    source.write_text(harness)
    binary = str(Path(tmp) / 'config')
    subprocess.run(['g++', '-std=c++23', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g',
                    '-I', str(repo / 'retoque/framegen/dlssg'),
                    '-isystem', str(repo / 'external/simpleini'), str(source), '-o', binary], check=True)
    subprocess.run([binary], check=True)

if args.mutations:
    controls = [
        ('first-match', 'MfgWrapperPolicy.h', 'matches == 1 &&', 'matches >= 1 &&'),
        ('bad-ceiling', 'MfgWrapperPolicy.h', '(maximum == 1 || maximum == 3 || maximum == 5)', '(true)'),
        ('sticky-reload', 'MfgWrapperPolicy.h', 'if (!load)', 'if (!load || true)'),
        ('global-latch', 'MfgWrapperPolicy.h', 'done.find(module)', '((void)module, done.begin())'),
        ('data-match', 'MfgWrapperPolicy.h', 'if (!section.executable)', 'if (false)'),
        ('early-C', 'DlssgPresetPolicy.h', 'runtime < Version(310, 9, 16)', 'runtime < Version(310, 9, 15)'),
        ('override-auto', 'DlssgPresetPolicy.h', 'preset < Preset::A || ', ''),
        ('override-off', 'DlssgPresetPolicy.h', '!unlock || ', '(!unlock && false) || '),
    ]
    for name, filename, before, after in controls:
        original = (repo / 'retoque/framegen/dlssg' / filename).read_text()
        assert original.count(before) == 1, (name, 'mutation anchor changed')
        with tempfile.TemporaryDirectory(prefix='mfg-mutant-') as tmp:
            tmp = Path(tmp)
            (tmp / filename).write_text(original.replace(before, after))
            binary = str(tmp / name)
            subprocess.run(['g++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
                            '-I', str(tmp), '-I', str(repo / 'retoque/framegen/dlssg'),
                            str(root / 'test.cpp'), '-o', binary], check=True)
            result = subprocess.run([binary], capture_output=True, text=True)
            assert result.returncode != 0 and 'Assertion' in result.stderr, (name, result.returncode, result.stderr)
            print('REJECTED:', name)

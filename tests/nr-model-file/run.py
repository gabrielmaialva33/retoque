#!/usr/bin/env python3
"""[DlssNr] ModelFile: the rule on a fake filesystem, and where Retoque's loaders, the driver route and Config use it.

The rule header is compiled under ASan/UBSan and broken one rule at a time. The loaders, the UseProxy gate, the
model-identity lookups and the four config points are read off the source, and each guard is broken once. Nothing
is loaded: no model, no GPU, no game.
"""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
RULE = ROOT / 'retoque/dlssnr/DlssNr_ModelFile.h'
FLAGS = ['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
         '-fno-omit-frame-pointer']

RULE_MUTANTS = {
    'missing falls back': ('    named.label = Utf8(wanted.filename());\n    named.refusal = "[DlssNr] ModelFile names " + named.label +\n'
                           '                    ", which is not beside',
                           '    named.path = optiFolder / kDefaultName;\n    named.label = Utf8(wanted.filename());\n'
                           '    named.refusal = "[DlssNr] ModelFile names " + named.label +\n'
                           '                    ", which is not beside'),
    'game before OptiScaler': ('    for (const auto* folder : { &optiFolder, &exeFolder })\n    {\n        if (folder->empty())',
                               '    for (const auto* folder : { &exeFolder, &optiFolder })\n    {\n        if (folder->empty())'),
    'blank is not auto': ('    std::string value = Trimmed(setting);\n    std::ranges::transform(',
                          '    std::string value(setting);\n    std::ranges::transform('),
    'absolute path in the label': ('        named.label = Utf8(wanted.filename());\n        if (isRegularFile(wanted))',
                                   '        named.label = Utf8(wanted);\n        if (isRegularFile(wanted))'),
    'partial root taken as full': ('        return value.size() >= 3 && slash(value[2]) ? Rooting::Full : Rooting::Partial;',
                                   '        return Rooting::Full;'),
    'root-relative taken as full': ('        return value.size() >= 2 && slash(value[1]) ? Rooting::Full : Rooting::Partial;',
                                    '        return Rooting::Full;'),
    'partial root tried': ('    if (rooting == Rooting::Partial)\n', '    if (false)\n'),

    'working directory tried': ('        if (folder->empty())\n            continue;\n', ''),
    'climbing out is inside': ('        if (part == "..")\n            return false;\n', '        (void) part;\n'),
    'the folder is inside itself': (' || relative == ".")', ')'),
    'backslashes kept': ("    std::ranges::replace(value, '\\\\', '/');\n", ''),
    'not trimmed': ('    std::string value = Trimmed(setting);\n    const Rooting rooting', '    std::string value(setting);\n    const Rooting rooting'),
}

with tempfile.TemporaryDirectory(prefix='retoque-nr-model-file-') as folder:
    folder = Path(folder)
    binary = folder / 'cases'
    subprocess.run(FLAGS + ['-I' + str(ROOT), str(HERE / 'cases.cpp'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
    rule = RULE.read_text()
    for index, (name, (old, new)) in enumerate(RULE_MUTANTS.items()):
        assert rule.count(old) == 1, f'rule mutant {name!r} no longer applies'
        root = folder / f'mutant{index}'
        target = root / 'retoque/dlssnr/DlssNr_ModelFile.h'
        target.parent.mkdir(parents=True)
        target.write_text(rule.replace(old, new))
        mutant = root / 'cases'
        built = subprocess.run(FLAGS + ['-I' + str(root), str(HERE / 'cases.cpp'), '-o', str(mutant)],
                               capture_output=True)
        assert built.returncode == 0, f'rule mutant {name!r} does not compile:\n{built.stderr.decode()}'
        result = subprocess.run([str(mutant)], capture_output=True)
        assert result.returncode != 0, f'rule mutant {name!r} escaped the cases'
    print(f'PASS: {len(RULE_MUTANTS)} rule mutants fail the cases')


def read(relative):
    return (ROOT / relative).read_text(encoding='utf-8-sig').replace('\r\n', '\n')


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


PRODUCTION = [p for d in ('retoque', 'OptiScaler') for p in (ROOT / d).rglob('*') if p.suffix in ('.cpp', '.h')
              and 'forwarder' not in p.parts and 'precompile' not in p.parts]

sources = {
    'glue': read('retoque/dlssnr/DlssNr_ModelFile.cpp'),
    'menu': read('retoque/dlssnr/DlssNr_Menu.cpp'),
    'rule': read('retoque/dlssnr/DlssNr_ModelFile.h'),
    'renderer': read('retoque/shaders/dlssnr/DlssNr_Dx12.cpp'),
    'model': read('retoque/shaders/dlssnr/DlssNr_Dx12_Model.cpp'),
    'vk': read('retoque/dlssnr/DlssNrFeature_Vk.cpp'),
    'ngx': read('retoque/dlssnr/DlssNr_NgxInfo.cpp'),
    'proxy': read('retoque/dlssnr/DlssNr_Proxy.cpp'),
    'pair': read('retoque/dlssnr/DlssNr_PairCapture.cpp'),
    'diag': read('retoque/shaders/dlssnr/DlssNr_Dx12_Diagnostics.cpp'),
    'direct': read('retoque/dlssnr/DlssNr_DirectRuntime.cpp'),
    'config_h': read('OptiScaler/Config.h'),
    'config_cpp': read('OptiScaler/Config.cpp'),
    'ini': read('OptiScaler.ini'),
    'note': read('docs/dlssnr/design/model-file.md'),
    'index': read('docs/dlssnr/README.md'),
}
LITERAL = re.compile(r'FindFilePath\([^;]*"nvngx_dlssnr\.dll"')
BASENAME = 'GetModuleHandleW(L"nvngx_dlssnr.dll")'


def check(s, tree):
    # --- One place searches for the model by name, for Retoque's loaders; one more for the driver's route.
    literal = {path: len(LITERAL.findall(text)) for path, text in tree.items() if LITERAL.search(text)}
    assert set(literal) == {'retoque/dlssnr/DlssNr_NgxInfo.cpp'}, f'a loader searches by the literal name: {literal}'
    basename = {path for path, text in tree.items() if BASENAME in text}
    assert basename == {'retoque/dlssnr/DlssNr_ModelFile.cpp'}, f'the loaded model is found by base name: {basename}'

    # --- The INI's UTF-8 makes the path on every platform (on Linux std::string already is UTF-8, so no case can
    # tell; MSVC converts a std::string through the process's code page).
    rule = s['rule']
    assert 'return std::filesystem::path(std::u8string(text.begin(), text.end()));' in rule
    resolve = body(rule, 'inline Named Resolve(')
    assert 'FromUtf8(value)' in resolve and 'std::filesystem::path(value)' not in resolve, 'a path made via the code page'
    assert '.string()' not in resolve and '.generic_string()' not in resolve, 'a label not in UTF-8'

    # --- auto is the search each site always made, from its own folder, then beside the game.
    find = body(s['glue'], 'std::optional<std::filesystem::path> Find(const std::filesystem::path& siteFolder)')
    auto = find[:find.index('const auto opti = OptiFolder();')]
    assert re.search(r'if \(!Explicit\(\)\)\s*\{[^}]*Util::FindFilePath\(siteFolder, kDefaultName\);\s*'
                     r'if \(!snippet\.has_value\(\)\)\s*snippet = Util::FindFilePath\(ExeFolder\(\), kDefaultName\);'
                     r'\s*return snippet;', auto), 'auto is not the search the sites made'
    assert 'std::filesystem::is_regular_file(p, ec) && !ec' in find, 'a named file is not checked as a regular file'
    reason = body(s['glue'], 'const char* NotFoundReason()')
    assert '"nvngx_dlssnr.dll was not found beside OptiScaler or the game"' in reason, 'auto\'s reason text changed'
    assert 'Config::Instance()->DlssNrModelFileAtStart()' in s['glue'] and 'DlssNrModelFile.value' not in s['glue'], \
        'the loaders read the editable value, not the one frozen at start-up'

    # --- Every loader asks the resolver, with the folder it always passed.
    renderer = s['renderer']
    assert renderer.count('DlssNr::ModelFile::Find(g_dllDir)') == 2, 'a D3D12 create site does not ask the resolver'
    assert 'g_nr.reason = DlssNr::ModelFile::NotFoundReason();' in renderer
    assert s['model'].count('DlssNr::ModelFile::Find(g_dllDir)') == 2, 'scaling ratios or the D3D11 probe do not ask'
    snippet = body(s['vk'], 'std::optional<std::filesystem::path> FindSnippet()')
    assert 'DlssNr::ModelFile::Find(Util::DllPath().remove_filename())' in snippet
    assert s['vk'].count('DlssNr::ModelFile::NotFoundReason()') == 2, 'a Vulkan refusal keeps the default text'
    assert 'ModelFile::Find(Util::DllPath().remove_filename())' in s['pair'], 'the capture describes another model'
    # The diagnostics report describes the named model; under auto, the default file as before.
    assert re.search(r'const auto model = DlssNr::ModelFile::Explicit\(\) \? DlssNr::ModelFile::Find\(Util::DllPath\(\)'
                     r'\.remove_filename\(\)\)\s*: DlssNr::NgxInfo::FindModel\(\);', s['menu']), \
        'the diagnostics report describes the default file under a named one'
    # The module identity is the first named file found, the one the loaders loaded; a later Find does not move it.
    assert re.search(r'if \(g_named\.empty\(\)\)\s*\{\s*g_named = named\.path;', s['glue']), \
        'a later Find changes which module is taken for the loaded model'

    # --- The driver's route keeps the driver's file, and is refused when a file is named.
    model = body(s['ngx'], 'std::optional<std::filesystem::path> FindModel()')
    assert 'ModelFile' not in model.split('{', 1)[1], 'the UseProxy gate judges the file the driver does not load'
    proxy = s['proxy']
    refuse = proxy.index('if (DlssNr::ModelFile::Explicit())')
    stanza = proxy[refuse:proxy.index('}', refuse)]
    assert 'g_proxy.failed = true;' in stanza and 'return 0;' in proxy[refuse:refuse + 600]
    assert refuse < proxy.index('DlssNr::NgxInfo::FindModel()') < proxy.index('NVNGXProxy::D3D12_CreateFeature()(cmdList')

    # --- Identity and labels follow the named file.
    assert 's.loadedModel = ModulePath(DlssNr::ModelFile::LoadedModule());' in s['ngx']
    loaded = body(s['glue'], 'HMODULE LoadedModule()')
    assert 'return GetModuleHandleW(L"nvngx_dlssnr.dll");' in loaded.split('std::filesystem::path named;')[0]
    assert 'GetModuleHandleW(named.wstring().c_str())' in loaded and 'filename()' not in loaded, \
        'a named model is looked up by base name'
    assert 'DlssNr::ModelFile::Display(modelPath)' in s['ngx']
    assert 'const auto module = DlssNr::ModelFile::LoadedModule();' in s['diag']
    assert 'DlssNr::ModelFile::Display(path), label' in s['diag']
    assert 'DlssNr::ModelFile::Display(path), error' in s['direct'] and 'wstring_to_string(path), GetLastError()' \
        not in s['direct']

    # --- Four config points, frozen at the first load, kept out of the parameter list.
    assert 'CustomOptional<std::string> DlssNrModelFile { std::string("auto") };' in s['config_h']
    reload = s['config_cpp'][s['config_cpp'].index('bool Config::Reload('):s['config_cpp'].index('bool Config::SaveIni()')]
    assert 'DlssNrModelFile.set_from_config(NamedModelFile(ini.GetValue("DlssNr", "ModelFile", "auto")));' in reload \
        and 'readString("DlssNr", "ModelFile"' not in reload
    ctor = body(s['config_cpp'], 'Config::Config()')
    assert re.search(r'Reload\(absoluteFileName\);\s*(//[^\n]*\n\s*)*_dlssNrModelFileAtStart = '
                     r'DlssNrModelFile\.value_or_default\(\);\s*\}$', ctor), 'the start-up value is not frozen by the constructor'
    assert s['config_cpp'].count('_dlssNrModelFileAtStart =') == 1, 'the start-up value is written after start-up'
    save = s['config_cpp'][s['config_cpp'].index('bool Config::SaveIni()'):]
    assert 'ini.SetValue("DlssNr", "ModelFile", Instance()->DlssNrModelFile.value_for_config_or("auto").c_str());' in save
    assert re.search(r'^ModelFile=auto$', s['ini'], re.M), 'the shipped INI does not carry the key'

    assert 'model-file.md' in s['index'] and 'never falls back' in ' '.join(s['note'].replace('*', '').split())


tree = {str(p.relative_to(ROOT)): p.read_text(encoding='utf-8-sig', errors='replace').replace('\r\n', '\n')
        for p in PRODUCTION}
check(sources, tree)
print('PASS: auto unchanged at every site, loaders ask the resolver, UseProxy keeps the driver\'s file and refuses a '
      'named one, identity by path, four config points frozen at start-up')

SOURCE_MUTANTS = {
    'proxy runs a named file': ('proxy', 'if (DlssNr::ModelFile::Explicit())', 'if (false)'),
    'gate judges the named file': ('ngx', '    auto found = Util::FindFilePath(Util::DllPath().remove_filename(), "nvngx_dlssnr.dll");',
                                   '    auto found = DlssNr::ModelFile::Find(Util::DllPath().remove_filename());'),
    'value in the parameter list': ('config_cpp', 'NamedModelFile(ini.GetValue("DlssNr", "ModelFile", "auto"))',
                                    'readString("DlssNr", "ModelFile")'),
    'not frozen': ('config_cpp', '                DlssNrModelFile.set_from_config(NamedModelFile(ini.GetValue("DlssNr", "ModelFile", "auto")));\n',
                   '                DlssNrModelFile.set_from_config(NamedModelFile(ini.GetValue("DlssNr", "ModelFile", "auto")));\n'
                   '                _dlssNrModelFileAtStart = DlssNrModelFile.value_or_default();\n'),
    'code page, not UTF-8': ('rule', '    return std::filesystem::path(std::u8string(text.begin(), text.end()));',
                             '    return std::filesystem::path(std::string(text));'),
    'named looked up by base name': ('glue', 'GetModuleHandleW(named.wstring().c_str())',
                                     'GetModuleHandleW(named.filename().wstring().c_str())'),
    'not saved': ('config_cpp', '        ini.SetValue("DlssNr", "ModelFile", Instance()->DlssNrModelFile.value_for_config_or("auto").c_str());\n', ''),
    'base name identity': ('diag', 'const auto module = DlssNr::ModelFile::LoadedModule();',
                           'const auto module = GetModuleHandleW(L"nvngx_dlssnr.dll");'),
    'full path in the direct error': ('direct', 'DlssNr::ModelFile::Display(path), error', 'wstring_to_string(path), error'),
    'auto from another folder': ('glue', 'auto snippet = Util::FindFilePath(siteFolder, kDefaultName);',
                                 'auto snippet = Util::FindFilePath(OptiFolder(), kDefaultName);'),
    'diagnostics on the default file': ('menu', 'DlssNr::ModelFile::Explicit() ? DlssNr::ModelFile::Find(Util::DllPath().remove_filename())',
                                        'false ? DlssNr::ModelFile::Find(Util::DllPath().remove_filename())'),
    'identity follows the last Find': ('glue', '            if (g_named.empty())\n', '            if (true)\n'),
    'Vulkan default text': ('vk', '            Fail(DlssNr::ModelFile::NotFoundReason());',
                            '            Fail("nvngx_dlssnr.dll was not found beside OptiScaler or the game");'),
}

for name, (which, old, new) in SOURCE_MUTANTS.items():
    mutated = dict(sources)
    assert mutated[which].count(old) == 1, f'source mutant {name!r} no longer applies'
    mutated[which] = mutated[which].replace(old, new)
    mutated_tree = dict(tree)
    paths = {'diag': 'retoque/shaders/dlssnr/DlssNr_Dx12_Diagnostics.cpp', 'ngx': 'retoque/dlssnr/DlssNr_NgxInfo.cpp'}
    if which in paths:
        mutated_tree[paths[which]] = mutated[which]
    try:
        check(mutated, mutated_tree)
    except (AssertionError, ValueError):
        continue
    raise SystemExit(f'FAIL: the source guards accepted the mutant: {name}')
print(f'PASS: {len(SOURCE_MUTANTS)} source mutants rejected')

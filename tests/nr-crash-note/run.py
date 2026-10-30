#!/usr/bin/env python3
"""NR crash note (docs/dlssnr/design/engagement-funnel.md, "The crash note"): the note's text and the image-name
reader compiled as they are under ASan/UBSan with every allocation counted, plus source guards on the Win32 half: one
last-chance filter installed once and only with NR on, chained to the filter it replaced and never swallowing the
crash, nothing allocated or logged inside it, restored on an unload, and no vectored handler anywhere."""
from pathlib import Path
import re
import subprocess
import tempfile

here = Path(__file__).resolve().parent
root = here.parents[1]
failures = []


def read(relative):
    path = root / relative
    return path.read_text(encoding='utf-8-sig') if path.exists() else ''


def require(condition, message):
    if not condition:
        failures.append(message)


def function_body(source, signature):
    """The text of one function: from its signature to the brace that closes it."""
    start = source.find(signature)
    if start < 0:
        return ''
    brace = source.index('{', start)
    depth = 0
    for i in range(brace, len(source)):
        depth += {'{': 1, '}': -1}.get(source[i], 0)
        if depth == 0:
            return source[start:i + 1]
    return ''


def code_only(text):
    """Comments removed, so a guard reads what is compiled."""
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    return re.sub(r'//[^\n]*', '', text)


# --- The header: standard library and the funnel only ---------------------------------------------------------------

header = read('retoque/dlssnr/DlssNr_CrashNote.h')
require(header, 'retoque/dlssnr/DlssNr_CrashNote.h does not exist')
includes = re.findall(r'^#include\s+(\S+)', header, re.M)
require(includes and all((i.startswith('<') and '/' not in i and '.' not in i) or i == '"DlssNr_Funnel.h"'
                         for i in includes),
        f'DlssNr_CrashNote.h must include only standard headers and the funnel (found {includes})')
header_code = code_only(header)
require(not re.search(r'\bstd::(string|vector|to_string)\b|\bnew\b|malloc|printf', header_code),
        'the note is formatted by hand into the caller\'s buffer: no string, no printf, no allocation')

# --- The Win32 half -------------------------------------------------------------------------------------------------

source = read('retoque/dlssnr/DlssNr_CrashNote.cpp')
require(source, 'retoque/dlssnr/DlssNr_CrashNote.cpp does not exist')
code = code_only(source)
require(source.startswith('#include "pch.h"'), 'a translation unit starts with pch.h')
require(code.count('SetUnhandledExceptionFilter(') == 3,
        'SetUnhandledExceptionFilter is called by the install and twice by the restore, nowhere else')

filter_body = function_body(code, 'LONG WINAPI Filter(EXCEPTION_POINTERS* info)')
require(filter_body, 'DlssNr_CrashNote.cpp no longer defines Filter; update this runner')
require(re.search(r'if \(g_entered\.exchange\(true\)\)\s*return EXCEPTION_CONTINUE_SEARCH;', filter_body),
        'a second entry (a fault in the note, or a chained filter calling back) continues the search at once')
require(re.search(r'return g_previous != nullptr \? g_previous\(info\) : EXCEPTION_CONTINUE_SEARCH;\s*\}\s*$',
                  filter_body),
        'the filter ends by handing the crash to the filter it replaced, or continuing the search: never swallowed')
require('EXCEPTION_EXECUTE_HANDLER' not in filter_body and 'EXCEPTION_CONTINUE_EXECUTION' not in filter_body,
        'the filter itself never handles or resumes the crash')

write_body = function_body(code, 'void WriteNote(EXCEPTION_POINTERS* info)')
require(write_body, 'DlssNr_CrashNote.cpp no longer defines WriteNote; update this runner')
require('__try' in write_body and '__except (EXCEPTION_EXECUTE_HANDLER)' in write_body,
        'a fault while writing the note is contained in the note')
for body, name in ((filter_body, 'Filter'), (write_body, 'WriteNote')):
    require(not re.search(r'\bLOG_\w+|spdlog|std::(string|wstring|vector|to_string|format)|\bnew\b|malloc|printf|'
                          r'Config::|GetModuleFileName|GetModuleHandleEx', body),
            f'{name} runs on a dying process: no logger, no allocation, no Config, no loader-lock call')
locate = function_body(code, 'void Locate(uint64_t address')
require(locate and not re.search(r'GetModuleHandle|GetModuleFileName|LoadLibrary|\bnew\b|std::', locate),
        'the image is found by VirtualQuery and its own export name: no loader-lock call, no allocation')
require('WriteFile(g_file' in write_body, 'the note goes out through the handle opened at install')
require('kStackOverflowLine' in write_body and '0xC00000FD' in write_body,
        'a stack overflow writes the fixed line and formats nothing')

once = function_body(code, 'void InstallOnce()')
require(once, 'DlssNr_CrashNote.cpp no longer defines InstallOnce; update this runner')
require(re.search(r'if \(g_tried\.load\(std::memory_order_relaxed\)\)\s*return;', once) and
        'g_tried.exchange(true)' in once,
        'InstallOnce costs one relaxed load once tried, and installs at most once per process')
install = function_body(code, 'void Install()')
require(install, 'DlssNr_CrashNote.cpp no longer defines Install; update this runner')
require('LogToFile' in install and 'OPEN_EXISTING' in install and 'FILE_APPEND_DATA' in install,
        'the note appends to OptiScaler\'s existing log file, only with logging to a file on, and creates nothing')
require(re.search(r'FILE_SHARE_READ \| FILE_SHARE_WRITE \| FILE_SHARE_DELETE', install),
        'the second handle never blocks the logger, a reader, or the log\'s rotation')
require(install.find('CreateFileW(') < install.find('SetUnhandledExceptionFilter(Filter)'),
        'the handle is open before the filter can run')
require('g_previous = SetUnhandledExceptionFilter(Filter);' in install, 'the filter it replaces is kept to chain to')
uninstall = function_body(code, 'void Uninstall()')
require(re.search(r'const auto current = SetUnhandledExceptionFilter\(g_previous\);\s*'
                  r'if \(current != Filter\)\s*SetUnhandledExceptionFilter\(current\);', uninstall),
        'an unload gives the slot back only while it is still ours; a filter installed over ours keeps it')

for tree in ('OptiScaler', 'retoque'):
    for path in (root / tree).rglob('*'):
        if path.suffix in ('.cpp', '.h', '.hpp') and path.is_file():
            text = code_only(path.read_text(encoding='utf-8', errors='replace'))
            require('AddVectoredExceptionHandler' not in text and 'AddVectoredContinueHandler' not in text,
                    f'{path.relative_to(root)} adds a vectored handler: the crash note is last-chance only')

# --- Call sites -----------------------------------------------------------------------------------------------------

dx12 = read('retoque/shaders/dlssnr/DlssNr_Dx12.cpp')
require('#include <dlssnr/DlssNr_CrashNote.h>' in dx12, 'DlssNr_Dx12.cpp includes the crash note header')
for signature, gate in (('void EvaluateAfterUpscale(ID3D12GraphicsCommandList* cmdList', 'if (!dlssNrEnabled)'),
                        ('bool EvaluateAtPresent(ID3D12GraphicsCommandList* cmdList',
                         'if (!Config::Instance()->DlssNrEnabled.value_or_default())')):
    body = function_body(dx12, signature)
    off = body.find(gate)
    off_return = body.find('return', off)
    call = body.find('CrashNote::InstallOnce();')
    require(0 <= off < off_return < call and body.count('CrashNote::InstallOnce();') == 1,
            f'{signature.split("(")[0]} installs the crash note once it is past the NR-off return, never with NR off')
require(read('retoque/dlssnr/DlssNr_Menu.cpp').count('CrashNote::') == 0, 'the panel does not install the note')

dllmain = read('OptiScaler/dllmain.cpp')
require('#include <dlssnr/DlssNr_CrashNote.h>' in dllmain, 'dllmain.cpp includes the crash note header')
require(re.search(r'if \(lpReserved != nullptr\)\s*break;\s*(?://[^\n]*\n\s*)*DlssNr::CrashNote::Uninstall\(\);',
                  dllmain),
        'the filter is given back on an unload (FreeLibrary) only: at process exit nothing is undone')
require(dllmain.count('CrashNote::') == 1, 'dllmain only uninstalls')

vcxproj = read('OptiScaler/OptiScaler.vcxproj')
filters = read('OptiScaler/OptiScaler.vcxproj.filters')
for text, name in ((vcxproj, 'OptiScaler.vcxproj'), (filters, 'OptiScaler.vcxproj.filters')):
    require('<ClCompile Include="..\\retoque\\dlssnr\\DlssNr_CrashNote.cpp"' in text,
            f'{name} compiles DlssNr_CrashNote.cpp')

# --- Build and run ---------------------------------------------------------------------------------------------------

cases = None
if header:
    with tempfile.TemporaryDirectory(prefix='optiscaler-nr-crash-note-') as directory:
        binary = Path(directory) / 'cases'
        built = subprocess.run(['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                                '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined',
                                '-fno-omit-frame-pointer', '-I', str(root / 'retoque'), str(here / 'cases.cpp'),
                                '-o', str(binary)])
        require(built.returncode == 0, 'cases.cpp did not compile')
        if built.returncode == 0:
            cases = subprocess.run([str(binary)])

if failures:
    for failure in failures:
        print('FAIL:', failure)
if failures or cases is None or cases.returncode != 0:
    raise SystemExit(1)

print('PASS: crash note source guards')

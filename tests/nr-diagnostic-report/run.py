#!/usr/bin/env python3
"""NR diagnostic export (docs/dlssnr/design/diagnostic-export.md): the report header compiled as it is under ASan/UBSan,
plus source guards on the panel's button, the settings-contract wrapper and the NGX lookup it reads."""
from pathlib import Path
import re
import subprocess
import tempfile

here = Path(__file__).resolve().parent
root = here.parents[1]
failures = []


def read(relative):
    return (root / relative).read_text(encoding='utf-8')


def require(condition, message):
    if not condition:
        failures.append(message)


def body(source, signature):
    """From a definition's signature to the next line that closes a column-zero brace."""
    start = source.find(signature)
    if start < 0:
        return ''
    end = source.find('\n}\n', start)
    return source[start:end if end >= 0 else len(source)]


# --- The header: standard library only, so this suite compiles it unchanged ----------------------------------------

header = read('retoque/dlssnr/DlssNr_DiagnosticReport.h')
includes = re.findall(r'^#include\s+(\S+)', header, re.M)
require(includes and all(i.startswith('<') and '/' not in i and '.' not in i for i in includes),
        f'DlssNr_DiagnosticReport.h must include only standard headers (found {includes})')

# --- The panel: assembled on a click, never per frame ---------------------------------------------------------------

menu = read('retoque/dlssnr/DlssNr_Menu.cpp')
require('#include "DlssNr_DiagnosticReport.h"' in menu, 'the NR panel does not include the report header')
collect = body(menu, 'static std::string CollectDiagnosticReport(Config* config)')
require(collect, 'DlssNr_Menu.cpp no longer defines CollectDiagnosticReport; update this runner')
require(menu.count('CollectDiagnosticReport(') == 2, 'CollectDiagnosticReport is defined once and called once')
button = re.search(r'if \(ImGui::Button\(Localization::Label\("Copy NR diagnostics"\)\)\)\s*\{\s*'
                   r'ImGui::SetClipboardText\(CollectDiagnosticReport\(config\)\.c_str\(\)\);', menu)
require(menu.count('RenderDiagnosticExport(') == 2, 'the button is drawn from one place')
require(button, 'the report is assembled inside the button\'s click branch and goes straight to the clipboard')
require('namespace DR = DlssNr::DiagnosticReport;' in collect and collect.count('DR::Build(') == 2,
        'the menu builds the text with DiagnosticReport::Build only, once per way out')
gate = collect.find('if (!in.nrEnabled)\n        return DR::Build(in);')
getter = re.search(r'\b(?:DlssNr::(?!DiagnosticReport\b)|Lsfg::|GpuTiming::)\w+', collect)
first_nr = getter.start() if getter else -1
require(0 <= gate < first_nr, 'with NR off the report returns before any NR, LSFG or timing getter is asked')
require('GpuTiming::SetEnabled' not in collect and 'RetryAfterFailure' not in collect,
        'the export only reads: it never changes timing or retries the pass')
require('NgxInfo::Describe(' not in collect,
        'the export must not call NgxInfo::Describe: it caches its first answer, which LoaderRouteKnownToFault decides '
        'from; use the lookup-only NgxInfo::Described')

# --- The settings contract: the one TimingContract, with frame fields the report strips -----------------------------

diagnostics = read('retoque/shaders/dlssnr/DlssNr_Dx12_Diagnostics.cpp')
wrapper = body(diagnostics, 'std::string SettingsContractText()')
require(wrapper, 'DlssNr_Dx12_Diagnostics.cpp no longer defines SettingsContractText; update this runner')
require(wrapper.count('TimingContract(') == 1, 'SettingsContractText calls the one TimingContract')
require('std::string SettingsContractText();' in read('retoque/dlssnr/DlssNrFeature_Dx12.h'),
        'SettingsContractText is declared in the feature API the menu includes')
for key in ('stage', 'render', 'compose', 'model', 'scale', 'linear_hdr', 'compose_format', 'effective_transfer',
            'enlarge', 'cadence', 'highlight_recovery'):
    require(re.search(r'\b' + key + r'=', body(diagnostics, 'std::string TimingContract(')),
            f'TimingContract no longer writes {key}=; update kFrameFields in DlssNr_DiagnosticReport.h and this list')
    require(f'"{key}"' in header, f'kFrameFields does not strip the frame field {key}')

# --- NgxInfo: a lookup that adds and logs nothing --------------------------------------------------------------------

ngx = read('retoque/dlssnr/DlssNr_NgxInfo.cpp')
described = body(ngx, 'std::optional<Snapshot> Described(const std::filesystem::path& modelPath)')
require(described, 'DlssNr_NgxInfo.cpp no longer defines Described; update this runner')
require('emplace' not in described and 'LOG_' not in described and 'g_snapshots.find(' in described
        and 'std::lock_guard lock(g_mutex);' in described,
        'NgxInfo::Described must look up under the lock and never add, describe or log')

# --- Build and run ---------------------------------------------------------------------------------------------------

with tempfile.TemporaryDirectory(prefix='optiscaler-nr-diagnostic-report-') as directory:
    binary = Path(directory) / 'cases'
    subprocess.run(['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined', '-fno-omit-frame-pointer',
                    '-I', str(root / 'retoque'), str(here / 'cases.cpp'), '-o', str(binary)], check=True)
    cases = subprocess.run([str(binary)])

if failures:
    for failure in failures:
        print('FAIL:', failure)
if failures or cases.returncode != 0:
    raise SystemExit(1)

print('PASS: diagnostic report source guards')

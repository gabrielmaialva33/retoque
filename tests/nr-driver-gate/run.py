#!/usr/bin/env python3
"""NR minimum-driver gate (docs/dlssnr/design/driver-gate.md): the production decision header compiled under ASan/UBSan,
plus source guards on where the driver version is read, where the gate is applied, every writer of DlssNrEnabled, the
menu, and the model-create sites that must never run past NGX's FAIL_OutOfDate."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

here = Path(__file__).resolve().parent
root = here.parents[1]
failures = []


def read(relative):
    return (root / relative).read_text(encoding='utf-8-sig')


def require(condition, message):
    if not condition:
        failures.append(message)


def function_body(source, signature):
    """The text of one function: from its signature to the brace that closes it ('' when absent)."""
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


def joined(text):
    """`text` with adjacent C string literals merged ("a" "b" -> "ab"), so a wrapped literal is found whole."""
    return re.sub(r'"\s*\n\s*"', '', text)


def before(text, first, second):
    """Both present, and `first` strictly before `second`."""
    a, b = text.find(first), text.find(second)
    return 0 <= a < b


identify_h = read('OptiScaler/misc/IdentifyGpu.h')
identify = read('OptiScaler/misc/IdentifyGpu.cpp')
dllmain = read('OptiScaler/dllmain.cpp')
session = read('retoque/dlssnr/DlssNr_Session.cpp')
session_h = read('retoque/dlssnr/DlssNr_Session.h')
menu = read('retoque/dlssnr/DlssNr_Menu.cpp')
common = read('OptiScaler/menu/menu_common.cpp')
direct = read('retoque/dlssnr/DlssNr_DirectRuntime.cpp')
proxy = read('retoque/dlssnr/DlssNr_Proxy.cpp')
dx12 = read('retoque/shaders/dlssnr/DlssNr_Dx12.cpp')
pack = read('retoque/translations/optional/pt-BR.lang')
vcxproj = read('OptiScaler/OptiScaler.vcxproj')
filters = read('OptiScaler/OptiScaler.vcxproj.filters')
gate_h = root / 'retoque/dlssnr/DlssNr_DriverGate.h'

# --- The header stays portable: the cases compile it as shipped. ---------------------------------------------------
if gate_h.exists():
    gate = gate_h.read_text(encoding='utf-8-sig')
    require('#include <windows.h>' not in gate and 'pch.h' not in gate, 'DlssNr_DriverGate.h must stay free of '
            'Windows headers and pch.h')
require('retoque\\dlssnr\\DlssNr_DriverGate.h' in vcxproj and 'retoque\\dlssnr\\DlssNr_DriverGate.h' in filters,
        'DlssNr_DriverGate.h is not listed in OptiScaler.vcxproj(.filters)')

# --- 1. The version comes from NVAPI, on the GPU found by LUID. ------------------------------------------------------
require(re.search(r'uint32_t\s+nvidiaDriverVersion\s*=\s*0;', identify_h) is not None,
        'GpuInformation has no nvidiaDriverVersion (0 = unknown)')
query = function_body(identify, 'void IdentifyGpu::queryNvapi(GpuInformation& gpuInfo)')
require('GET_INTERFACE(NvAPI_SYS_GetDriverAndBranchVersion' in query,
        'queryNvapi does not read NvAPI_SYS_GetDriverAndBranchVersion')
require('gpuInfo.nvidiaDriverVersion = ' in query, 'queryNvapi does not store the driver version')

# --- 2. Applied once, from the GPU worker, with the Wine flag; decided in the fork-owned layer. ----------------------
worker = function_body(dllmain, 'DWORD WINAPI getGpuInfo(')
require('DlssNr::ApplyDriverGate(' in worker, 'getGpuInfo does not apply the NR driver gate')
call = worker[worker.find('DlssNr::ApplyDriverGate('):]
call = call[:call.find(';')]
require('State::Instance().isRunningOnLinux' in call and 'nvidiaDriverVersion' in call and 'VendorId::Nvidia' in call,
        'ApplyDriverGate must be given the vendor, the Wine flag and the NVAPI version')
require(before(worker, 'IdentifyGpu::getPrimaryGpu()', 'DlssNr::ApplyDriverGate('),
        'the gate must run after the primary GPU is known')
require('void ApplyDriverGate(bool nvidia, bool onWine, uint32_t driverVersion);' in session_h,
        'DlssNr_Session.h does not declare ApplyDriverGate')
apply = function_body(session, 'void ApplyDriverGate(bool nvidia, bool onWine, uint32_t driverVersion)')
require(before(apply, 'DriverGate::Resolve(nvidia, onWine, driverVersion)', 'DriverGate::Publish('),
        'ApplyDriverGate must resolve, then publish')
require(before(apply, 'DriverGate::Publish(', 'DlssNrEnabled.set_volatile_value(false)'),
        'ApplyDriverGate must publish before turning NR off')
require(re.search(r'if \(DriverGate::Blocks\(verdict\)\)\s*\{[^}]*DlssNrEnabled\.set_volatile_value\(false\)', apply)
        is not None, 'NR is turned off only for a blocking verdict, and through the volatile setter')
require(not re.search(r'DlssNrEnabled\s*=', apply), 'the gate must never assign DlssNrEnabled (that reaches the ini)')
require('BlockedMessage(' in apply and 'LOG_WARN' in apply, 'a blocked driver must be logged with the message')

# --- 3. Nothing turns NR back on while the driver is blocked. --------------------------------------------------------
# Every assignment of DlssNrEnabled in the shipped sources, outside tests.
writers = {}
for path in list((root / 'retoque').rglob('*.cpp')) + list((root / 'retoque').rglob('*.h')) + \
        list((root / 'OptiScaler').rglob('*.cpp')) + list((root / 'OptiScaler').rglob('*.h')):
    text = path.read_text(encoding='utf-8-sig', errors='replace')
    for m in re.finditer(r'DlssNrEnabled\s*=(?!=)', text):
        writers.setdefault(str(path.relative_to(root)), []).append(text[m.start():text.find(';', m.start())])
expected = {'retoque/dlssnr/DlssNr_Session.cpp', 'retoque/dlssnr/DlssNr_Menu.cpp', 'OptiScaler/menu/menu_common.cpp'}
require(set(writers) == expected, 'DlssNrEnabled is written somewhere the gate does not cover: '
        + ', '.join(sorted(set(writers) ^ expected)))
anyway = function_body(session, 'void SessionGuardEnableAnyway()')
require(before(anyway, 'if (DriverGate::Blocked())', 'DlssNrEnabled = true'),
        'SessionGuardEnableAnyway must refuse while the driver is blocked')
hotkey_at = common.find('if (inputDlssNr)\n')
blocked_key = common.find('if (inputDlssNr && DlssNr::DriverGate::Blocked())')
require(0 <= blocked_key < hotkey_at, 'the NR hotkey must be refused before it toggles')
if blocked_key >= 0:
    key_block = function_body(common[blocked_key:], 'if (inputDlssNr && DlssNr::DriverGate::Blocked())')
    require('inputDlssNr = false;' in key_block and 'BlockedMessage(' in key_block, 'the refused hotkey must be '
            'consumed and say why')
    require('DlssNrEnabled' not in key_block, 'the refused hotkey must not write the setting')

# --- 4. The menu: the checkbox disabled with the reason, no dead "Enable anyway" or "Retry". -------------------------
render = joined(function_body(menu, 'void RenderMenu(Config* config, float menuResScale)'))
require('const bool driverBlocked = DlssNr::DriverGate::Blocked();' in render, 'the menu does not read the gate')
require(before(render, 'ImGui::BeginDisabled(driverBlocked);', 'Localization::Label("Enable Neural Rendering")'),
        'the Enable checkbox is not disabled while the driver is blocked')
require(before(render, 'Localization::Label("Enable Neural Rendering")', 'ImGui::EndDisabled();'),
        'BeginDisabled without its EndDisabled after the checkbox')
driver_line = 'Neural Rendering is off for this session: it needs NVIDIA driver %s or newer (%s or newer ' \
              'recommended), and this one is %s. Your settings are unchanged.'
require(driver_line in render, 'the menu does not say why NR is off for the driver')
require(before(render, 'if (driverBlocked)', driver_line), 'the driver line is not gated on the block')
require('guard.action == SessionGuard::Action::Disable && !driverBlocked' in render,
        '"Enable anyway" must not be offered while the driver is blocked (it would be a dead control)')
require(re.search(r'if \(!DlssNr::DriverGate::Refuses\(\)\)\s*\{\s*ImGui::SameLine\(\);\s*'
                  r'if \(ImGui::SmallButton\(Localization::Label\("Retry"\)\)\)', render) is not None,
        '"Retry" must not be offered when the driver gate refuses the model (it would be a dead control)')
require(re.search(r'if \(!enabled\)\s*\{\s*if \(!driverBlocked\)', render) is not None,
        'the "Off. The model stays loaded" line must give way to the driver line')
require(driver_line in pack, 'the driver line has no pt-BR entry')

# --- 5. The model is never created past the gate or NGX's FAIL_OutOfDate. --------------------------------------------
create = function_body(direct, 'void* Create(const wchar_t* snippetPath')
require(before(create, 'if (DriverGate::Refuses())', 'LoadSnippet(snippetPath)'),
        'DirectRuntime::Create must refuse before it loads or initialises anything')
require(before(create, 'g_lastInit = Guarded(g_snip.init', 'DriverGate::NoteNgxResult((unsigned int) g_lastInit)'),
        'DirectRuntime::Create does not latch an Init_Ext FAIL_OutOfDate')
create_tail = create[create.find('g_lastCreate = Guarded(g_snip.create'):]
require(re.search(r'DriverGate::NoteNgxResult\(\(unsigned int\) g_lastCreate\)\)\s*return nullptr;', create_tail)
        is not None, 'DirectRuntime::Create must not hand back a feature from a FAIL_OutOfDate create')
init_tail = create[create.find('g_lastInit = Guarded(g_snip.init'):create.find('setUInt(capabilityParams')]
require('return nullptr;' in init_tail, 'a failed Init_Ext must still stop before the create')

run = function_body(proxy, 'unsigned int Run(ID3D12GraphicsCommandList* cmdList')
require(before(run, 'if (DlssNr::DriverGate::Refuses())', 'NVNGXProxy::D3D12_CreateFeature()'),
        'the proxy must refuse before it asks NGX for feature 18')
require(before(run, 'NVNGXProxy::D3D12_CreateFeature()', 'DlssNr::DriverGate::NoteNgxResult((unsigned int) created)'),
        'the proxy does not latch a FAIL_OutOfDate from NGX')

site = dx12.find('g_nr.feature = g_nr.create(')
build = dx12.rfind('if (g_nr.feature == nullptr && g_nr.output != nullptr', 0, site)
require(site > 0 and build > 0, 'the D3D12 model build site moved')
if site > 0 and build > 0:
    head = dx12[build:site]
    require('if (DlssNr::DriverGate::Refuses())' in head and 'DlssNr::DriverGate::Reason()' in head,
            'the D3D12 build must refuse, with the gate\'s reason, before it creates the model')
    tail = dx12[site:dx12.find('NoteModelFault();', site)]
    require('DlssNr::DriverGate::NoteNgxResult(initResult)' in tail and
            'DlssNr::DriverGate::NoteNgxResult(createResult)' in tail,
            'a failed D3D12 build does not latch FAIL_OutOfDate from the loader\'s answers')
    require('g_nr.reason = DlssNr::DriverGate::kOutOfDateReason;' in tail,
            'a FAIL_OutOfDate build must say the driver is the cause')

# --- 6. The cases, as shipped. ---------------------------------------------------------------------------------------
result = 1
if gate_h.exists():
    with tempfile.TemporaryDirectory(prefix='retoque-nr-driver-gate-') as folder:
        binary = Path(folder) / 'cases'
        build_cmd = ['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-pthread',
                     '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-fno-omit-frame-pointer',
                     '-I', str(root / 'retoque/dlssnr'), str(here / 'cases.cpp'), '-o', str(binary)]
        if subprocess.run(build_cmd).returncode == 0:
            result = subprocess.run([str(binary)]).returncode
        else:
            failures.append('cases.cpp did not compile')
else:
    failures.append('retoque/dlssnr/DlssNr_DriverGate.h does not exist')

for f in failures:
    print('FAIL: ' + f)
if failures or result != 0:
    sys.exit(1)
print('nr-driver-gate: all source guards and cases passed')

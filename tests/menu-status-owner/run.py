#!/usr/bin/env python3
"""Compile pure production rules and the real WndProc against minimal host fakes."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent

def body(text, anchor):
    start = text.index(anchor)
    end = text.index('\n}', start) + 2
    return text[start:end]

messages = (ROOT / 'OptiScaler/menu/input/input_system_messages.cpp').read_text()
wndproc = body(messages, 'LRESULT CALLBACK OptiInputWndProc(')
# Regression guard also fails before either new header is available.
assert 'WM_NCDESTROY' in wndproc, 'destroyed HWND retains OriginalWndProcs entry'
overlay = (ROOT / 'OptiScaler/menu/menu_overlay_dx.cpp').read_text()
present = body(overlay, 'void MenuOverlayDx::Present(')
assert present.index('std::lock_guard lock(g_dx12OverlayMutex)') < present.index('ClaimMenuWindow(hWnd)')
assert present.index('ClaimMenuWindow(hWnd)') < present.index('pDevice->QueryInterface')
claim = body(overlay, 'static bool ClaimMenuWindow(HWND hWnd)')
assert 'owner.Accept(hWnd, ownerIsWindow, GetTickCount64())' in claim
assert 'IsWindow(owner.Handle())' in claim
window = (ROOT / 'OptiScaler/menu/input/input_system_window.cpp').read_text()
install = body(window, 'bool InstallWindowSubclass(HWND hwnd)')
assert install.index('it->second != currentWndProc') < install.index('SetWindowLongPtrW')
menu = (ROOT / 'retoque/dlssnr/DlssNr_Menu.cpp').read_text()
collect = body(menu, 'static DlssNr::StatusPage::Inputs CollectStatusInputs(Config* config)')
assert collect.index('if (!in.enabled)') < collect.index('DlssNr::IsRunningVk()')
for source in ('IsRunningVk()', 'IsRunning()', 'FailureReason()', 'HookStatus()', 'Dx11RouteReport()',
               'NativeDx11SceneStatus()', 'WorkingScale()', 'RRWorkingScale()', 'CadenceStatus()',
               'DlssNrModelFileAtStart()', 'DlssNrPasses.value_or_default()', 'DlssNrCadence.value_or_default()'):
    assert source in collect, source
for forbidden in ('NgxInfo::Describe(', 'ModelFile::Find(', 'RetryAfterFailure(', 'SetEnabled(', 'SettingsContractText('):
    assert forbidden not in collect, forbidden
render = body(menu, 'void RenderStatus(Config* config)')
assert 'StatusPage::Rows(CollectStatusInputs(config))' in render
assert 'TextWrapped' in render and 'Button' not in render and 'Checkbox' not in render
common = (ROOT / 'OptiScaler/menu/menu_common.cpp').read_text()
assert 'ScopedCard::Shows("status_nr")' in common and 'DlssNr::RenderStatus(config)' in common
assert '{ "status_nr", "Status", "Neural Rendering", nullptr, RenderStatusInfo }' in common
status_unit = ((HERE / 'status_fakes.h').read_text() + '\nnamespace DlssNr {\n' + collect + '\n' + render + '\n}\n' + (HERE / 'status_main.h').read_text())
unit = (HERE / 'cases.cpp').read_text() + '\n' + body(messages, 'static WNDPROC OriginalWndProcLocked(HWND hwnd)') + '\n' + wndproc + '\n' + status_unit + '\n' + (HERE / 'main.cpp').read_text()
with tempfile.TemporaryDirectory(prefix='menu-status-owner-') as folder:
    cpp, binary = Path(folder) / 'unit.cpp', Path(folder) / 'unit'
    cpp.write_text(unit.replace('#pragma once', ''))
    subprocess.run(['g++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
                    '-fsanitize=address,undefined', '-fno-sanitize-recover=all', '-pthread',
                    '-I', str(ROOT / 'retoque'), '-I', str(HERE), str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('PASS: menu ownership, WndProc destruction and NR Status rules/wiring')

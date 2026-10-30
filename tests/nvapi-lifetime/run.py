#!/usr/bin/env python3
"""nvapi64.dll stays loaded once NvAPI_Initialize has run in OptiScaler's two startup NVIDIA checks
(IdentifyGpu::queryNvapi and dllmain's isNvidiaViaNvapi). Source slices of the shipped functions: after the initialise
call no NvAPI_Unload and no FreeLibrary of the module; before it, the paths that never initialise still free it.
Adapted from janblade/OptiScaler-F5-DLSSNR-Multipass d2c06938 (GPL-3.0)."""
from pathlib import Path
import re
import sys

root = Path(__file__).resolve().parents[2]
failures = []


def require(condition, message):
    if not condition:
        failures.append(message)


def strip_comments(text):
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    return re.sub(r'//[^\n]*', '', text)


def block(source, start_marker):
    """From `start_marker` to the brace closing the first block opened after it ('' when absent)."""
    start = source.find(start_marker)
    if start < 0:
        return ''
    brace = source.index('{', start)
    depth = 0
    for i in range(brace, len(source)):
        depth += {'{': 1, '}': -1}.get(source[i], 0)
        if depth == 0:
            return source[start:i + 1]
    return ''


FREE = 'FreeLibrary_Ldr(nvapiModule)'
UNLOAD = 'NvAPI_Unload'


def check(name, body, init_call):
    require(body != '', f'{name}: function not found')
    if not body:
        return
    at = body.find(init_call)
    require(at >= 0, f'{name}: no NvAPI_Initialize call ({init_call!r})')
    if at < 0:
        return
    after = body[at:]
    require(FREE not in after, f'{name}: nvapi64.dll is freed after NvAPI_Initialize ran')
    require(UNLOAD not in after, f'{name}: NvAPI_Unload is called after NvAPI_Initialize ran')
    require(UNLOAD not in body, f'{name}: NvAPI_Unload must not be called at all')
    require(FREE in body[:at], f'{name}: the paths that never initialise nvapi no longer free it')


identify = strip_comments((root / 'OptiScaler/misc/IdentifyGpu.cpp').read_text(encoding='utf-8-sig'))
query = block(identify, 'void IdentifyGpu::queryNvapi(GpuInformation& gpuInfo)')
check('IdentifyGpu::queryNvapi', query, 'init()')
# The failed-initialise path returns without freeing too: NvAPI_Initialize may have started work before failing.
if query:
    at = query.find('init()')
    require(query.find('return;', at) > 0, 'IdentifyGpu::queryNvapi: no early return after a failed initialise')

dllmain = strip_comments((root / 'OptiScaler/dllmain.cpp').read_text(encoding='utf-8-sig'))
lambda_body = block(dllmain, 'auto isNvidiaViaNvapi = [&]()')
require(lambda_body != '', 'dllmain isNvidiaViaNvapi: lambda not found')
if lambda_body:
    require(lambda_body.count('init()') == 1, 'dllmain isNvidiaViaNvapi: expected exactly one NvAPI_Initialize call')
    require(UNLOAD not in lambda_body, 'dllmain isNvidiaViaNvapi: NvAPI_Unload must not be called at all')
    # The branch that initialises: from the interface lookup to the brace closing `if (init...)`.
    branch = block(lambda_body, 'GET_INTERFACE(NvAPI_Initialize')
    require('init()' in branch, 'dllmain isNvidiaViaNvapi: the initialise call left its branch')
    require(FREE not in branch, 'dllmain isNvidiaViaNvapi: nvapi64.dll is freed after NvAPI_Initialize ran')
    # It leaves the lambda itself, so the shared FreeLibrary after it is reached only by paths that never initialise
    # (dxvk-nvapi, fakenvapi, no NvAPI_Initialize export).
    tail = branch[branch.find('init()'):]
    require(re.search(r'return\s+nvidiaDetected\s*;\s*\}\s*$', tail) is not None,
            'dllmain isNvidiaViaNvapi: the initialised branch must return before the shared FreeLibrary')
    require(FREE in lambda_body[lambda_body.find(branch) + len(branch):],
            'dllmain isNvidiaViaNvapi: the paths that never initialise nvapi no longer free it')

for f in failures:
    print('FAIL: ' + f)
if failures:
    sys.exit(1)
print('nvapi-lifetime: nvapi64.dll is never unloaded after NvAPI_Initialize')

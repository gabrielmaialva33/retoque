#!/usr/bin/env python3
"""Exercise DlssNr::Exposure::PlausibilityLatch rules, windowing, WhitePoint/LiveWanted fallback,
and the production D3D12 exposure meter readback functions against simulated fences."""
from pathlib import Path
import re
import subprocess
import tempfile

here = Path(__file__).resolve().parent
root = here.parents[1]


def code_mask(source):
    pattern = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'', re.S)
    return pattern.sub(lambda match: ''.join('\n' if char == '\n' else ' ' for char in match[0]), source)


def extract_block(source, signature):
    masked = code_mask(source)
    match = re.search(signature, masked, re.M)
    if not match:
        raise ValueError(f'production block missing: {signature}')
    opening = masked.find('{', match.end())
    depth = 1
    for position in range(opening + 1, len(masked)):
        if masked[position] == '{':
            depth += 1
        elif masked[position] == '}':
            depth -= 1
            if depth == 0:
                end = position + 1
                if end < len(masked) and masked[end] == ';':
                    end += 1
                return source[match.start():end]
    raise ValueError(f'production block not closed: {signature}')


def build_dx12_meter_unit(diagnostics_source, target):
    masked = code_mask(diagnostics_source)
    has_session = bool(re.search(r'\bs_meterReadback\b', masked))
    has_begin = bool(re.search(r'^bool\s+BeginMeterDispatch\s*\(', masked, re.M))
    has_shutdown = bool(re.search(r'^void\s+ShutdownExposureMeter\s*\(', masked, re.M))

    pieces = []
    if has_session:
        session_ns = re.search(r'^namespace\s*\{[^{}]*MeterReadbackSession[\s\S]*?s_meterReadback\s*;[\s\S]*?^\}',
                               masked, re.M)
        if not session_ns:
            raise ValueError('s_meterReadback namespace block not found')
        pieces.append(diagnostics_source[session_ns.start():session_ns.end()])

    # ConsumeMeterReadback may be called from BeginMeterDispatch / CopyMeterToReadback; forward-declare first.
    pieces.append('void ConsumeMeterReadback();')
    if has_begin:
        pieces.append(extract_block(diagnostics_source, r'^bool\s+BeginMeterDispatch\s*\('))
    pieces.append(extract_block(diagnostics_source, r'^void\s+CopyMeterToReadback\s*\('))
    pieces.append(extract_block(diagnostics_source, r'^void\s+ConsumeMeterReadback\s*\('))
    pieces.append(extract_block(diagnostics_source, r'^void\s+InvalidateExposureMeter\s*\('))
    if has_shutdown:
        pieces.append(extract_block(diagnostics_source, r'^void\s+ShutdownExposureMeter\s*\('))

    for piece in pieces:
        assert 'WaitForSingleObject' not in piece and 'SetEventOnCompletion' not in piece, \
            'meter readback must never block the CPU on GPU telemetry'

    template = (here / 'dx12_meter_cases.cpp').read_text()
    header = (f'#define TEST_DX12_METER_SESSION {int(has_session)}\n'
              f'#define TEST_DX12_METER_BEGIN {int(has_begin)}\n'
              f'#define TEST_DX12_METER_SHUTDOWN {int(has_shutdown)}\n')
    target.write_text(header + template.replace('// PRODUCTION_METER_FUNCTIONS', '\n\n'.join(pieces)))
    return has_session, has_begin, has_shutdown


with tempfile.TemporaryDirectory(prefix='optiscaler-nr-exposure-latch-') as directory:
    # Compile the production function, rather than a second implementation of its rule.
    shader = (root / 'retoque/shaders/dlssnr/precompile/dlssnr.hlsl').read_text()
    start = shader.index('float WhitePoint()')
    end = shader.index('\n}', start) + 2
    (Path(directory) / 'white_point.h').write_text(shader[start:end])
    binary = Path(directory) / 'cases'
    common_flags = ['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer']
    subprocess.run(common_flags + ['-I', directory, '-I', str(root / 'retoque'), '-I', str(root / 'OptiScaler'),
                                   str(here / 'cases.cpp'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
    subprocess.run(common_flags + ['-D', 'VK_MODE', '-I', directory, '-I', str(root / 'retoque'),
                                   '-I', str(root / 'OptiScaler'), str(here / 'cases.cpp'), '-o', str(binary)],
                   check=True)
    subprocess.run([str(binary)], check=True)

    diagnostics = (root / 'retoque/shaders/dlssnr/DlssNr_Dx12_Diagnostics.cpp').read_text()
    meter_unit = Path(directory) / 'dx12_meter.cpp'
    has_session, has_begin, has_shutdown = build_dx12_meter_unit(diagnostics, meter_unit)
    meter_binary = Path(directory) / 'dx12_meter'
    extra = [] if has_session else ['-Wno-unused-parameter']
    subprocess.run(common_flags + extra + ['-I', str(root / 'retoque'), str(meter_unit), '-o', str(meter_binary)],
                   check=True)
    subprocess.run([str(meter_binary)], check=True)

    assert has_session and has_begin and has_shutdown, \
        'D3D12 meter readback must gate dispatch, track submission completion and guard shutdown lifetime'
    dx12_core = (root / 'retoque/shaders/dlssnr/DlssNr_Dx12.cpp').read_text()
    dx12_internal = (root / 'retoque/shaders/dlssnr/DlssNr_Dx12_Internal.h').read_text()
    assert 'bool BeginMeterDispatch(ID3D12GraphicsCommandList* cmdList);' in dx12_internal
    assert 'void ShutdownExposureMeter();' in dx12_internal
    assert dx12_core.count('BeginMeterDispatch(cmdList)') == 1
    assert dx12_core.count('ShutdownExposureMeter();') == 1
    shutdown_tail = dx12_core[dx12_core.index('\nvoid Shutdown()\n{'):]
    assert 'for (auto& rb : g_nr.meterReadback)' not in shutdown_tail
    assert 'g_nr.meter->Release();' not in shutdown_tail

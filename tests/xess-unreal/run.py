#!/usr/bin/env python3
"""Execute the production decoder and transaction/request source slices with host fakes."""
from pathlib import Path
import re
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
SOURCE = ROOT / 'OptiScaler/inputs/XeSS_UE_Dx12.cpp'


def function(source, name):
    # Bound the signature search to a single line to avoid preceding functions.
    start = re.search(r'^[^\n]*\b' + re.escape(name) + r'\(', source, re.M).start()
    end = source.index('\n}', start) + 2
    return source[start:end]


def run(source, folder, label, decoder_override=None):
    header = ROOT / 'OptiScaler/inputs/XeSS_UE_Decode.h'
    decoder = (decoder_override if decoder_override is not None else header.read_text()).replace('#pragma once', '')
    hooks = '\n\n'.join(function(source, name) for name in ('InitCopy', 'RequestCopy', 'HookModule'))
    hook_gate = function(source, 'Hook')
    hook_gate = hook_gate[:hook_gate.index('    auto exe =')] + '    ++scanCount;\n}'
    hook_gate += '\n' + re.search(r'^bool XeSSUnreal::Hooked\(\).*$', source, re.M).group(0)
    code = (HERE / 'cases.cpp').read_text().replace('// DECODER', decoder).replace('// PRODUCTION', hooks + '\n' + hook_gate)
    cpp = folder / (label + '.cpp')
    exe = folder / label
    cpp.write_text(code)
    subprocess.run(['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie',
                    str(cpp), '-o', str(exe)], check=True)
    return subprocess.run([str(exe)], capture_output=True, text=True)


if __name__ == '__main__':
    source = SOURCE.read_text()
    with tempfile.TemporaryDirectory(prefix='xess-unreal-') as tmp:
        result = run(source, Path(tmp), 'cases')
        print(result.stdout, end='')
        print(result.stderr, end='')
        if result.returncode:
            raise SystemExit(1)
        mutants = {
            'copy reset rollback': ('        o_CommandListReset = nullptr;', ''),
            'copy barrier rollback': ('        o_ResourceBarrier = nullptr;', ''),
            'copy execute rollback': ('        o_ExecuteCommandLists = nullptr;', ''),
            'inline function rollback': ('            o_FlattenVelocity = nullptr;', ''),
            'inline name rollback': ('            _xessVelocityName = nullptr;', ''),
            'wrapper function rollback': ('        o_AddVelocityFlatteningXeSSPass = nullptr;', ''),
            'view helper rollback': ('        o_GetSecondaryViewRectSize = nullptr;', ''),
            'view offset rollback': ('        _viewRectOffset = 0;', ''),
            'foreign device admitted': ('if (device != _device)', 'if (false && device != _device)'),
            'off switch ignored': ('if (!Config::Instance()->XeSSUnrealDepth.value_or_default())',
                                   'if (false && !Config::Instance()->XeSSUnrealDepth.value_or_default())'),
        }
        for index, (label, (old, new)) in enumerate(mutants.items()):
            assert source.count(old) == 1, label
            result = run(source.replace(old, new), Path(tmp), 'mutant' + str(index))
            assert result.returncode != 0 and 'FAIL line' in result.stderr, label
            print('REJECTED', label)
        decoder = (ROOT / 'OptiScaler/inputs/XeSS_UE_Decode.h').read_text()
        old = 'if (wide && (viewRegisters & (1u << source)))'
        assert decoder.count(old) == 1
        mutant = decoder.replace(old, 'if (!wide) return;\n        if (viewRegisters & (1u << source))')
        result = run(source, Path(tmp), 'decoder-mutant', mutant)
        assert result.returncode != 0 and 'FAIL line' in result.stderr
        print('REJECTED narrow MOV preserves View')
    assert '#include "XeSS_UE_Decode.h"' in source
    assert re.search(r'XeSSUnrealDecode::TrackViewRegisters\(\(const uint8_t\*\) address, viewRegisters,\s*viewRectOffset\)', source)
    config = (ROOT / 'OptiScaler/Config.cpp').read_text()
    assert 'CustomOptional<bool> XeSSUnrealDepth { true };' in (ROOT / 'OptiScaler/Config.h').read_text()
    assert 'XeSSUnrealDepth.set_from_config(readBool("XeSS", "UnrealDepth"))' in config
    assert re.search(r'ini.SetValue\("XeSS", "UnrealDepth",\s*GetBoolValue\(Instance\(\)->XeSSUnrealDepth.value_for_config\(\)\)', config)
    assert 'UnrealDepth=auto' in (ROOT / 'OptiScaler.ini').read_text()
    assert 'UnrealDepth' in (ROOT / 'Config.md').read_text()
    print('PASS config declaration/read/save/INI/documentation wiring')

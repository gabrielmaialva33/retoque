#!/usr/bin/env python3
"""Vulkan NGX ownership, recent native activity and storage formats: host only."""
from pathlib import Path
import re
import struct
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
PRE = ROOT / 'retoque/shaders/dlssnr/precompile'


def storage_formats(path):
    data = path.read_bytes()
    words = struct.unpack('<' + 'I' * (len(data) // 4), data)
    result, caps, i = [], set(), 5
    while i < len(words):
        count, op = words[i] >> 16, words[i] & 65535
        assert count and i + count <= len(words)
        args = words[i+1:i+count]
        if op == 25 and args[6] == 2:  # OpTypeImage, Sampled = 2
            result.append(args[7])
        if op == 17:
            caps.add(args[0])
        i += count
    return result, caps


def main():
    for name in ('DlssNr_Shader_Vk', 'DlssNr_PresentHdr_Shader_Vk'):
        formats, caps = storage_formats(PRE / (name + '.spv'))
        assert formats and set(formats) == {0}, (name, 'storage images must use Unknown', formats)
        assert 56 in caps and 55 not in caps, (name, 'write without format only', caps)
        header = (PRE / (name + '.h')).read_text()
        encoded = bytes(int(x, 16) for x in re.findall(r'0x([0-9a-fA-F]{2})\b', header))
        assert encoded == (PRE / (name + '.spv')).read_bytes()
    with tempfile.TemporaryDirectory(prefix='vk-core-') as temp:
        from integration import run
        run(Path(temp))
        from off_paths import run as run_off
        run_off(Path(temp))
        for case in ('cases', 'runtime'):
            exe = str(Path(temp) / case)
            subprocess.run(['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-fno-pie', '-no-pie', '-pthread',
                            '-I'+str(ROOT/'retoque'), '-I'+str(ROOT/'external/vulkan/include'),
                            str(HERE/(case+'.cpp')), '-o', exe], check=True)
            subprocess.run([exe], check=True)
    print('PASS: storage bytecode, core generations, activity, three capability paths and copied feature-request contracts (host only)')

if __name__ == '__main__':
    main()

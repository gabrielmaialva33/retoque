#!/usr/bin/env python3
"""Frame transport protocol only: no platform API, GPU, pipe or model."""
from pathlib import Path
import os
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
HEADER = ROOT / 'retoque/frame_transport/Protocol.h'
# Keep compiler scratch off tmpfs, and avoid mutating the source for mutants.
ARTIFACTS = HERE / 'artifacts'
ARTIFACTS.mkdir(exist_ok=True)
env = dict(os.environ, TMPDIR=str(ARTIFACTS))
with tempfile.TemporaryDirectory(dir=ARTIFACTS) as directory:
    temp = Path(directory)
    binary = temp / 'cases'
    flags = ['g++', '-std=c++20', '-Wall', '-Wextra', '-Werror', '-g',
             '-I', str(temp), '-I', str(HEADER.parent), '-I', str(HERE / 'stubs'),
             str(HERE / 'cases.cpp'), '-o', str(binary)]
    subprocess.run(flags + ['-fsanitize=address,undefined', '-fno-omit-frame-pointer'], check=True, env=env)
    subprocess.run([binary], check=True, env=env)
    header = HEADER.read_text()
    mutants = [
        ('wrong version', 'if (Detail::Read(bytes, 4, 2) != kVersion)', 'if (false)', 'wrong version'),
        ('truncated packet', 'if (bytes.size() < length)', 'if (false)', 'truncated message'),
        ('oversized packet', 'if (length > kMaxMessage || bytes.size() > kMaxMessage)', 'if (false)',
         'oversized message'),
        ('duplicate return', 'if (slot.state != state)', 'if (false && slot.state != state)', 'double return'),
        ('wrong generation', 'if (id.generation != generation_)', 'if (false)', 'wrong generation return'),
        ('peer death frees a lease', 'slot.state = Ownership::Abandoned;', 'slot.state = reason == Refusal::PeerGone ? Ownership::Free : Ownership::Abandoned;',
         'peer gone retains ownership'),
        ('queue bound removed', 'if (Outstanding() >= agreement_.slots)', 'if (false)', 'queue saturation'),
        ('completion ignored', 'return observed >= expected ? Refusal::None : Refusal::Incomplete;',
         'return observed >= expected ? Refusal::None : Refusal::None;', 'capture not complete'),
        ('device-loss latch removed', 'Abort(Refusal::DeviceLost);', '(void)0;', 'device loss latched'),
        ('negotiation ignores required features', 'if ((common & needed) != needed)',
         'if (false && (common & needed) != needed)', 'missing required capability'),
    ]
    for name, before, after, label in mutants:
        assert header.count(before) == 1, (name, 'mutation anchor drift')
        (temp / HEADER.name).write_text(header.replace(before, after))
        subprocess.run(flags, check=True, env=env)
        result = subprocess.run([binary], capture_output=True, text=True, env=env)
        assert result.returncode == 1 and result.stderr.strip() == f'FAIL: {label}', (name, result)
        print(f'PASS: rejected mutant: {name}: {label}', flush=True)

# This proposal must acquire no shipped caller or build entry.
paths = subprocess.check_output(['git', 'ls-files', '-z'], cwd=ROOT).decode().split('\0')
for relative in paths:
    path = ROOT / relative
    if not relative or relative.startswith(('tests/', 'docs/', 'retoque/frame_transport/')):
        continue
    if path.suffix not in {'.cpp', '.h', '.hpp', '.c', '.cc', '.vcxproj', '.cmake'} and path.name != 'CMakeLists.txt':
        continue
    source = path.read_text(errors='replace')
    assert 'frame_transport/' not in source and 'frame_transport\\' not in source, relative
    assert 'Retoque::FrameTransport' not in source, relative
print('PASS: no shipped source or build references to transport module')

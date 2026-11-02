"""Slice unchanged production method/shader bodies into the bounded host resource sink."""
from pathlib import Path
import re
import subprocess

HERE = Path(__file__).resolve().parent


def extract(source, signature):
    start = source.index(signature)
    mask = re.sub(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"',
                  lambda m: ' ' * len(m[0]), source, flags=re.S)
    opening = mask.index('{', start)
    depth = 1
    for end in range(opening + 1, len(mask)):
        depth += (mask[end] == '{') - (mask[end] == '}')
        if not depth:
            return source[start:end + 1]
    raise ValueError(signature)


def run(root, directory):
    directory.mkdir(parents=True, exist_ok=True)
    shader_dir = root / 'retoque/shaders/dlssnr'
    header = (shader_dir / 'DlssNr_Cadence_Dx12.h').read_text()
    source = (shader_dir / 'DlssNr_Cadence_Dx12.cpp').read_text()
    shader = (shader_dir / 'precompile/dlssnr_cadence.hlsl').read_text()
    # All pixel modes execute verbatim; only texture/command effects are substituted.
    (directory / 'pixel_modes.h').write_text(extract(shader, 'void PixelModes('))
    types = '\n'.join(extract(header, 'struct ' + name) + ';'
                      for name in ('Region', 'Frame', 'Smooth', 'alignas(256) Constants'))
    methods = '\n\n'.join(extract(source, signature) for signature in (
        'DlssNr_Cadence_Dx12::Constants DlssNr_Cadence_Dx12::Base(',
        'bool DlssNr_Cadence_Dx12::Record(', 'bool DlssNr_Cadence_Dx12::Carry(',
        'ID3D12Resource* DlssNr_Cadence_Dx12::ModelMotion(',
        'bool DlssNr_Cadence_Dx12::Fade(', 'void DlssNr_Cadence_Dx12::Probe('))
    generated = (HERE / 'orchestration.cpp').read_text().replace('// PRODUCTION_TYPES', types)
    generated = generated.replace('// PRODUCTION_METHODS', methods)
    (directory / 'orchestration.cpp').write_text(generated)
    binary = directory / 'orchestration'
    subprocess.run(['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    '-I', str(root / 'retoque'), '-I', str(HERE), '-I', str(directory),
                    str(directory / 'orchestration.cpp'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

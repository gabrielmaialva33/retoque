#!/usr/bin/env python3
"""Inspect the embedded production SPIR-V: RGBA16F storage, four-component stores.

--upstream-root is the behavioral RED adapter: it reads actual existing OS shader
arrays and applies the same NR target contract, without requiring a new API/file.
"""
import argparse
from pathlib import Path
import re
import struct

VARIANTS = ('bcus', 'bcds_bicubic', 'bcds_catmull', 'bcds_lanczos2', 'bcds_lanczos3',
            'bcds_kaiser2', 'bcds_kaiser3', 'bcds_magc', 'fsr_easu')
UPSTREAM = ('precompile/bcus_Shader_Vk.h', 'precompile/bcds_bicubic_Shader_Vk.h',
            'precompile/bcds_catmull_Shader_Vk.h', 'precompile/bcds_lanczos2_Shader_Vk.h',
            'precompile/bcds_lanczos3_Shader_Vk.h', 'precompile/bcds_kaiser2_Shader_Vk.h',
            'precompile/bcds_kaiser3_Shader_Vk.h', 'precompile/bcds_magc_Shader_Vk.h',
            'fsr1/FSR_EASU_Shader_Vk.h')


def embedded(path, name):
    text = Path(path).read_text(encoding='utf-8', errors='replace')
    match = re.search(r'\b' + re.escape(name) + r'\s*\[\s*\]\s*=\s*\{([^}]*)\}', text, re.S)
    if not match:
        raise ValueError(f'{path}: missing embedded array {name}')
    values = re.findall(r'0x([0-9a-fA-F]{2})\b', match[1])
    if not values:
        raise ValueError(f'{path}: array has no byte literals')
    return bytes(int(value, 16) for value in values)


def inspect(blob):
    if len(blob) % 4 or len(blob) < 20:
        raise ValueError('SPIR-V byte length is invalid')
    words = struct.unpack('<' + 'I' * (len(blob)//4), blob)
    if words[0] != 0x07230203:
        raise ValueError('SPIR-V magic is invalid')
    instructions = []
    cursor = 5
    while cursor < len(words):
        count, opcode = words[cursor] >> 16, words[cursor] & 0xffff
        if count == 0 or cursor + count > len(words):
            raise ValueError('SPIR-V instruction length is invalid')
        instructions.append((opcode, words[cursor+1:cursor+count]))
        cursor += count
    vectors, pointers, images, variables, values, decorations = {}, {}, {}, {}, {}, {}
    writes, capabilities, entries, local_sizes = [], set(), {}, {}
    # Actual DXC storage accesses use OpLoad, and its texels are SSA vector
    # values. Only result-bearing instructions needed by these compiled shaders
    # are admitted; unknown definitions fail closed instead of guessing operands.
    result_ops = {12, 41, 42, 43, 44, 45, 46, 48, 49, 50, 51, 52, 55, 57, 59, 61,
                  65, 66, 67, 70, 77, 78, 79, 80, 81, 82, 83, 84, 86, 100, 124,
                  126, 127, 128, 129, 130, 131, 132, 133, 134, 135, 136, 137,
                  138, 139, 140, 141, 142, 143, 144, 145, 146, 147, 148, 169, 245}
    for opcode, operands in instructions:
        if opcode == 17:  # OpCapability
            capabilities.add(operands[0])
        elif opcode == 15:  # OpEntryPoint
            name_bytes = struct.pack('<' + 'I' * len(operands[2:]), *operands[2:])
            entries[operands[1]] = (operands[0], name_bytes.split(b'\0', 1)[0].decode('ascii'))
        elif opcode == 16 and operands[1] == 17:  # OpExecutionMode LocalSize
            local_sizes[operands[0]] = list(operands[2:5])
        elif opcode == 23:  # OpTypeVector
            vectors[operands[0]] = operands[2]
        elif opcode == 25:  # OpTypeImage
            images[operands[0]] = {'sampled': operands[6], 'format': operands[7]}
        elif opcode == 32:  # OpTypePointer
            pointers[operands[0]] = operands[2]
        elif opcode == 59:  # OpVariable
            variables[operands[1]] = operands[0]
        elif opcode == 71 and len(operands) >= 3:  # OpDecorate
            decorations.setdefault(operands[0], {})[operands[1]] = operands[2]
        if opcode in result_ops and len(operands) >= 2:
            values[operands[1]] = (operands[0], opcode, operands[2:])
        if opcode == 99:  # OpImageWrite
            writes.append(operands)
    compute = [entry for entry, value in entries.items() if value == (5, 'CSMain')]
    if len(compute) != 1 or len(entries) != 1 or compute[0] not in local_sizes:
        raise ValueError('expected one CSMain compute entry with literal LocalSize')
    if 56 in capabilities:  # StorageImageWriteWithoutFormat
        raise ValueError('typed storage output must not require StorageImageWriteWithoutFormat')
    storage = {key: value for key, value in images.items() if value['sampled'] == 2}
    if not storage:
        raise ValueError('no storage image type')
    if any(value['format'] != 2 for value in storage.values()):  # ImageFormat Rgba16f = 2
        raise ValueError(f'storage format must be Rgba16f (2), got {[v["format"] for v in storage.values()]}')
    destinations = {var for var, pointer in variables.items()
                    if pointers.get(pointer) in storage}
    if not destinations or any(decorations.get(var, {}).get(33) != 2 or
                               decorations.get(var, {}).get(34) != 0 for var in destinations):
        raise ValueError('storage image is not descriptor set 0 binding 2')
    if not writes:
        raise ValueError('shader has no OpImageWrite')
    widths = []
    for operands in writes:
        image, _, texel = operands[:3]
        image_def = values.get(image)
        if not image_def or image_def[0] not in storage:
            raise ValueError('image write does not use the declared storage type')
        texel_def = values.get(texel)
        width = vectors.get(texel_def[0]) if texel_def else None
        widths.append(width)
        if width != 4:
            raise ValueError(f'OpImageWrite texel must have four components, got {width}')
    return {'storage_format': 'Rgba16f', 'descriptor': [0, 2], 'write_components': widths, 'local_size': local_sizes[compute[0]]}


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--header', type=Path, default=Path(__file__).resolve().parents[2]/
                        'retoque/shaders/dlssnr/precompile/DlssNr_Scaler_Shaders_Vk.h')
    parser.add_argument('--upstream-root', type=Path)
    parser.add_argument('--spv-dir', type=Path, help='also require each embedded array to equal <variant>.spv')
    args = parser.parse_args(argv)
    failed = False
    for index, variant in enumerate(VARIANTS):
        path = args.header
        symbol = 'nr_' + variant + '_spv'
        if args.upstream_root:
            path = args.upstream_root/'OptiScaler/shaders/output_scaling'/UPSTREAM[index]
            symbol = ('FSR_EASU' if variant == 'fsr_easu' else variant) + '_spv'
        try:
            blob = embedded(path, symbol)
            if args.spv_dir and blob != (args.spv_dir/(variant + '.spv')).read_bytes():
                raise ValueError('embedded array differs from compiled .spv bytes')
            result = inspect(blob)
            expected_size = [64, 1, 1] if variant == 'fsr_easu' else [16, 16, 1] if variant == 'bcus' else [8, 8, 1]
            if result['local_size'] != expected_size:
                raise ValueError(f'LocalSize changed: {result["local_size"]} != {expected_size}')
            print(f'PASS {variant}: {result}')
        except (OSError, ValueError, IndexError) as error:
            failed = True
            print(f'FAIL {variant}: {error}')
    return int(failed)


if __name__ == '__main__':
    raise SystemExit(main())

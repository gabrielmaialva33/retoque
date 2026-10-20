#!/usr/bin/env python3
"""Actual internal declarations, original owner definitions, two independent consumer TUs.

No D3D12 semantics: the verbatim header is compiled with platform/domain include stubs.
The DLL build is the real-header/MSVC gate; this host test specifically tests storage/linkage.
"""
from pathlib import Path
import hashlib
import json
import re
import os
import subprocess
import tempfile
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / '_support'))
from nr_sources import renderer_internal_header, renderer_source

HERE = Path(__file__).resolve().parent
header = renderer_internal_header()
source = renderer_source()
contract = json.loads((HERE / 'state-contract.json').read_text())
assert '#include "pch.h"' not in header
assert 'using namespace' not in header
assert 'namespace DlssNr::Dx12Internal\n{' in source
assert 'namespace DlssNr::Dx12Internal\n{' in header
names = re.findall(r'^extern .*?\b(g_\w+)', header, re.M)
assert names == contract['names'], 'shared state declarations changed/order drifted'
assert len(names) == len(set(names))
# Check actual definitions in original order, with original initializers. Skip function bodies.
definitions = []
seen = []
for match in re.finditer(r"^[A-Za-z_][^\n]*?\bg_\w+[\s\S]*?;", source, re.M):
    line = match[0]
    if '(' in line.split('g_')[0]:
        continue
    found = [n for n in re.findall(r'\bg_\w+', line) if n in names]
    if not found:
        continue
    assert not line.startswith(('static ', 'inline ', 'extern ')), line
    line = line.split(';')[0] + ';'
    definitions.append(line.replace('Dx12Internal::', ''))
    seen.extend(found)
assert seen == names, f'missing/duplicate/reordered owner definition: {set(names)-set(seen)}, {set(seen)-set(names)}; count {len(seen)} / {len(names)}'
normalize = lambda s: re.sub(r'\s+', '', s)
digest = hashlib.sha256(normalize('\n'.join(definitions)).encode()).hexdigest()
assert digest == contract['initializers_sha256'], 'shared initializers changed from pre-refactor'

with tempfile.TemporaryDirectory(prefix='nr-dx12-internal-') as tmp:
    out = Path(tmp)
    # A verbatim copy keeps quoted project includes from finding real Windows headers on this host.
    # Standard library includes remain real. No state/type text is rewritten for this link test.
    (out / 'DlssNr_Dx12_Internal.h').write_text(header)
    (out / 'platform.h').write_text((HERE / 'platform.h').read_text())
    for include in re.findall(r'^#include [<"]([^>"]+)[>"]', header, re.M):
        if include.endswith('.h'):
            p = out / include
            p.parent.mkdir(parents=True, exist_ok=True)
            p.write_text('#pragma once\n')
    preamble = '#include "platform.h"\n#include "DlssNr_Dx12_Internal.h"\n'
    (out / 'owner.cpp').write_text(preamble + 'namespace DlssNr::Dx12Internal {\n' + '\n'.join(definitions) + '\n}\n')
    address = ',\n'.join('&' + name for name in names)
    for letter in ('a', 'b'):
        (out / f'{letter}.cpp').write_text(preamble + f'''
using namespace DlssNr::Dx12Internal;
const void* address_{letter}(unsigned i) {{
    static const void* values[] = {{ {address} }};
    return values[i];
}}
void mutate_{letter}() {{
    std::lock_guard<std::mutex> lock(g_nrMutex);
    ++g_cadenceEvaluations;
    ++g_nrScratchGeneration;
    g_nr.width += 7;
    g_pre.width += 11;
    g_presentFlip += 13;
    g_sourceIsPresent = true;
}}
''')
    (out / 'main.cpp').write_text(preamble + f'''
#include <cassert>
using namespace DlssNr::Dx12Internal;
const void* address_a(unsigned); const void* address_b(unsigned);
void mutate_a(); void mutate_b();
int main() {{
    const void* expected[] = {{ {address} }};
    for (unsigned i = 0; i < {len(names)}; ++i) {{
        assert(address_a(i) == expected[i]);
        assert(address_b(i) == expected[i]);
    }}
    const auto evals = g_cadenceEvaluations, generation = g_nrScratchGeneration;
    mutate_a(); mutate_b();
    assert(g_cadenceEvaluations == evals + 2 && g_nrScratchGeneration == generation + 2);
    assert(g_nr.width == 14 && g_pre.width == 22 && g_presentFlip == 26);
    assert(g_sourceIsPresent.load());
}}
''')
    flags = ['g++', '-std=c++20', '-O1', '-Wall', '-Wextra', '-Werror', '-pthread', '-I' + tmp]
    objects = []
    for name in ('owner', 'a', 'b', 'main'):
        obj = out / f'{name}.o'
        subprocess.run(flags + ['-c', str(out / f'{name}.cpp'), '-o', str(obj)], check=True)
        objects.append(str(obj))
    exe = out / 'identity'
    subprocess.run(flags + objects + ['-o', str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
    # The consumers must not silently own storage: removing the one owner must fail to link.
    # The linker's diagnostics are localized; the negative controls read them in the C locale.
    c_locale = dict(os.environ, LC_ALL='C', LANG='C')
    negative = subprocess.run(flags + objects[1:] + ['-o', str(out / 'missing-owner')], capture_output=True,
                              env=c_locale)
    assert negative.returncode != 0 and b'undefined reference' in negative.stderr
    # A second owner must likewise be diagnosed, not hidden by internal/inline linkage.
    negative = subprocess.run(flags + objects + [objects[0], '-o', str(out / 'duplicate-owner')], capture_output=True,
                              env=c_locale)
    assert negative.returncode != 0 and b'multiple definition' in negative.stderr
# Functions moved out of the main unit: one definition each, in the unit that owns it (function-owners.json).
from nr_sources import renderer_paths  # noqa: E402
units = {p.name: p.read_text(encoding='utf-8-sig') for p in renderer_paths()}
owners = json.loads((HERE / 'function-owners.json').read_text())['owners']
moved = 0
for unit, signatures in owners.items():
    assert unit in units, f'{unit} is listed as an owner but is not a renderer unit'
    for signature in signatures:
        homes = [name for name, text in units.items() if signature in text]
        assert homes == [unit] and units[unit].count(signature) == 1, f'{signature!r} defined in {homes}, not once in {unit}'
        moved += 1
print(f'PASS: {len(names)} shared objects, separate-TU identity/mutations, missing/duplicate owner controls; '
      f'{moved} moved functions defined once in their owning unit')

# Resource extraction contract: same helper definitions, allowing clang-format whitespace only.
resource_contract = json.loads((HERE / 'resource-contract.json').read_text())
resource_source = units['DlssNr_Dx12_Resources.cpp']
for name, expected in resource_contract['definitions'].items():
    match = re.search(r'^([\w:*]+ ' + name + r'\([^;{]*\))\s*\{', resource_source, re.M)
    assert match, f'missing resource definition: {name}'
    start = resource_source.index('{', match.start())
    level, end = 1, start + 1
    while level:
        level += (resource_source[end] == '{') - (resource_source[end] == '}')
        end += 1
    actual = hashlib.sha256(normalize(resource_source[match.start():end]).encode()).hexdigest()
    assert actual == expected, f'{name} changed from {resource_contract["base"]}'
assert 'uint64_t g_nrScratchGeneration = 1;' in units['DlssNr_Dx12.cpp']
assert 'bool DescriptorReuseAvailable()' in units['DlssNr_Dx12.cpp']
print('PASS: resource helper bodies preserved; scratch generation/descriptor reuse remain in core')

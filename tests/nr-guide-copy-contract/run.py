#!/usr/bin/env python3
"""Compile actual guide creation/reuse; observe format/plane/device and retirement."""
from pathlib import Path
import subprocess
import tempfile
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
source = (ROOT / "retoque/shaders/dlssnr/DlssNr_Dx12_Resources.cpp").read_text()
def function(signature):
    start = source.index(signature)
    brace = source.index('{', start)
    depth = 0
    for position in range(brace, len(source)):
        depth += (source[position] == '{') - (source[position] == '}')
        if depth == 0:
            return source[start:position+1]+'\n'
    raise AssertionError('unterminated production function: '+signature)

unit = (HERE / "fakes.h").read_text()
# The resource overload (2026-10-30, a resource under a wrapper layer) is taken when present, so a source without it
# fails the wrapper cases on behaviour rather than on a missing anchor.
RESOURCE_RULE = 'bool DeviceChildBelongsTo(ID3D12Device* device, ID3D12Resource* resource)'
for signature in ('DXGI_FORMAT TypedGuideFormat(', 'bool IsTypeless(',
                  'bool DeviceChildBelongsTo(ID3D12Device* device, ID3D12DeviceChild* child)',
                  *((RESOURCE_RULE,) if RESOURCE_RULE in source else ()),
                  'DXGI_FORMAT GuideCopyFamily(', 'bool GuideCopyCompatible(',
                  'ID3D12Resource* CreateGuideClone(', 'ID3D12Resource* ReadableGuide('):
    unit += function(signature)
unit += (HERE / "cases.cpp").read_text()
with tempfile.TemporaryDirectory(prefix="retoque-guide-copy-contract-") as folder:
    cpp, binary = Path(folder) / "cases.cpp", Path(folder) / "cases"
    cpp.write_text(unit)
    subprocess.run(["g++", "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-I" + str(ROOT), str(cpp), "-o", str(binary)],
                   check=True)
    cases = ('shape', 'format-change', 'fallback-family-r24', 'fallback-family-r32g8',
             'legal-fallback', 'plane-query-failure', 'plane-mismatch',
             'same-family-two-planes', 'device-change', 'mixed-device', 'wrapped-device', 'allocation-and-passthrough')
    failures = []
    for case in cases:
        result = subprocess.run([str(binary), case])
        if result.returncode: failures.append(case)
    if failures:
        print('FAIL:', ', '.join(failures), file=sys.stderr)
        raise SystemExit(1)


subprocess.run([sys.executable, str(HERE / "owner-run.py")], check=True)

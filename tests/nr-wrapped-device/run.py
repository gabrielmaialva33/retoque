#!/usr/bin/env python3
"""NR's D3D12 device follows the command list (or queue), not a resource; resources may sit under a wrapper.

The rule (DlssNr_DeviceMatch.h) and the production DeviceChildBelongsTo pair are compiled against wrapper-modelling
fakes under ASan/UBSan; every route's device source is read off the source, with mutants that must fail. The routes
themselves run against host fakes in nr-before-upscale and nr-guide-copy-contract. No D3D12, ReShade or game.
"""
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / '_support'))
from nr_sources import renderer_source  # noqa: E402

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
resources = (ROOT / 'retoque/shaders/dlssnr/DlssNr_Dx12_Resources.cpp').read_text(encoding='utf-8-sig')
internal = (ROOT / 'retoque/shaders/dlssnr/DlssNr_Dx12_Internal.h').read_text(encoding='utf-8-sig')

STRICT = 'bool DeviceChildBelongsTo(ID3D12Device* device, ID3D12DeviceChild* child)\n{'
RESOURCE = 'bool DeviceChildBelongsTo(ID3D12Device* device, ID3D12Resource* resource)\n{'


def body(text, signature):
    assert text.count(signature) == 1, f'expected one definition: {signature!r}'
    start = text.index(signature)
    brace = text.index('{', start)
    depth = 0
    for i in range(brace, len(text)):
        depth += (text[i] == '{') - (text[i] == '}')
        if depth == 0:
            return text[start:i + 1]
    raise AssertionError('unterminated: ' + signature)


with tempfile.TemporaryDirectory(prefix='retoque-nr-wrapped-device-') as folder:
    folder = Path(folder)
    (folder / 'artifacts').mkdir()
    pair = body(resources, STRICT) + '\n'
    if RESOURCE in resources:
        pair += body(resources, RESOURCE) + '\n'
    (folder / 'artifacts/belongs.inc').write_text(pair)
    binary = folder / 'cases'
    subprocess.run(['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer', '-I' + str(ROOT), '-I' + str(folder), str(HERE / 'cases.cpp'), '-o',
                    str(binary)], check=True)
    subprocess.run([str(binary)], check=True)


def check(renderer, resources, internal):
    # No route takes its device from a resource any more.
    for gone in ('target->GetDevice(', 'colour->GetDevice(', 'backbuffer->GetDevice('):
        assert gone not in renderer, f'a route still takes its device from a resource: {gone}'

    dispatch = body(renderer, 'bool DlssNr_Dx12::Dispatch(')
    assert 'cmdList->GetDevice(IID_PPV_ARGS(&timingDevice));' in dispatch
    assert dispatch.count('cmdList->GetDevice(IID_PPV_ARGS(&device))') == 1
    # The prefix holds the list to the pass's own device before anything is recorded.
    assert dispatch.index('!DeviceChildBelongsTo(_device, cmdList)') < dispatch.index('cmdList->GetDevice(')

    entries = {
        'void EvaluateAfterUpscale(': 'DeviceChildBelongsTo(device, target)',
        'bool EvaluateAtPresent(': 'DeviceChildBelongsTo(device, colour)',
        'ScopedPreUpscale::ScopedPreUpscale(': 'DeviceChildBelongsTo(device, colour)',
    }
    for signature, resource_check in entries.items():
        entry = body(renderer, signature)
        taken = entry.index('cmdList->GetDevice(IID_PPV_ARGS(&device))')
        assert entry.count('cmdList->GetDevice(IID_PPV_ARGS(&device))') == 1, signature
        assert 'DeviceChildBelongsTo(device, cmdList)' not in entry, f'{signature}: the list is the device'
        assert taken < entry.index(resource_check) < entry.index('EnsureCompose(device)'), signature

    present = body(renderer, 'void RunPresentPass(')
    taken = present.index('queue->GetDevice(IID_PPV_ARGS(&device))')
    assert taken < present.index('DeviceChildBelongsTo(device, backbuffer)') < present.index('EnsureCompose(device)')
    assert taken < present.index('GetPresentCommandList(device, slot)'), 'the list is made on the queue\'s device'
    assert 'DeviceChildBelongsTo(device, queue)' not in present

    # Lists and queues stay strict; only a resource may report the device under a wrapper.
    strict = body(resources, STRICT)
    assert 'owner == device' in strict and 'DeviceMatch' not in strict
    relaxed = body(resources, RESOURCE)
    assert 'DlssNr::DeviceMatch::Relate(device, owner)' in relaxed
    assert '#include <dlssnr/DlssNr_DeviceMatch.h>' in resources
    for declaration in ('bool DeviceChildBelongsTo(ID3D12Device* device, ID3D12DeviceChild* child);',
                        'bool DeviceChildBelongsTo(ID3D12Device* device, ID3D12Resource* resource);'):
        assert internal.count(declaration) == 1, declaration
    return True


renderer = renderer_source()
check(renderer, resources, internal)

mutants = {
    'output texture again': (renderer.replace('cmdList->GetDevice(IID_PPV_ARGS(&device))', 'target->GetDevice(IID_PPV_ARGS(&device))', 1), resources, internal),
    'timing from the output': (renderer.replace('cmdList->GetDevice(IID_PPV_ARGS(&timingDevice));', 'target->GetDevice(IID_PPV_ARGS(&timingDevice));', 1), resources, internal),
    'present from the backbuffer': (renderer.replace('queue->GetDevice(IID_PPV_ARGS(&device))', 'backbuffer->GetDevice(IID_PPV_ARGS(&device))', 1), resources, internal),
    'target unchecked': (renderer.replace('DeviceChildBelongsTo(device, target)', 'true', 1), resources, internal),
    'list relaxed': (renderer, resources.replace('const bool matches = SUCCEEDED(hr) && owner == device;', 'const bool matches = SUCCEEDED(hr) && DlssNr::DeviceMatch::Relate(device, owner) != DlssNr::DeviceMatch::Relation::Foreign;', 1), internal),
    'resource strict': (renderer, resources.replace('DlssNr::DeviceMatch::Relate(device, owner)', 'owner == device', 1), internal),
    'overload undeclared': (renderer, resources, internal.replace('bool DeviceChildBelongsTo(ID3D12Device* device, ID3D12Resource* resource);', '', 1)),
}
for name, (r, s, h) in mutants.items():
    assert (r, s, h) != (renderer, resources, internal), f'mutant {name!r} did not apply'
    try:
        check(r, s, h)
    except (AssertionError, ValueError):
        continue
    raise AssertionError(f'mutant {name!r} escaped the source guard')
print(f'PASS: every D3D12 route takes its device from its list or queue; {len(mutants)} mutants rejected')

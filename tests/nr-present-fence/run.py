#!/usr/bin/env python3
"""Host checks for the D3D12 present lists when their fence cannot prove completion (DlssNr_FenceProgress.h).

1. The pure header and the verbatim production helpers (the removal latch through WaitPresentListsIdle,
   RetirePresentHdrTexture, CollectPresentHdrRetired) against fake D3D12 objects under ASan/UBSan, one scenario per
   process.
2. Mutation controls: each rule taken out of the helpers or the header must fail a scenario.
3. Source guards: one reader of the present fence, PresentSync's wait through it, the header in the project files.

No D3D12 runtime, no GPU.
"""
from pathlib import Path
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / '_support'))
from nr_sources import renderer_source  # noqa: E402  every DlssNr_Dx12*.cpp unit, in a fixed order

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def body(text: str, signature: str) -> str:
    assert text.count(signature) == 1, f'expected one definition: {signature!r}'
    start = text.index(signature)
    brace = text.index('{', start)
    depth = 0
    for i in range(brace, len(text)):
        depth += (text[i] == '{') - (text[i] == '}')
        if depth == 0:
            return text[start:i + 1]
    raise AssertionError('unterminated: ' + signature)


def between(text: str, start: str, stop: str) -> str:
    assert text.count(start) == 1 and text.count(stop) == 1, (start, stop)
    assert text.index(start) < text.index(stop)
    return text[text.index(start):text.index(stop)]


internal = (ROOT / 'retoque/shaders/dlssnr/DlssNr_Dx12_Internal.h').read_text(encoding='utf-8-sig')
dx12 = renderer_source()
header = (ROOT / 'retoque/dlssnr/DlssNr_FenceProgress.h').read_text(encoding='utf-8-sig')

types = body(internal, 'struct PresentList\n') + ';\n' + body(internal, 'struct PresentHdrRetired\n') + ';\n'
# One contiguous region, so whatever the helpers share (the removal latch, the fence reader) comes with them.
helpers = between(dx12, '// The device the present lists were made on was removed.',
                  '// The present route\'s synthesized motion is owed')
retire = body(dx12, 'static void RetirePresentHdrTexture(ID3D12Resource*& resource)')
collect = body(dx12, 'static void CollectPresentHdrRetired()')


def build(folder: Path, helpers_text: str, collect_text: str, header_text: str | None = None) -> Path:
    """The scenarios over one version of the helpers; header_text replaces DlssNr_FenceProgress.h when given."""
    artifacts = folder / 'artifacts'
    artifacts.mkdir(parents=True)
    (artifacts / 'present_types.inc').write_text(types, encoding='utf-8')
    (artifacts / 'present_list_slice.inc').write_text(helpers_text + '\n' + retire + '\n' + collect_text + '\n',
                                                      encoding='utf-8')
    includes = ['-I' + str(folder)]
    if header_text is not None:
        (folder / 'dlssnr').mkdir()
        (folder / 'dlssnr/DlssNr_FenceProgress.h').write_text(header_text, encoding='utf-8')
        includes.append('-DNR_PRESENT_FENCE_MUTANT')  # the header's static_asserts off: a scenario has to notice
    binary = folder / 'cases'
    subprocess.run(
        ['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
         '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
         *includes, '-I' + str(ROOT / 'retoque'), str(HERE / 'cases.cpp'), '-o', str(binary)],
        check=True,
    )
    return binary


def failures(binary: Path) -> list[str]:
    names = subprocess.run([str(binary), '--list'], check=True, capture_output=True, text=True).stdout.split()
    assert len(names) >= 16, names
    return [name for name in names
            if subprocess.run([str(binary), name], capture_output=True, text=True).returncode != 0]


def mutate(text: str, old: str, new: str) -> str:
    assert text.count(old) == 1, f'mutation anchor is not unique: {old!r}'
    return text.replace(old, new)


# Each takes one rule out. (name, helpers, collect, header)
MUTANTS = (
    ('UINT64_MAX read as a value', helpers, collect,
     mutate(header, '    if (completedValue == UINT64_MAX)\n        return State::Removed;\n\n', '')),
    ('an allocator reset on a removed device',
     mutate(helpers, '        if (progress == DlssNr::FenceProgress::State::Removed)\n            return nullptr;\n', ''),
     collect, None),
    ('the allocator wait taken as the answer',
     mutate(helpers, '            progress = PresentFenceProgress(g_presentList.fenceValue[slot]);\n', ''), collect, None),
    ('the idle wait taken as the answer',
     mutate(helpers, ' &&\n           PresentFenceProgress(g_presentFenceValue) == '
                     'DlssNr::FenceProgress::State::Completed;', ';'), collect, None),
    ('a refused signal ignored',
     mutate(helpers, '        if (FAILED(signalled))\n', '        if (false && FAILED(signalled))\n'), collect, None),
    ('a refused signal read as a list that never ran',
     mutate(helpers, '            NotePresentSignalFailed(signalled, slot);\n        executed = true;',
            '            NotePresentSignalFailed(signalled, slot);\n        executed = SUCCEEDED(signalled);'),
     collect, None),
    ('a removal not latched',
     mutate(helpers, '    if (s_presentDeviceRemoved.load(std::memory_order_relaxed))\n        return UINT64_MAX;\n\n',
            ''), collect, None),
    ('retired HDR textures released on a raw comparison', helpers,
     mutate(collect, 'DlssNr::FenceProgress::Completed(completed, g_hdrRetired[i].fenceValue)',
            'g_hdrRetired[i].fenceValue <= completed'), None),
)

with tempfile.TemporaryDirectory(prefix='retoque-nr-present-fence-') as folder:
    failed = failures(build(Path(folder) / 'production', helpers, collect))
    if failed:
        print('FAIL: ' + ', '.join(failed))
        sys.exit(1)

    for index, (name, mutant_helpers, mutant_collect, mutant_header) in enumerate(MUTANTS):
        if not failures(build(Path(folder) / f'mutant-{index}', mutant_helpers, mutant_collect, mutant_header)):
            print(f'FAIL: no scenario notices {name}')
            sys.exit(1)

# 3. Source guards. RunPresentPass is not executed by any host fixture.
read_fence = body(dx12, 'static UINT64 ReadPresentFence()')
assert dx12.count('g_presentList.fence->GetCompletedValue()') == 1, 'one reader of the present fence'
assert 'g_presentList.fence->GetCompletedValue()' in read_fence
present = between(dx12, 'void RunPresentPass(', '// ReportSpatialContract, GatherGuideFrame')
assert ('if (presentSync && g_presentList.fenceValue[slot] != 0 &&\n'
        '        PresentFenceProgress(g_presentList.fenceValue[slot]) == DlssNr::FenceProgress::State::Pending)'
        in present), 'PresentSync\'s wait decides through the same look'
# The latch belongs to the objects it describes: gone with them, so a later set starts unproven, not refused.
reset = body(dx12, 'static void ResetPresentList()')
assert reset.rstrip().endswith('s_presentDeviceRemoved = false;\n}')
assert dx12.count('s_presentDeviceRemoved = false;') == 1 and dx12.count('s_presentDeviceRemoved.exchange(true)') == 1

vcxproj = (ROOT / 'OptiScaler/OptiScaler.vcxproj').read_text(encoding='utf-8-sig')
filters = (ROOT / 'OptiScaler/OptiScaler.vcxproj.filters').read_text(encoding='utf-8-sig')
entry = '<ClInclude Include="..\\retoque\\dlssnr\\DlssNr_FenceProgress.h"'
assert entry in vcxproj, 'OptiScaler.vcxproj does not list DlssNr_FenceProgress.h'
assert entry in filters, 'OptiScaler.vcxproj.filters does not list DlssNr_FenceProgress.h'

print(f'PASS: DlssNr_FenceProgress and the verbatim present-list helpers: a removed device is never completion, '
      f'{len(MUTANTS)} mutants rejected, one reader of the present fence')
sys.exit(0)

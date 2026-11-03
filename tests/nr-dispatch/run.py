#!/usr/bin/env python3
"""Exercise the production composition constructor, dispatch and cleanup with host fakes."""
from pathlib import Path
import re
import subprocess
import tempfile
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / '_support'))
from nr_sources import renderer_source  # noqa: E402  every DlssNr_Dx12*.cpp unit, in a fixed order

here = Path(__file__).resolve().parent
root = here.parents[1]
renderer = renderer_source()
header = (root / 'retoque/shaders/dlssnr/DlssNr_Dx12.h').read_text()


def between(source, start, end):
    assert source.count(start) == 1, f'production boundary changed: {start}'
    begin = source.index(start)
    return source[begin:source.index(end, begin)]


# The descriptor keys read a generation that lives above the constructor, so it comes along.
generation = between(renderer, 'uint64_t g_nrScratchGeneration', '// A change has to hold still')

declaration = header[header.index('#define DLSSNR_NUM_OF_HEAPS'):]
implementation = between(renderer, 'DlssNr_Dx12::DlssNr_Dx12(', 'bool DlssNr_Dx12::Dispatch(')
# The Streamline stand-down helper (DlssNr_SlNativeNr.h) sits just before Dispatch; it is not what this suite tests
# (tests/nr-sl-native-nr covers it), so its definition is cut out of the slice.
_stand = implementation.find('static const char* GameNrStandDown()')
if _stand >= 0:
    implementation = implementation[:_stand] + implementation[implementation.index('\n}\n', _stand) + 3:]


def guard_descriptor_key(source):
    """The view arguments a descriptor is built from must also be the ones its key carries.

    The cache is only sound while those two agree, and nothing in the type system says they do: a
    parameter added to one and not the other compiles and silently serves a view built under the other
    policy. That is not hypothetical -- translateTypeless became a parameter in 97c94b05, after the
    first version of this cache was written. So the names are pinned here. A new argument at either
    call site fails this until it is also in the key.
    """
    policy = ('kViewFormat', 'kViewMipLevel', 'kViewTranslateTypeless')
    for call, expected in (('CreateShaderResourceView(_device,', ('kViewFormat', 'kViewTranslateTypeless')),
                           ('CreateUnorderedAccessView(_device,', ('kViewMipLevel', 'kViewTranslateTypeless'))):
        assert source.count(call) == 1, f'view creation call moved: {call}'
        arguments = source[source.index(call):]
        arguments = arguments[:arguments.index(';')]
        passed = tuple(name for name in policy if name in arguments)
        assert passed == expected, f'{call} passes {passed}, expected {expected}'

    keys = source.count('const BindKey key {')
    assert keys == 2, f'expected one key per view kind, found {keys}'
    for block in source.split('const BindKey key {')[1:]:
        block = block[:block.index('};')]
        missing = [name for name in policy if name not in block]
        assert not missing, f'BindKey does not carry {missing}; a view argument is outside its key'


def guard_key_members(source):
    """Every member of BindKey must appear in its comparison.

    A member that is in the struct but not in operator== is invisible to every behavioural test that
    can be written today, because the arguments it stands for are constants: drop translateTypeless
    from the comparison and nothing observable changes until the day someone varies it, which is the
    day the cache starts serving a view built under the other policy. So the struct is checked against
    its own comparison directly.
    """
    start = source.index('struct BindKey')
    body = source[start:source.index('\n    };', start)]
    comparison = body[body.index('bool operator=='):]
    members = re.findall(r'^        [A-Za-z_][\w:*<> ]*?\*? ?(\w+) = ', body, re.M)
    assert len(members) >= 8, f'BindKey members not found as expected: {members}'
    missing = [name for name in members if f'{name} == o.{name}' not in comparison]
    assert not missing, f'BindKey::operator== ignores {missing}; those keys cannot retire a descriptor'


def guard_original_in_target(shader, dispatch):
    """The keepless permutation (docs/dlssnr/design/original-in-target.md) is only sound under its gate.

    Its encode writes no keep and its resolve reads the frame from the target, so every reader of hdrCopy must
    either be excluded by the gate decided before the encode or skip its frame. No host fixture runs Dispatch end
    to end; these are source-order guards, each proven by a mutant below.
    """
    assert shader.count('#ifndef NR_ORIGINAL_IN_TARGET\n        gKeep[id.xy] = source;\n#endif') == 1
    assert shader.count('gKeep[id.xy]') == 1
    assert '#define NR_ORIGINAL_AT(p) gTarget[p]' in shader
    assert '#define NR_ORIGINAL_AT(p) gOriginal.Load(int3(p, 0))' in shader
    assert shader.count('NR_ORIGINAL_AT(id.xy)') == 2 and shader.count('gOriginal.Load(') == 1  # the macro's
    assert shader.count('gOriginal.SampleLevel(') == 1  # side by side only, which the host never pairs with it
    # The whole decision, whitespace-normalised: a term's presence is not enough, its polarity and the && joining
    # it to the rest are what make the gate safe (an inverted sourceIsTarget would compose over a stale scratch).
    gate = dispatch[dispatch.index('bool originalInTarget ='):dispatch.index('if (sourceIsTarget)\n        Barrier')]
    decision = ' '.join(gate[:gate.index('static bool s_originalInTargetSaid')].split())
    expected = ('bool originalInTarget = sourceIsTarget && cfg.DlssNrCompare.value_or_default() == 0 && '
                '!(cfg.DlssNrStabilizerStrength.value_or_default() > 0.0f) && !g_capture.isActive() && '
                '!DlssNr::PairCapture::Wanted() && !cfg.DlssNrPulseMetric.value_or_default() && '
                '!DlssNr::ExposureScan::MeterWanted() && OriginalInTargetUsable(desc.Format); '
                'if (originalInTarget) { std::lock_guard<std::mutex> lock(g_detailMeasure.mutex); '
                'originalInTarget = !g_detailMeasure.wanted; }')
    assert decision == expected, f'keepless gate changed: {decision}'
    calls = [m for m in re.finditer(r'DispatchPass\(cmdList, (encodeParams|resolveParams),[^;]*;', dispatch)]
    assert len(calls) == 2 and all(m.group(0).rstrip(');').rstrip().endswith('originalInTarget') for m in calls)
    assert dispatch.index('bool originalInTarget =') < calls[0].start()
    assert 'if (originalInTarget)\n            resolveParams.CompareMode = 0;' in dispatch
    for reader in ('if (stabilizerStrength > 0.0f && !beforeUpscale && wrote && !originalInTarget)',
                   'if (g_capture.isActive() && !originalInTarget)',
                   'if (wrote && !originalInTarget && DlssNr::PairCapture::Wanted())',
                   'if (g_detailMeasure.wanted && wrote && !originalInTarget && DlssNr_DetailStats_Dx12::Available())',
                   'g_nr.hdrCopy, wrote && !originalInTarget, width, height'):
        assert reader in dispatch, f'a reader of the keep is not guarded: {reader}'
    assert dispatch.count('g_nr.hdrCopy') == 13, 'a new reader of the keep must be guarded and listed here'


shader_source = (root / 'retoque/shaders/dlssnr/precompile/dlssnr.hlsl').read_text()
dispatch_body = between(renderer, 'bool DlssNr_Dx12::Dispatch(', '\nnamespace DlssNr\n{')
guard_original_in_target(shader_source, dispatch_body)
keepless_mutants = {
    'keep always written': ('shader', lambda t: t.replace('#ifndef NR_ORIGINAL_IN_TARGET\n        gKeep', '#if 1\n        gKeep', 1)),
    'original read beside the macro': ('shader', lambda t: t.replace('NR_ORIGINAL_AT(id.xy)', 'gOriginal.Load(int3(id.xy, 0))', 1)),
    'stabilizer not in gate': ('dispatch', lambda t: t.replace('!(cfg.DlssNrStabilizerStrength.value_or_default() > 0.0f) &&', '', 1)),
    'detail not in gate': ('dispatch', lambda t: t.replace('originalInTarget = !g_detailMeasure.wanted;', ';', 1)),
    'resolve on the default pipeline': ('dispatch', lambda t: t.replace('target, nullptr, originalInTarget)', 'target, nullptr)', 1)),
    'compare kept': ('dispatch', lambda t: t.replace('if (originalInTarget)\n            resolveParams.CompareMode = 0;', '', 1)),
    'capture unguarded': ('dispatch', lambda t: t.replace('g_capture.isActive() && !originalInTarget', 'g_capture.isActive()', 1)),
    'pulse unguarded': ('dispatch', lambda t: t.replace('wrote && !originalInTarget, width', 'wrote, width', 1)),
    'new reader': ('dispatch', lambda t: t.replace('hdrRead.Restore();', 'Use(g_nr.hdrCopy);\n    hdrRead.Restore();', 1)),
    'source not the target': ('dispatch', lambda t: t.replace('bool originalInTarget = sourceIsTarget &&', 'bool originalInTarget = !sourceIsTarget &&', 1)),
    'format bypasses the gate': ('dispatch', lambda t: t.replace('MeterWanted() && OriginalInTargetUsable', 'MeterWanted() || OriginalInTargetUsable', 1)),
    'stabilizer required': ('dispatch', lambda t: re.sub(r'== 0 &&\s+!\(cfg\.DlssNrStabilizerStrength', '== 0 && (cfg.DlssNrStabilizerStrength', t, count=1)),
}
for name, (which, mutate) in keepless_mutants.items():
    texts = {'shader': shader_source, 'dispatch': dispatch_body}
    mutated = mutate(texts[which])
    assert mutated != texts[which], f'mutant did not apply: {name}'
    texts[which] = mutated
    try:
        guard_original_in_target(texts['shader'], texts['dispatch'])
    except (AssertionError, ValueError):
        continue
    raise SystemExit(f'FAIL: keepless guard accepted the mutant: {name}')
print(f'PASS: keepless permutation gated and every keep reader guarded; {len(keepless_mutants)} mutants rejected')
guard_descriptor_key(implementation)
guard_key_members(declaration)

unit = (here / 'fakes.h').read_text() + '\n' + declaration + '\n' + generation + '\n' + implementation
unit += '\n' + (here / 'cases.h').read_text()
with tempfile.TemporaryDirectory(prefix='optiscaler-nr-dispatch-') as directory:
    source = Path(directory) / 'dispatch.cpp'
    binary = Path(directory) / 'dispatch'
    source.write_text(unit)
    subprocess.run(['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                    '-I' + str(root), str(source), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

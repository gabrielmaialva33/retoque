#!/usr/bin/env python3
"""Which buffers the game tags for Streamline: the inventory on synthetic tags, and where it is fed and said.

The inventory header is compiled under ASan/UBSan and broken one rule at a time. The Streamline hooks, the D3D12 status
line and the Vulkan after-upscale route are read off the source, and each guard is broken once. No Streamline, no GPU,
no game.
"""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / '_support'))
from nr_sources import renderer_source  # noqa: E402

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]

INVENTORY = ROOT / 'retoque/dlssnr/DlssNr_SlTagInventory.h'
FLAGS = ['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
         '-fno-omit-frame-pointer', '-pthread']

# Each rule of the inventory, broken one at a time: the cases must catch every one.
INVENTORY_MUTANTS = {
    'notes unarmed': ('        if (!Armed())\n            return;\n        if (!hasResource)',
                      '        if (!hasResource)'),
    'removal is a tag': ('        if (!hasResource)\n        {\n            _removals.fetch_add(1, std::memory_order_relaxed);\n'
                         '            return;\n        }\n',
                         '        if (!hasResource)\n            _removals.fetch_add(1, std::memory_order_relaxed);\n'),
    'other types dropped': ('            _other.fetch_add(1, std::memory_order_relaxed);\n            return;',
                            '            return;'),
    'unknown lifecycle as valid': ('s.lifecycles.fetch_or(1u << 3, std::memory_order_relaxed);',
                                   's.lifecycles.fetch_or(1u << kValidUntilPresent, std::memory_order_relaxed);'),
    'no gap': ('        if (_saidAtMs != 0 && nowMs < _saidAtMs + kMinGapMs)\n            return false;\n', ''),
    'said every time': ('        if (signature == _saidSignature)\n            return false;\n', ''),
    'shape not in the signature': ('            mix(s.shape);\n', ''),
    'wrong valid lifecycle': ('    const bool valid = (lifecycleMask & (1u << kValidUntilPresent)) != 0;',
                              '    const bool valid = (lifecycleMask & (1u << kOnlyValidNow)) != 0;'),
    'mixed said as valid': ('    if (valid && notValid)\n        return AtPresent::Mixed;\n',
                            '    if (valid && notValid && lifecycleMask == 0)\n        return AtPresent::Mixed;\n'),
    'subrect ignored': ('                shape.subrect = true;\n', '                (void) 0;\n'),
    'no extent fallback': ('                shape.width = tag.extent.width;\n                shape.height = tag.extent.height;\n',
                           ''),
    'format spills': ('(uint64_t(s.format > 0x7FFFu ? 0x7FFFu : s.format) << 32)', '(uint64_t(s.format) << 32)'),
    'describe skipped': ('            describe(*tag.resource, shape);\n', '            (void) describe;\n'),
    'buffer as an image': ('                if (shape.buffer)\n                    types += ", buffer";\n', ''),
    'buffer flag not packed': ('(uint64_t(s.buffer ? 1u : 0u) << 49) |', ''),
    'wrong HUD-less type': ('inline constexpr uint32_t kHudLessColor = 2;', 'inline constexpr uint32_t kHudLessColor = 3;'),
}

COMPILE_CAUGHT = {'wrong HUD-less type'}

with tempfile.TemporaryDirectory(prefix='retoque-nr-sl-tag-inventory-') as folder:
    folder = Path(folder)
    binary = folder / 'cases'
    subprocess.run(FLAGS + ['-I' + str(ROOT), str(HERE / 'cases.cpp'), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

    inventory = INVENTORY.read_text()
    for index, (name, (old, new)) in enumerate(INVENTORY_MUTANTS.items()):
        assert inventory.count(old) == 1, f'inventory mutant {name!r} no longer applies'
        root = folder / f'mutant{index}'
        target = root / 'retoque/dlssnr/DlssNr_SlTagInventory.h'
        target.parent.mkdir(parents=True)
        target.write_text(inventory.replace(old, new))
        mutant = root / 'cases'
        built = subprocess.run(FLAGS + ['-I' + str(root), str(HERE / 'cases.cpp'), '-o', str(mutant)],
                               capture_output=True)
        if built.returncode != 0:
            assert name in COMPILE_CAUGHT, f'inventory mutant {name!r} does not compile:\n{built.stderr.decode()}'
            continue
        result = subprocess.run([str(mutant)], capture_output=True)
        assert result.returncode != 0, f'inventory mutant {name!r} escaped the cases'
    print(f'PASS: {len(INVENTORY_MUTANTS)} inventory mutants fail the cases')


def read(relative):
    return (ROOT / relative).read_text(encoding='utf-8-sig').replace('\r\n', '\n')


def body(text, signature):
    assert text.count(signature) == 1, f'expected one definition: {signature}'
    start = text.index(signature)
    brace = text.index('{', start)
    depth = 0
    for i in range(brace, len(text)):
        depth += (text[i] == '{') - (text[i] == '}')
        if depth == 0:
            return text[start:i + 1]
    raise AssertionError('unterminated: ' + signature)


VENDORED = read('external/streamline/sl_core_types.h')

sources = {
    'hooks': read('OptiScaler/hooks/Streamline_Hooks.cpp'),
    'declaration': read('OptiScaler/hooks/Streamline_Hooks.h'),
    'renderer': renderer_source().replace('\r\n', '\n'),
    'vk': read('retoque/dlssnr/DlssNrFeature_Vk.cpp'),
    'header': INVENTORY.read_text(),
    'note': read('docs/dlssnr/design/sl-tag-inventory.md'),
    'index': read('docs/dlssnr/README.md'),
}


def check(s):
    header = s['header']
    assert 'LOG_' not in header and '#include <Config.h>' not in header and 'windows.h' not in header, \
        'the inventory is pure'
    # The line and the number it is remembered by come from one read of the inventory.
    take = body(header, '    bool TakeLine(std::string& line, uint64_t nowMs)')
    assert take.count('Take()') == 1 and 'Signature(now)' in take and 'line = Describe(now);' in take, \
        'the line and its number are read separately'
    # The numbers are Streamline's, and the vendored header agrees.
    for name, value in (('kBufferTypeDepth', 0), ('kBufferTypeMotionVectors', 1), ('kBufferTypeHUDLessColor', 2),
                        ('kBufferTypeScalingInputColor', 3), ('kBufferTypeScalingOutputColor', 4),
                        ('kBufferTypeExposure', 13), ('kBufferTypeUIColorAndAlpha', 23), ('kBufferTypeHiResDepth', 48),
                        ('kBufferTypeLinearDepth', 49), ('kBufferTypeBidirectionalDistortionField', 50),
                        ('kBufferTypeBackbuffer', 53), ('kBufferTypeNoWarpMask', 54)):
        assert f'constexpr BufferType {name} = {value};' in VENDORED, f'{name} is not {value} in Streamline'
    assert re.search(r'eOnlyValidNow,\s*(//[^\n]*\n\s*)*eValidUntilPresent,\s*(//[^\n]*\n\s*)*eValidUntilEvaluate',
                     VENDORED), 'the lifecycle order is not Streamline\'s'

    hooks = s['hooks']
    assert '#include <dlssnr/DlssNr_SlTagInventory.h>' in hooks

    # --- The helper: nothing before it is armed, GetDesc only on D3D12.
    helper = body(hooks, 'static void NoteTagInventory(')
    armed = helper.index('if (!inventory.Armed())')
    assert armed < helper.index('NoteTags(') and armed < helper.index('GetDesc()')
    assert re.search(r'if \(api != sl::RenderAPI::eD3D12 \|\|\s*\(resource\.type != sl::ResourceType::eTex2d && '
                     r'resource\.type != sl::ResourceType::eBuffer\)\)\s*return;', helper), \
        'GetDesc on something that may not be an ID3D12Resource'
    assert helper.index('if (api != sl::RenderAPI::eD3D12 ||') < helper.index('GetDesc()')
    assert re.search(r'if \(desc\.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER\)\s*\{\s*(//[^\n]*\n\s*)*shape = \{\};\s*'
                     r'shape\.buffer = true;\s*return;',
                     helper), 'a buffer\'s byte width is said as an image width'
    assert 'LOG_' not in helper and 'Config::' not in helper

    # --- Upstream's tag hooks note before anything else they do.
    for name, signature, via in (
            ('slSetTag', 'sl::Result StreamlineHooks::hkslSetTag(', 'kViaSetTag'),
            ('slSetTagForFrame', 'sl::Result StreamlineHooks::hkslSetTagForFrame(', 'kViaSetTagForFrame')):
        hook = body(hooks, signature)
        noted = hook.index(f'NoteTagInventory(')
        assert f'DlssNr::SlTagInventory::{via}' in hook[noted:noted + 200], f'{name} notes the wrong source'
        assert noted < hook.index('if (renderApi == sl::RenderAPI::eD3D11)'), f'{name} returns before noting'

    # --- The note-only hooks: note and forward, nothing else.
    for name, signature, forward in (
            ('slSetTag', 'sl::Result StreamlineHooks::hkslSetTagNoteOnly(',
             'return o_slSetTag(viewport, tags, numTags, cmdBuffer);'),
            ('slSetTagForFrame', 'sl::Result StreamlineHooks::hkslSetTagForFrameNoteOnly(',
             'return o_slSetTagForFrame(frame, viewport, resources, numResources, cmdBuffer);')):
        hook = body(hooks, signature)
        statements = [line.strip() for line in hook.split('{', 1)[1].rsplit('}', 1)[0].splitlines() if line.strip()]
        assert len(statements) == 3, f'the note-only {name} hook does more than note and forward: {statements}'
        assert statements[0].startswith('DlssNr::SlNativeNr::OnTags(')
        assert statements[1].startswith('NoteTagInventory(')
        assert statements[2] == forward
    declaration = s['declaration']
    assert 'VALIDATE_MEMBER_HOOK(hkslSetTagNoteOnly, decltype(&slSetTag))' in declaration
    assert 'VALIDATE_MEMBER_HOOK(hkslSetTagForFrameNoteOnly, decltype(&slSetTagForFrame))' in declaration

    # --- The evaluate's own inputs: noted while armed, before the call.
    evaluate = body(hooks, 'sl::Result StreamlineHooks::hkslEvaluateFeature(')
    gate = evaluate.index('if (inputs != nullptr && DlssNr::SlTagInventory::Instance().Armed())')
    assert gate < evaluate.index('auto result = o_slEvaluateFeature(')
    assert 'DlssNr::SlTagInventory::kViaEvaluate' in evaluate[gate:evaluate.index('auto result = o_slEvaluateFeature(')]

    # --- Install: armed only with NR on; the note-only hooks only with NR on and no Streamline FG input.
    attach = body(hooks, 'void StreamlineHooks::hookInterposer(HMODULE slInterposer)')
    nr_on = attach.index('const bool nrOn = Config::Instance()->DlssNrEnabled.value_or_default();')
    arm = attach.index('DlssNr::SlTagInventory::Instance().Arm();')
    assert re.search(r'if \(nrOn\)\s*DlssNr::SlTagInventory::Instance\(\)\.Arm\(\);', attach), 'armed with NR off'
    fg_gate = attach.index('if (State::Instance().activeFgInput == FGInput::NvngxFG ||')
    condition = ' '.join(attach[fg_gate:attach.index('{', fg_gate)].split())
    assert condition == ('if (State::Instance().activeFgInput == FGInput::NvngxFG || '
                         'State::Instance().activeFgInput == FGInput::DLSSG)'), f'the FG input gate changed: {condition}'
    assert nr_on < arm < fg_gate
    note_only = attach.index('else if (nrOn)')
    assert fg_gate < note_only
    fg_block = attach[fg_gate:note_only]
    assert 'NoteOnly' not in fg_block, 'a note-only hook in the FG input branch'
    assert 'AttachInterposerHook(&(PVOID&) o_slSetTag, (PVOID) hkslSetTag);' in fg_block
    note_block = body(attach[note_only:], 'else if (nrOn)')
    for line in ('AttachInterposerHook(&(PVOID&) o_slSetTag, (PVOID) hkslSetTagNoteOnly);',
                 'AttachInterposerHook(&(PVOID&) o_slSetTagForFrame, (PVOID) hkslSetTagForFrameNoteOnly);'):
        assert line in note_block, f'the note-only install lost: {line}'
    assert 'hkslSetConstants' not in note_block, 'the note-only branch hooks more than the tags'
    assert attach.count('NoteOnly') == 2

    # --- Detach: exactly what was attached, whichever tag hooks those were (tests/sl-interposer-unhook).
    unhook = body(hooks, 'bool StreamlineHooks::unhookInterposer()')
    live = '\n'.join(line for line in unhook.splitlines() if not line.strip().startswith('//'))
    assert 's_interposerHooks.DetachAll(' in live and 'DetourDetach(&(PVOID&) o_' not in live, \
        'the unhook detaches exports it did not attach'

    # --- Said by the D3D12 status line, after its two-second gate, and by the Vulkan after-upscale route.
    status = body(s['renderer'], 'void ReportRuntimeStatus(bool passRecorded, const char* skipReason)')
    said = status.index('DlssNr::SlTagInventory::Instance().TakeLine(tags, DlssNr::SlTagInventory::NowMs())')
    assert status.index('lastAt = now;') < said < status.index('if (renders == 0 && presents == 0)')
    vk = body(s['vk'], 'void EvaluateAfterUpscaleVk(')
    said = vk.index('DlssNr::SlTagInventory::Instance().TakeLine(tags, DlssNr::SlTagInventory::NowMs())')
    assert vk.index('DlssNr::SlNativeNr::AskInto(') < said < vk.index('if (g_vkResetOnReturn.exchange(false))')

    # --- The record.
    assert 'sl-tag-inventory.md' in s['index'], 'the design index does not list the note'
    assert 'log only' in s['note'].lower()


check(sources)
print('PASS: hooks note with NR on only, note-only hooks forward untouched, detach matches attach, line said by both '
      'routes')

SOURCE_MUTANTS = {
    'armed with NR off': ('hooks', 'if (nrOn)\n                    DlssNr::SlTagInventory::Instance().Arm();',
                          'DlssNr::SlTagInventory::Instance().Arm();'),
    'note-only with NR off': ('hooks', '                else if (nrOn)\n                {\n                    // No Streamline FG',
                              '                else\n                {\n                    // No Streamline FG'),
    'detach what was not attached': ('hooks', '    s_interposerHooks.DetachAll(',
                                     '    DetourDetach(&(PVOID&) o_slSetConstants, hkslSetConstants);\n    s_interposerHooks.DetachAll('),
    'note-only edits the tags': ('hooks', '    NoteTagInventory(tags, numTags, DlssNr::SlTagInventory::kViaSetTag, renderApi);\n'
                                 '    return o_slSetTag(viewport, tags, numTags, cmdBuffer);',
                                 '    NoteTagInventory(tags, numTags, DlssNr::SlTagInventory::kViaSetTag, renderApi);\n'
                                 '    LOG_TRACE("tags");\n    return o_slSetTag(viewport, tags, numTags, cmdBuffer);'),
    'GetDesc on Vulkan': ('hooks', 'if (api != sl::RenderAPI::eD3D12 ||\n                (resource.type',
                          'if (false ||\n                (resource.type'),
    'GetDesc on any type': ('hooks', '(resource.type != sl::ResourceType::eTex2d && resource.type != sl::ResourceType::eBuffer)',
                            '(resource.type == sl::ResourceType::eCount)'),
    'DLSSG polarity': ('hooks', 'State::Instance().activeFgInput == FGInput::DLSSG)\n                {\n                    if (o_slSetTag != nullptr)',
                       'State::Instance().activeFgInput != FGInput::DLSSG)\n                {\n                    if (o_slSetTag != nullptr)'),
    'buffer width as pixels': ('hooks', '                shape = {};\n                shape.buffer = true;\n                return;\n', ''),
    'second read for the line': ('header', '        line = Describe(now);', '        line = Describe();'),
    'evaluate unarmed': ('hooks', 'if (inputs != nullptr && DlssNr::SlTagInventory::Instance().Armed())',
                         'if (inputs != nullptr)'),
    'said before the gate': ('renderer', '    lastAt = now;\n\n    // Which buffers the game tags',
                             '\n    // Which buffers the game tags'),
}

for name, (which, old, new) in SOURCE_MUTANTS.items():
    mutated = dict(sources)
    text = mutated[which].replace('\r\n', '\n')
    assert text.count(old) == 1, f'source mutant {name!r} no longer applies'
    mutated[which] = text.replace(old, new)
    try:
        check(mutated)
    except (AssertionError, ValueError):
        continue
    raise SystemExit(f'FAIL: the source guards accepted the mutant: {name}')
print(f'PASS: {len(SOURCE_MUTANTS)} source mutants rejected')

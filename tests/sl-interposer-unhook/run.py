#!/usr/bin/env python3
"""Streamline interposer unhook: the detour record on a fake Detours, and the hooks that use it, read off the source.

The record header is compiled under ASan/UBSan and broken one rule at a time. StreamlineHooks::hookInterposer and
unhookInterposer are read and each guard is broken once. No Streamline, no Detours, no game.
"""
from pathlib import Path
import re
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
RECORD = ROOT / 'retoque/hooks/DetourRecord.h'
FLAGS = ['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
         '-fno-omit-frame-pointer']

RECORD_MUTANTS = {
    'refused attach recorded': ('        if (result == 0)\n            _pending.push_back', '        _pending.push_back'),
    'failed commit recorded': ('        if (ok)\n            _attached.insert(', '        if (ok || true)\n            _attached.insert('),
    'oldest first': ('for (auto it = _attached.rbegin(); it != _attached.rend(); ++it)',
                     'for (auto it = _attached.begin(); it != _attached.end(); ++it)'),
    'failed unhook forgets': ('        if (!ok)\n            return;\n        for (const auto& entry : _attached)',
                              '        if (!ok && false)\n            return;\n        for (const auto& entry : _attached)'),
    'originals kept': ('            *entry.target = nullptr;\n', '            (void) entry;\n'),
    'begin keeps the pending': ('    void Begin() { _pending.clear(); }', '    void Begin() {}'),
}

with tempfile.TemporaryDirectory(prefix='retoque-sl-unhook-') as folder:
    folder = Path(folder)
    subprocess.run(FLAGS + ['-I' + str(ROOT), str(HERE / 'cases.cpp'), '-o', str(folder / 'cases')], check=True)
    subprocess.run([str(folder / 'cases')], check=True)
    record = RECORD.read_text()
    for index, (name, (old, new)) in enumerate(RECORD_MUTANTS.items()):
        assert record.count(old) == 1, f'record mutant {name!r} no longer applies'
        root = folder / f'mutant{index}'
        target = root / 'retoque/hooks/DetourRecord.h'
        target.parent.mkdir(parents=True)
        target.write_text(record.replace(old, new))
        built = subprocess.run(FLAGS + ['-I' + str(root), str(HERE / 'cases.cpp'), '-o', str(root / 'cases')],
                               capture_output=True)
        assert built.returncode == 0, f'record mutant {name!r} does not compile:\n{built.stderr.decode()}'
        assert subprocess.run([str(root / 'cases')], capture_output=True).returncode != 0, \
            f'record mutant {name!r} escaped the cases'
    print(f'PASS: {len(RECORD_MUTANTS)} record mutants fail the cases')


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


hooks_text = (ROOT / 'OptiScaler/hooks/Streamline_Hooks.cpp').read_text(encoding='utf-8-sig').replace('\r\n', '\n')
header_text = (ROOT / 'OptiScaler/hooks/Streamline_Hooks.h').read_text(encoding='utf-8-sig').replace('\r\n', '\n')


def check(hooks, header):
    assert '#include <hooks/DetourRecord.h>' in hooks
    assert 'static bool unhookInterposer();' in header, 'the unhook does not say whether it worked'

    attach = body(hooks, 'void StreamlineHooks::hookInterposer(HMODULE slInterposer)')
    live = [line for line in attach.splitlines() if not line.strip().startswith('//')]
    raw = [line for line in live if 'DetourAttach(' in line]
    assert not raw, f'an interposer hook attached outside the record: {raw}'
    recorded = [line for line in live if 'AttachInterposerHook(' in line]
    assert len(recorded) >= 19, f'only {len(recorded)} interposer hooks go through the record'
    commits = attach.count('DetourTransactionCommit();')
    assert attach.count('s_interposerHooks.Begin();') == commits == 2, 'a transaction is not bracketed'
    assert attach.count('s_interposerHooks.Committed(detourResult == NO_ERROR);') == commits
    for begin in re.finditer(r'DetourTransactionBegin\(\);', attach):
        nxt = attach[begin.end():begin.end() + 200]
        assert nxt.lstrip().startswith('s_interposerHooks.Begin();') or 'DetourUpdateThread' in nxt.split('\n')[1] \
            or 's_interposerHooks.Begin();' in nxt, 'the record does not start with its transaction'
    # One swap at a time, admitted without waiting (a caller may hold the loader lock), released on every return.
    admit = attach.index('if (!s_interposerSwap.compare_exchange_strong(idle, true))')
    assert admit < attach.index('if (hookedInterposerModule == slInterposer)') < attach.index('s_interposerHooks.Size()')
    assert 'return;' in attach[admit:admit + 300] and 'lock(' not in attach.split('compare_exchange_strong')[0][-400:]
    assert re.search(r'struct SwapDone\s*\{\s*~SwapDone\(\) \{ s_interposerSwap\.store\(false\); \}\s*\} swapDone;', attach), \
        'the swap is not released on every return'
    assert attach.index('} swapDone;') < attach.index('if (hookedInterposerModule == slInterposer)')
    # A second interposer: refused when the first one's hooks cannot come off, never hooked over them.
    second = attach.index('if (s_interposerHooks.Size() != 0 && !unhookInterposer())')
    refused = attach[second:attach.index('}', second)]
    assert 'return;' in attach[second:second + 400] and 'State::EnableChecks(owner);' in refused
    assert second < attach.index('hookedInterposerModule = slInterposer;')

    helper = body(hooks, 'static void AttachInterposerHook(PVOID* target, PVOID detour)')
    assert 's_interposerHooks.Attach(' in helper and 'DetourAttach(t, d)' in helper

    unhook = body(hooks, 'bool StreamlineHooks::unhookInterposer()')
    assert 'DetourDetach(&(PVOID&)' not in unhook.replace('//     DetourDetach(&(PVOID&)', ''), \
        'the unhook detaches an export by name, attached or not'
    assert 's_interposerHooks.DetachAll(' in unhook
    commit = unhook.index('auto detourResult = DetourTransactionCommit();')
    assert unhook.index('s_interposerHooks.DetachAll(') < commit < unhook.index(
        's_interposerHooks.Detached(detourResult == NO_ERROR);')
    assert 'return detourResult == NO_ERROR;' in unhook
    for gone in ('s_slSetTagDetour', 's_slSetTagForFrameDetour', 'noteOnlyTags'):
        assert gone not in hooks and gone not in header, f'{gone}: the old per-hook bookkeeping is back'


check(hooks_text, header_text)
print('PASS: every interposer hook attached through the record, each transaction bracketed, the unhook detaches '
      'only what is recorded and says whether it worked, a second interposer is not hooked over live hooks')

SOURCE_MUTANTS = {
    'raw attach': ('hooks', 'AttachInterposerHook(&(PVOID&) o_slInit, (PVOID) hkslInit);',
                   'DetourAttach(&(PVOID&) o_slInit, hkslInit);'),
    'detach by name': ('hooks', '    s_interposerHooks.DetachAll(',
                       '    DetourDetach(&(PVOID&) o_slSetConstants, hkslSetConstants);\n    s_interposerHooks.DetachAll('),
    'commit not recorded': ('hooks', '                s_interposerHooks.Committed(detourResult == NO_ERROR);\n', ''),
    'unhook result ignored': ('hooks', 'if (s_interposerHooks.Size() != 0 && !unhookInterposer())',
                              'if (s_interposerHooks.Size() != 0 && !unhookInterposer() && false)'),
    'unhook says nothing': ('hooks', '    return detourResult == NO_ERROR;\n}', '    return true;\n}'),
    'swap not admitted': ('hooks', 'if (!s_interposerSwap.compare_exchange_strong(idle, true))',
                          'if (!s_interposerSwap.compare_exchange_strong(idle, true) && false)'),
    'swap never released': ('hooks', '        ~SwapDone() { s_interposerSwap.store(false); }', '        ~SwapDone() {}'),
    'detached not told': ('hooks', '    s_interposerHooks.Detached(detourResult == NO_ERROR);\n', ''),
}

for name, (which, old, new) in SOURCE_MUTANTS.items():
    texts = {'hooks': hooks_text, 'header': header_text}
    assert texts[which].count(old) >= 1, f'source mutant {name!r} no longer applies'
    texts[which] = texts[which].replace(old, new, 1)
    try:
        check(texts['hooks'], texts['header'])
    except (AssertionError, ValueError):
        continue
    raise SystemExit(f'FAIL: the source guards accepted the mutant: {name}')
print(f'PASS: {len(SOURCE_MUTANTS)} source mutants rejected')

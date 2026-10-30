#!/usr/bin/env python3
"""NR engagement funnel (docs/dlssnr/design/engagement-funnel.md): the counting header compiled as it is under
ASan/UBSan with threads, plus source guards that every way out of the D3D12 after-upscale pass names its outcome,
that nothing is counted with NR off, and that the counting takes no lock."""
from pathlib import Path
import re
import subprocess
import tempfile

here = Path(__file__).resolve().parent
root = here.parents[1]
failures = []


def read(relative):
    return (root / relative).read_text(encoding='utf-8-sig')


def require(condition, message):
    if not condition:
        failures.append(message)


def function_body(source, signature):
    """The text of one function: from its signature to the brace that closes it."""
    start = source.find(signature)
    if start < 0:
        return ''
    brace = source.index('{', start)
    depth = 0
    for i in range(brace, len(source)):
        depth += {'{': 1, '}': -1}.get(source[i], 0)
        if depth == 0:
            return source[start:i + 1]
    return ''


def enclosing_block(text, at):
    """From the brace that opens the block holding position `at` up to `at`."""
    depth = 0
    for i in range(at - 1, -1, -1):
        if text[i] == '}':
            depth += 1
        elif text[i] == '{':
            if depth == 0:
                return text[i:at]
            depth -= 1
    return text[:at]


# --- The header: standard library only, lock-free ------------------------------------------------------------------

header = read('retoque/dlssnr/DlssNr_Funnel.h') if (root / 'retoque/dlssnr/DlssNr_Funnel.h').exists() else ''
require(header, 'retoque/dlssnr/DlssNr_Funnel.h does not exist')
includes = re.findall(r'^#include\s+(\S+)', header, re.M)
require(includes and all(i.startswith('<') and '/' not in i and '.' not in i for i in includes),
        f'DlssNr_Funnel.h must include only standard headers (found {includes})')
require(not re.search(r'\b(mutex|lock_guard|unique_lock|scoped_lock|condition_variable)\b', header),
        'the funnel counts without a lock: it runs on every evaluate with NR on and is read from a crash handler')
require(not re.search(r'\bstd::(string|vector|map|unordered_map)\b|\bnew\b|malloc', header),
        'the funnel allocates nothing: a crash handler reads it')
enum = re.search(r'enum class Decline : uint8_t\s*\{(.*?)\};', header, re.S)
require(enum, 'DlssNr_Funnel.h no longer declares enum class Decline; update this runner')
declines = [d for d in re.findall(r'^\s*(\w+)\s*(?:=\s*\d+)?\s*,', enum.group(1), re.M) if d != 'Count'] if enum else []

# --- The D3D12 after-upscale route ---------------------------------------------------------------------------------

dx12 = read('retoque/shaders/dlssnr/DlssNr_Dx12.cpp')
evaluate = function_body(dx12, 'void EvaluateAfterUpscale(ID3D12GraphicsCommandList* cmdList')
require(evaluate, 'DlssNr_Dx12.cpp no longer defines EvaluateAfterUpscale; update this runner')
off = evaluate.find('if (!dlssNrEnabled)')
off_return = evaluate.find('return;', off)
scope = evaluate.find('Funnel::Evaluation funnel(Funnel::Route::D3d12AfterUpscale, GetCurrentThreadId());')
require(0 <= off < off_return < scope,
        'the funnel scope opens after the NR-off return: with NR off nothing is counted (rule 1, no per-frame work)')
require(evaluate.count('Funnel::Evaluation funnel(') == 1, 'one funnel scope per evaluate')
first_return_after_off = evaluate.find('return;', off_return + 1)
require(scope < first_return_after_off, 'the funnel scope opens before any other way out of the pass')

returns = [m.start() for m in re.finditer(r'\breturn;', evaluate) if m.start() > scope]
for at in returns:
    block = enclosing_block(evaluate, at)
    named = re.findall(r'funnel\.Declined\(Funnel::Decline::(\w+)', block)
    line = evaluate[:at].count('\n') + 1
    require(len(named) == 1, f'the return at line {line} of EvaluateAfterUpscale names exactly one decline '
                             f'(found {named}); a silent return leaves the ladder unbalanced')
tail = evaluate[returns[-1]:] if returns else evaluate
require(re.search(r'if \(coverage\.sample\.applied\)\s*funnel\.Ran\(coverage\.sample\.modelCalled\);\s*else\s*'
                  r'funnel\.Declined\(Funnel::Decline::PassRecordedNothing, coverage\.sample\.reason\);', tail),
        'the end of the pass counts what Dispatch did: ran (with whether the model was called) or recorded nothing')
require(re.search(r'funnel\.Recording\(\);\s*(?:\n.*){0,6}g_compose->Dispatch\(', evaluate),
        'the stage mark says the pass is recording just before Dispatch')
used = set(re.findall(r'Funnel::Decline::(\w+)', evaluate))
for decline in declines:
    require(decline in used, f'Decline::{decline} is never counted by EvaluateAfterUpscale: a dead bucket')
require('#include <dlssnr/DlssNr_Funnel.h>' in dx12, 'DlssNr_Dx12.cpp includes the funnel header')

# --- Build and run ---------------------------------------------------------------------------------------------------

cases = None
if header:
    with tempfile.TemporaryDirectory(prefix='optiscaler-nr-funnel-') as directory:
        binary = Path(directory) / 'cases'
        built = subprocess.run(['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-pthread',
                                '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined',
                                '-fno-omit-frame-pointer', '-I', str(root / 'retoque'), str(here / 'cases.cpp'),
                                '-o', str(binary)])
        require(built.returncode == 0, 'cases.cpp did not compile')
        if built.returncode == 0:
            cases = subprocess.run([str(binary)])

if failures:
    for failure in failures:
        print('FAIL:', failure)
if failures or cases is None or cases.returncode != 0:
    raise SystemExit(1)

print('PASS: funnel source guards')

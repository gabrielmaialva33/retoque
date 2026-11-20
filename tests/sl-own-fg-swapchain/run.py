#!/usr/bin/env python3
"""A game's own DLSS-G hooks are cleared when OptiScaler makes the frame generation swapchain itself.

retoque/hooks/Streamline_OwnFgSwapchain.h is compiled under ASan/UBSan against the production FGInput/FGOutput enums
(sliced out of OptiScaler/State.h) and checked over every input, output and API; each rule is then broken once and must
fail. Its rule is held to the first rule of CheckForFGStatus, and the three places that use it (sl.dlss_g plugin load,
slDLSSGSetOptions, the frame generation menu) are read off the source. No Streamline, no game.
"""
from pathlib import Path
import re
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
RULE = ROOT / 'retoque/hooks/Streamline_OwnFgSwapchain.h'
STATE = ROOT / 'OptiScaler/State.h'
FG_HOOKS = ROOT / 'OptiScaler/hooks/FG_Hooks.cpp'
SL_HOOKS = ROOT / 'OptiScaler/hooks/Streamline_Hooks.cpp'
MENU = ROOT / 'OptiScaler/menu/menu_common.cpp'
FLAGS = ['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
         '-fno-omit-frame-pointer']


def enum(source, name):
    begin = source.index(f'enum class {name} : uint32_t')
    return source[begin:source.index('};', begin) + 2]


def function(source, signature):
    begin = source.index(signature)
    start = source.index('{', begin)
    depth, end = 1, start + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[begin:end]


def names(enum_text):
    body = enum_text[enum_text.index('{') + 1:enum_text.rindex('}')]
    return [m.group(1) for m in re.finditer(r'^\s*(\w+)\s*,', body, re.M)]


state = STATE.read_text()
fg_input, fg_output = enum(state, 'FGInput'), enum(state, 'FGOutput')
inputs, outputs = names(fg_input), names(fg_output)
assert {'NoFG', 'NvngxFG', 'DLSSG', 'Upscaler'} <= set(inputs), inputs
assert {'NoFG', 'DLSSG', 'XeFG', 'FSRFG'} <= set(outputs), outputs

# The expected table, written here independently of the header: OptiScaler makes the FG swapchain for every input but
# None and NvngxFG when there is an output, on D3D12 (the API this rule is limited to).
cases = []
for vulkan in (False, True):
    for i in inputs:
        for o in outputs:
            makes = not vulkan and i not in ('NoFG', 'NvngxFG') and o != 'NoFG'
            cases.append(f'    CHECK(StreamlineOwnFg::MakesFgSwapchainD3D12({str(vulkan).lower()}, FGInput::{i}, '
                         f'FGOutput::{o}) == {str(makes).lower()});')
for cleared in (False, True):
    for i in inputs:
        replaces = cleared and i != 'DLSSG'
        cases.append(f'    CHECK(StreamlineOwnFg::ReplacesGameDlssg({str(cleared).lower()}, FGInput::{i}) == '
                     f'{str(replaces).lower()});')

UNIT = '''#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <hooks/Streamline_OwnFgSwapchain.h>
@ENUMS@
static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL line %d: %s\\n", __LINE__, #x); ++failures; } } while (0)
static_assert(StreamlineOwnFg::MakesFgSwapchainD3D12(false, FGInput::Upscaler, FGOutput::XeFG));
int main()
{
@CASES@
    if (failures)
        return 1;
    std::puts("PASS: own frame generation swapchain rule over every input, output and API");
    return 0;
}
'''.replace('@ENUMS@', fg_input + '\n' + fg_output).replace('@CASES@', '\n'.join(cases))

RULE_MUTANTS = {
    'Vulkan included': ('return !vulkan && input', 'return input'),
    'NvngxFG makes a swapchain': (' && input != Input::NvngxFG', ''),
    'no output needed': (' && output != Output::NoFG', ''),
    'None makes a swapchain': ('input != Input::NoFG && ', ''),
    'DLSSG input replaces': ('hooksCleared && input != Input::DLSSG', 'hooksCleared'),
    'replaced without clearing': ('return hooksCleared && ', 'return '),
}


def build_and_run(root, folder, expect_pass):
    (folder / 'unit.cpp').write_text(UNIT)
    built = subprocess.run(FLAGS + ['-I' + str(root), str(folder / 'unit.cpp'), '-o', str(folder / 'unit')],
                           capture_output=True, text=True)
    if built.returncode != 0:
        if expect_pass:
            raise SystemExit(built.stderr)
        return False
    ran = subprocess.run([str(folder / 'unit')], capture_output=True, text=True)
    if expect_pass and ran.returncode != 0:
        raise SystemExit(ran.stdout + ran.stderr)
    return ran.returncode == 0


# The rule stays the first rule of CheckForFGStatus: if that changes, this rule (and its comment) must be revisited.
check = function(FG_HOOKS.read_text(), 'static bool CheckForFGStatus(')
rule_at = check.index('if (State::Instance().activeFgInput')
first = re.sub(r'\s+', ' ', check[rule_at:check.index('return false;', rule_at) + len('return false;')])
assert first == ('if (State::Instance().activeFgInput == FGInput::NoFG || State::Instance().activeFgInput == '
                 'FGInput::NvngxFG) return false;'), f'CheckForFGStatus first rule changed: {first}'

# Wiring, read off the source; each guard is a substring the mutant below removes.
sl = SL_HOOKS.read_text()
load = function(sl, 'bool StreamlineHooks::hkdlssg_slOnPluginLoad(')
options = function(sl, 'sl::Result StreamlineHooks::hkslDLSSGSetOptions(')
menu = MENU.read_text()
WIRING = {
    'plugin load asks the rule with the API': (load, 'StreamlineOwnFg::MakesFgSwapchainD3D12(renderApi == sl::RenderAPI::eVulkan'),
    'plugin load clears for its own swapchain': (load, 'fgState.activeFgOutput == FGOutput::DLSSG || ownsFgSwapchain)'),
    'plugin load records the replacement': (load, 'fgState.gameDlssgReplacedByOwnFg.store(true'),
    'options force the game off when replaced': (options, 'state.gameDlssgReplacedByOwnFg.load(std::memory_order_relaxed))'),
    'options free the game resources': (options, 'newOptions.flags &= ~sl::DLSSGFlags::eRetainResourcesWhenOff;'),
    'menu hides the game DLSS-G controls': (menu, 'state.gameDlssgReplacedByOwnFg.load(std::memory_order_relaxed);'),
}
for name, (body, guard) in WIRING.items():
    assert body.count(guard) == 1, f'wiring {name!r}: guard not found once'
# Ordering: the hooks are cleared inside the block the condition opens, and the forced-off branch comes before any
# policy that could turn the game's DLSS-G on (auto -> on, Dynamic MFG).
clear_at = load.index('ownsFgSwapchain)')
assert load.index('configJson["hooks"].clear()', clear_at) > clear_at
assert load.index('configJson["exclusive_hooks"].clear()', clear_at) > clear_at
replaced_at = options.index('state.gameDlssgReplacedByOwnFg.load')
assert replaced_at < options.index('if (newOptions.mode == sl::DLSSGMode::eAuto)'), 'forced off after the auto policy'
assert options.index('newOptions.mode = sl::DLSSGMode::eOff;', replaced_at) < options.index(
    'if (newOptions.mode == sl::DLSSGMode::eAuto)'), 'replaced branch does not force eOff'

with tempfile.TemporaryDirectory(prefix='retoque-sl-own-fg-') as folder:
    folder = Path(folder)
    build_and_run(ROOT / 'retoque', folder, True)
    rule = RULE.read_text()
    survivors = []
    for index, (name, (old, new)) in enumerate(RULE_MUTANTS.items()):
        assert rule.count(old) == 1, f'rule mutant {name!r} no longer applies'
        root = folder / f'mutant{index}'
        target = root / 'hooks/Streamline_OwnFgSwapchain.h'
        target.parent.mkdir(parents=True)
        target.write_text(rule.replace(old, new))
        if build_and_run(root, root, False):
            survivors.append(name)
    if survivors:
        raise SystemExit(f'rule mutants survived: {survivors}')
    print(f'PASS: {len(RULE_MUTANTS)} rule mutants fail; CheckForFGStatus first rule matches; '
          f'{len(WIRING)} wiring guards present in plugin load, slDLSSGSetOptions and the menu')

#!/usr/bin/env python3
"""The driver-init gate (retoque/dlssnr/DlssNr_DriverInitGate.h): the production header on real threads under ASan/UBSan,
mutation controls that must turn cases red, and source guards on every site that takes it -- each vkCreateDevice call
down of ours (NR on only), each NGX initialisation, core bring-up and feature creation the model makes -- with the locks
held at each site checked against the documented order, and the sites that take those locks frozen so a new one is
reviewed against it."""
from pathlib import Path
import argparse
import os
import re
import subprocess
import sys
import tempfile

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--no-mutations', action='store_true', help='skip the regression controls')
args = parser.parse_args()

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
HEADER = ROOT / 'retoque/dlssnr/DlssNr_DriverInitGate.h'

CASES = ['exclusion', 'reentrant', 'not-wanted', 'suspend-resume', 'no-suspend-under-ngx', 'bounded',
         'resume-times-out', 'hook-chain', 'process-and-reporter']

FLAGS = ['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-fsanitize=address,undefined',
         '-fno-sanitize-recover=undefined', '-fno-omit-frame-pointer', '-pthread']
env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0:detect_stack_use_after_return=1')
failed = []


def say(ok, what):
    print(f'{"PASS" if ok else "FAIL"} {what}', flush=True)
    if not ok:
        failed.append(what)


with tempfile.TemporaryDirectory(prefix='nr-driver-init-gate-') as folder:
    folder = Path(folder)

    def build(name, header_text):
        include = folder / name / 'include'
        (include / 'dlssnr').mkdir(parents=True)
        (include / 'dlssnr/DlssNr_DriverInitGate.h').write_text(header_text)
        binary = folder / name / 'cases'
        subprocess.run(FLAGS + ['-I', str(include), str(HERE / 'cases.cpp'), '-o', str(binary)], check=True)
        return binary

    def run(binary, case):
        return subprocess.run([str(binary), case], env=env, capture_output=True, text=True, timeout=120)

    header = HEADER.read_text()
    binary = build('production', header)
    for case in CASES:
        result = run(binary, case)
        sys.stdout.write(result.stdout)
        if result.returncode:
            sys.stdout.write(result.stderr[-4000:])
        say(result.returncode == 0, case)

    if not args.no_mutations:
        def mutate(text, old, new):
            assert text.count(old) == 1, f'mutation anchor not found once: {old!r}'
            return text.replace(old, new)

        def control(name, mutated, cases, what):
            binary = build(name, mutated)
            for case in cases:
                result = run(binary, case)
                why = next((l for l in result.stderr.splitlines() if l.startswith('FAIL:')), '')
                say(result.returncode != 0, f'control: {case} fails {what}' + (f' ({why[:150]})' if why else ''))

        # The fix the design depends on: the present host's own code outside the gate. Without it the three-thread
        # chain only ends at the bound.
        control('no-suspend', mutate(header, '        if (_depth == 0 || _owner != std::this_thread::get_id() || _ngx != 0)\n'
                                             '            return 0;',
                                     '        return 0;'),
                ['hook-chain', 'suspend-resume'], 'when OwnCode lets nothing go')
        # A gate that never excludes: the original freeze's overlap.
        control('no-exclusion', mutate(header, '        if (_depth == 0 || _owner == std::this_thread::get_id())\n'
                                               '            return true;',
                                       '        return true;'),
                ['exclusion'], 'when a second holder is let in')
        # Not re-entrant: a hook below a hook waits for itself until the bound.
        control('no-reentry', mutate(header, '        if (_depth == 0 || _owner == std::this_thread::get_id())\n'
                                             '            return true;',
                                     '        if (_depth == 0)\n            return true;'),
                ['reentrant'], 'when the owning thread must wait for itself')
        # Suspending under an NGX call: a create nested in NGX lets another create in.
        control('suspend-under-ngx', mutate(header, ' || _ngx != 0)\n            return 0;', ')\n            return 0;'),
                ['no-suspend-under-ngx'], 'when OwnCode lets an NGX hold go')
        # A lost hold's scope end releasing whatever is held: another thread's hold.
        control('orphan-releases', mutate(header, '        if (_depth != 0 && _owner == me)\n',
                                          '        if (_depth != 0)\n'),
                ['resume-times-out'], "when a lost hold's end releases another thread's")


# ---- Source guards ------------------------------------------------------------------------------------------------

def strip(text):
    """Comments and string/char literals blanked (length kept), so braces and names in them do not count."""
    out = list(text)
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith('//', i):
            j = text.find('\n', i)
            j = n if j < 0 else j
            out[i:j] = ' ' * (j - i)
            i = j
        elif text.startswith('/*', i):
            j = text.index('*/', i) + 2
            out[i:j] = [ch if ch == '\n' else ' ' for ch in text[i:j]]
            i = j
        elif c == 'R' and text.startswith('R"', i):
            delim = text[i + 2:text.index('(', i)]
            j = text.index(')' + delim + '"', i) + len(delim) + 2
            out[i:j] = [ch if ch == '\n' else ' ' for ch in text[i:j]]
            i = j
        elif c in '"\'' and not (c == '\'' and i > 0 and (text[i - 1].isalnum())):
            j = i + 1
            while text[j] != c:
                j += 2 if text[j] == '\\' else 1
            out[i + 1:j] = ' ' * (j - i - 1)
            i = j + 1
        else:
            i += 1
    return ''.join(out)


def blocks(code):
    pairs, stack = {}, []
    for i, c in enumerate(code):
        if c == '{':
            stack.append(i)
        elif c == '}':
            pairs[stack.pop()] = i
    return pairs


LOCK = re.compile(r'\b(?:std::)?(?:lock_guard|unique_lock|scoped_lock)\b(?:<[^>;]*>)?\s+\w+\s*[({]\s*([\w:.>-]+)'
                  r'|\b(TransportLock)\s+\w+\s*;')


def own_level(code, start, end):
    """code[start:end] without the blocks that open and close inside it."""
    out, depth = [], 0
    for c in code[start:end]:
        if c == '{':
            depth += 1
        elif c == '}':
            depth -= 1
        elif depth == 0:
            out.append(c)
    return ''.join(out)


def held_at(code, pairs, pos):
    """The locks declared in the blocks enclosing pos, before it."""
    held = set()
    for open_, close in pairs.items():
        if open_ < pos < close:
            for m in LOCK.finditer(own_level(code, open_ + 1, pos)):
                held.add(m.group(1) or m.group(2))
    return held


def innermost(pairs, pos):
    return max(((o, c) for o, c in pairs.items() if o < pos < c), key=lambda p: p[0])


GATE_NGX = 'DlssNr::DriverInitGate::Scope gate(DlssNr::DriverInitGate::Hold::Ngx, true,'
GATE_DEVICE = 'DlssNr::DriverInitGate::Scope gate(DlssNr::DriverInitGate::Hold::DeviceCreate,'
LISTED = {'g_preMutex', 'g_nrMutex', 'g_vkMutex', 'g_presentNgxMutex', 'g_ownerMutex', 'g_fwd.mutex', 'TransportLock',
          'g_transportMutex', 'g_registryMutex', 'g_installMutex', 'g_loadMutex'}


def gated(path, call, count, gate, allowed, label):
    """Every `call` in path sits in a block that takes the gate first and does nothing else of ours, and the listed
    locks held around it are exactly `allowed`."""
    text = (ROOT / path).read_text()
    code = strip(text)
    pairs = blocks(code)
    sites = [m.start() for m in re.finditer(re.escape(call), code)]
    say(len(sites) == count, f'{label}: {call} appears {count} time(s) in {path} (found {len(sites)})')
    for pos in sites:
        open_, close = innermost(pairs, pos)
        body = code[open_ + 1:close]
        first = ' '.join(body.split()).split(';', 1)[0] + ';'
        say(first.startswith(gate), f'{label}: the block around {call} at {path}:{text.count(chr(10), 0, pos) + 1} '
                                    f'takes the gate first')
        say(not LOCK.search(body) and body.count(';') <= 2,
            f'{label}: nothing but the call runs under the gate (no lock, one statement)')
        held = held_at(code, pairs, pos) & LISTED
        say(held == set(allowed), f'{label}: held at the gate: {sorted(held) or "nothing"} '
                                  f'(the order allows {sorted(allowed) or "nothing"})')
    return text


# The D3D12 route (and the D3D11 bridge, which runs it): Dispatch holds g_nrMutex; the before-upscale route's caller
# holds g_preMutex above it (g_preMutex -> g_nrMutex -> gate).
gated('retoque/shaders/dlssnr/DlssNr_Dx12.cpp', 'g_nr.create(', 2, GATE_NGX, {'g_nrMutex'}, 'D3D12 feature')
model = gated('retoque/shaders/dlssnr/DlssNr_Dx12_Model.cpp', 'NVNGXProxy::InitDx12(', 1, GATE_NGX, set(),
              'D3D12 core init')
gated('retoque/shaders/dlssnr/DlssNr_Dx12_Model.cpp', ' create(cmdList, (NVSDK_NGX_Feature)', 2, GATE_NGX, set(),
      'proxy probe')
gated('retoque/shaders/dlssnr/DlssNr_Dx12_Model.cpp', ' init(snippet->wstring()', 1, GATE_NGX, {'g_nrMutex'},
      'D3D11 init probe')
gated('retoque/shaders/dlssnr/DlssNr_Dx12_Enlarge.cpp', 'feature.Create(api, cmdList, shape)', 1, GATE_NGX, set(),
      'enlargement feature')
# The Vulkan after-upscale route under g_vkMutex; the present host's builder under no lock of ours.
feature = (ROOT / 'retoque/dlssnr/DlssNrFeature_Vk.cpp').read_text()
for call, label in (('g_fwd.init(', 'Vulkan NGX init'), ('g_fwd.create(', 'Vulkan feature')):
    code = strip(feature)
    pairs = blocks(code)
    builder = code.index('void RunBuild(PresentBuildJob& job)')
    sites = [m.start() for m in re.finditer(re.escape(call), code)]
    say(len(sites) == 2, f'{label}: {call} twice in DlssNrFeature_Vk.cpp, the route and the builder (found {len(sites)})')
    for pos in sites:
        open_, close = innermost(pairs, pos)
        body = code[open_ + 1:close]
        line = feature.count('\n', 0, pos) + 1
        say(' '.join(body.split()).startswith(GATE_NGX), f'{label}: DlssNrFeature_Vk.cpp:{line} takes the gate first')
        say(not LOCK.search(body) and body.count(';') <= 2, f'{label}: :{line} runs only the call under it')
        held = held_at(code, pairs, pos) & LISTED
        expected = set() if pos > builder else {'g_vkMutex'}
        say(held == expected, f'{label}: :{line} holds {sorted(held) or "nothing"} (allowed {sorted(expected) or "nothing"})')
gated('retoque/dlssnr/DlssNrFeature_Vk.cpp', 'NVNGXProxy::InitVulkan(', 1, GATE_NGX, set(), 'Vulkan core bring-up')

# vkCreateDevice: OptiScaler's hook and the present host's, each around its call down, only with NR on.
hooks = gated('OptiScaler/hooks/Vulkan_Hooks.cpp', 'o_vkCreateDevice(physicalDevice, &localCreateInfo', 1, GATE_DEVICE,
              set(), 'vkCreateDevice (OptiScaler)')
host = gated('retoque/dlssnr/DlssNr_VkPresentHost.cpp', 'o_CreateDevice(physical,', 1, GATE_DEVICE, set(),
             'vkCreateDevice (present host)')
nr_on = 'Config::Instance()->DlssNrEnabled.value_or_default(), "vkCreateDevice'
say(' '.join(hooks.split()).count(GATE_DEVICE + ' ' + nr_on) == 1, 'OptiScaler\'s hook gates only with [DlssNr] Enabled (NR off unchanged)')
say(' '.join(host.split()).count(GATE_DEVICE + ' ' + nr_on) == 1, 'the present host\'s hook gates only with [DlssNr] Enabled')
body = host[host.index('VkResult VKAPI_CALL hkCreateDevice('):]
body = body[:body.index('\n}\n')]
own = body.find('DlssNr::DriverInitGate::OwnCode ownCode;')
say(own >= 0 and own < body.find('HostActivity') and own < body.find('TransportLock') and
    own < body.find('g_registryMutex'),
    'the present host\'s hook lets an outer hook\'s hold go before its own code (OwnCode first, destroyed last)')
say(body.count('DlssNr::DriverInitGate::OwnCode') == 1 and 'OwnCode' not in hooks,
    'OwnCode only in the hook that runs below another of ours')

# Nothing else takes the gate, and its lines reach the log from one place.
users = sorted(str(p.relative_to(ROOT)) for d in ('OptiScaler', 'retoque') for p in (ROOT / d).rglob('*')
               if p.suffix in ('.cpp', '.h') and p != HEADER and 'DriverInitGate::' in p.read_text(errors='ignore'))
say(users == ['OptiScaler/hooks/Vulkan_Hooks.cpp', 'retoque/dlssnr/DlssNrFeature_Vk.cpp',
              'retoque/dlssnr/DlssNr_VkPresentHost.cpp', 'retoque/shaders/dlssnr/DlssNr_Dx12.cpp',
              'retoque/shaders/dlssnr/DlssNr_Dx12_Enlarge.cpp', 'retoque/shaders/dlssnr/DlssNr_Dx12_Model.cpp'],
    f'the gate is used only at the reviewed sites: {users}')
say(feature.count('DlssNr::DriverInitGate::SetReporter(') == 1, 'one reporter, set by the Vulkan model\'s unit')

# The locks the order names are taken only where they were when it was checked: a new site is reviewed against it
# (nr-vulkan-present-host.md, "The driver-init gate"). In particular no hook a driver or NGX call can reach takes them.
ACQUIRE = re.compile(r'\b(?:std::)?(?:lock_guard|unique_lock|scoped_lock)\b(?:<[^>;]*>)?\s+\w+\s*[({]\s*'
                     r'(g_preMutex|g_nrMutex|g_vkMutex|g_presentNgxMutex)\b')
found = {}
for d in ('OptiScaler', 'retoque'):
    for p in (ROOT / d).rglob('*'):
        if p.suffix in ('.cpp', '.h'):
            for m in ACQUIRE.finditer(strip(p.read_text(errors='ignore'))):
                found.setdefault(m.group(1), set()).add(str(p.relative_to(ROOT)))
EXPECTED = {
    'g_preMutex': {'retoque/shaders/dlssnr/DlssNr_Dx12.cpp'},
    'g_nrMutex': {'retoque/shaders/dlssnr/DlssNr_Dx12.cpp', 'retoque/shaders/dlssnr/DlssNr_Dx12_Model.cpp',
                  'retoque/shaders/dlssnr/DlssNr_Dx12_PresentGuides.cpp'},
    'g_vkMutex': {'retoque/dlssnr/DlssNrFeature_Vk.cpp'},
    'g_presentNgxMutex': {'retoque/dlssnr/DlssNrFeature_Vk.cpp'},
}
for name, files in EXPECTED.items():
    say(found.get(name, set()) == files, f'{name} is taken only in {sorted(files)} (found {sorted(found.get(name, []))})')

if failed:
    raise SystemExit(f'Failed: {len(failed)}: ' + '; '.join(failed[:12]))
print('PASS: the gate excludes, re-enters, suspends only device creates, is bounded; every site gated in order')

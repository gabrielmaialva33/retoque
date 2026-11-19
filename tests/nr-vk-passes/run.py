#!/usr/bin/env python3
"""Portable Vulkan chain decisions and owned creation adapter; no GPU."""
from pathlib import Path
import resource
import subprocess
import sys
import tempfile

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
with tempfile.TemporaryDirectory(prefix='nr-vk-passes-') as directory:
    binary = Path(directory) / 'cases'
    subprocess.run(['g++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-g',
                    '-I', str(ROOT / 'retoque/dlssnr'), str(HERE / 'cases.cpp'),
                    '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
    header = (ROOT / 'retoque/dlssnr/DlssNr_VkChain.h').read_text()
    mutants = [
        ('off path requires new observers', 'if (count == 1)', 'if (false)', 'Plan(1, route, {}).legacy'),
        ('missing submit observer admitted', 'if (!facts.completionObserved)', 'if (false)',
         'plan.refusal != Refusal::None'),
        ('time replaces completion', '_state == State::Complete && present > _present',
         'present > _present', '!creation.Ready(21)'),
        ('stale completion accepted', ' || serial != _serial', ' || serial == 0',
         '!creation.Complete(ticket, 6)'),
        ('reused handle accepted', ' && ticket.epoch == _ticket.epoch', '',
         '!creation.Complete({ 1, 2, 4 }, 7)'),
        ('invalidation preserves evidence', 'void Invalidate() { _state = State::Invalid; }',
         'void Invalidate() {}', '!creation.Ready(22)'),
        ('headroom omitted', 'std::max<uint64_t>(512ull << 20, budget / 5)', 'uint64_t(0)',
         '!Admit(1024 * MiB, 128 * MiB + 1, 0, 0)'),
        ('secondary creation admitted', 'ticket.level == VkRecording::Level::Primary', 'true',
         '!refused.Submitted(nonPrimary, 1)'),
        ('failure latch erased', 'return _failed && *_failed == settings;',
         'return _failed && *_failed != settings;', 'failure.Refused(tuning)'),
    ]
    for name, old, new, assertion in mutants:
        assert header.count(old) == 1, name
        (Path(directory) / 'DlssNr_VkChain.h').write_text(header.replace(old, new))
        subprocess.run(['g++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
                        '-I', directory, '-I', str(ROOT / 'retoque/dlssnr'),
                        str(HERE / 'cases.cpp'), '-o', str(binary)], check=True)
        result = subprocess.run([str(binary)], capture_output=True, text=True)
        assert result.returncode == -6 and assertion in result.stderr, (name, result.stderr)
        print(f'PASS: mutant rejected: {name}', flush=True)

# Deliberate boundary of slice 1: do not advertise/wire the planner as an available GPU chain.
source = (ROOT / 'retoque/dlssnr/DlssNrFeature_Vk.cpp').read_text()
after = source[source.index('void EvaluateAfterUpscaleVk('):source.index('// The pre-stage keeps')]
assert after.count('g_fwd.create(') == 1
assert after.count('g_fwd.evaluate(') == 1
assert 'cfg.DlssNrIntensity.value_or_default()' in after
assert 'DlssNr_VkChain.h' not in source
menu = (ROOT / 'retoque/dlssnr/DlssNr_Menu.cpp').read_text()
controls = menu[menu.index('static void RenderPassControls('):menu.index('static void RenderCadenceControls(')]
assert 'if (vulkan || config->DlssNrUseProxy.value_or_default())' in controls
assert controls.index('return;') < controls.index('GetDlssNrPassSnapshot()')
print('PASS: runtime and menu retain the single-pass boundary until budget and generation integration')

subprocess.run([sys.executable, str(HERE / 'submit_boundary.py')], check=True)
subprocess.run([sys.executable, str(HERE / 'budget_boundary.py')], check=True)

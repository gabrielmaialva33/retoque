"""Compile the entire scene control flow; substitute only its platform/renderer includes."""
from pathlib import Path
import os
import re
import subprocess

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
OUT = HERE / 'artifacts'
CASES = ['different-colours', 'same-colour', 'unbound', 'disabled-gap', 'aside', 'cold',
         'stage-gap', 'busy', 'concurrent-gap', 'fg-gap', 'fg-dx12', 'fg-vk', 'fg-mode',
         'fg-count', 'fg-request', 'fg-fsr', 'fg-input-game', 'fg-input', 'fg-output']


def compile_scene(source, name):
    folder = OUT / name
    folder.mkdir(parents=True, exist_ok=True)
    # All function bodies and global state are unchanged. Graphics types and renderer callbacks
    # come from scene.cpp; picture/geometry/policy headers are the actual production headers.
    text = re.sub(r'^#include[^\n]*\n', '', source, flags=re.M)
    (folder / 'scene-under-test.inc').write_text(text)
    exe = folder / 'scene'
    result = subprocess.run([os.environ.get('CXX', 'c++'), '-std=c++17', '-Wall', '-Wextra', '-Werror',
                             '-Wno-unused-variable', '-pthread', '-I' + str(folder), '-I' + str(ROOT / 'retoque'),
                             str(HERE / 'scene.cpp'), '-o', str(exe)], capture_output=True, text=True)
    assert result.returncode == 0, f'{name} failed to compile:\n{result.stderr}'
    return exe


def run_cases(source):
    exe = compile_scene(source, 'scene-baseline')
    failures = []
    for case in CASES:
        result = subprocess.run([str(exe), case], capture_output=True, text=True)
        if result.returncode:
            failures.append(f'{case}: {result.stderr.strip()}')
    assert not failures, '\n'.join(failures)
    print(f'PASS: {len(CASES)} production scene control-flow scenarios')


def run_mutants(source):
    mutants = [
        ('picture-output', 'Resource(params, NVSDK_NGX_Parameter_Color)',
         'Resource(params, NVSDK_NGX_Parameter_Output)', ['different-colours', 'same-colour']),
        ('unbound-admitted', 'if (!binding || binding->params != params || binding->key == 0)',
         'if (false)', ['unbound']),
        ('foreign-binding', 'binding->params != params || ', '', ['unbound']),
        ('zero-key', ' || binding->key == 0', '', ['unbound']),
        ('gap-forgotten', 'if (activated.load())', 'if (false)', ['disabled-gap', 'stage-gap']),
        ('reset-not-carried', 'frame.Reset |= missed.exchange(false);',
         '(void) missed.exchange(false);', ['disabled-gap']),
        ('aside-is-gap', '.role == Pictures::Role::Aside)\n        return;',
         '.role == Pictures::Role::Aside) { NoteGap(); return; }', ['aside']),
        ('concurrent-gap-erased', 'if (result != Dx11Scene::Transport::Result::Recorded)\n        missed = true;',
         'missed = result != Dx11Scene::Transport::Result::Recorded;', ['concurrent-gap']),
        ('fg-admitted', 'if (FrameGenerationActive())', 'if (FrameGenerationActive() && false)',
         [case for case in CASES if case.startswith('fg-')]),
    ]
    for name, old, new, cases in mutants:
        assert source.count(old) == 1, (name, old)
        exe = compile_scene(source.replace(old, new), 'scene-' + name)
        for case in cases:
            result = subprocess.run([str(exe), case], capture_output=True, text=True)
            assert result.returncode != 0 and 'Assertion' in result.stderr, (name, case, result.stderr)
        print(f'KILLED (compiled, runtime assertion): {name} [{", ".join(cases)}]')
    print(f'PASS: {len(mutants)} scene mutants compile and fail their scenario assertions')


if __name__ == '__main__':
    run_cases((ROOT / 'retoque/dlssnr/DlssNr_Dx11Scene.cpp').read_text())

#!/usr/bin/env python3
"""Production slices: Vulkan scope isolation and Reflex ownership/async expiry. No GPU."""
from pathlib import Path
import os
import re
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent


def block(source, anchor):
    start = source.rindex(anchor)
    opening = source.index('{', start)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == '{') - (source[end] == '}')
        end += 1
    return source[start:end]


state = (ROOT / 'OptiScaler/State.h').read_text()
hooks = (ROOT / 'OptiScaler/hooks/Vulkan_Hooks.cpp').read_text()
reflex = (ROOT / 'OptiScaler/hooks/Reflex_Hooks.cpp').read_text()
declaration = re.search(r'^    [^\n;]*bool vulkanCreatingSC = false;', state, re.M).group()
scope = block(state, 'class ScopedVulkanCreatingSC') + ';'
present = block(hooks, 'static VkResult hkvkQueuePresentKHR(')
assert present.count('ReflexHooks::update(') == 1
assert 'IsDxgiBackedCaller(DlssNr::VkPresentHost::OriginalPresentCaller(_ReturnAddress()))' in present
# Keep the complete statement after the menu's early return, including any guard.
update = present[present.index('}', present.index('if (!dxgiBacked && !MenuOverlayVk::QueuePresent')) + 1:
                 present.index('// dxvk/vkd3d-proton\'s go through')]
marker = block(reflex, 'NvAPI_Status ReflexHooks::processGameMarker(')
expiry = marker[marker.index('_updatesWithoutMarker = 0;'):marker.index('// RTSS reflex markers')]
# Both entrypoints must still establish the production RAII scope around their original calls.
for entry in ('static VkResult hkvkQueuePresentKHR(', 'static VkResult hkvkCreateSwapchainKHR('):
    assert 'ScopedVulkanCreatingSC scopedVulkanCreatingSC {};' in block(hooks, entry)

prefix = '''#include "pch.h"
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <semaphore>
#include <thread>
#include <cstdlib>
void require(bool ok, const char* message) {
    if (!ok) { std::fprintf(stderr, "FAIL: %s\\n", message); std::exit(1); }
}
class State {
public:
@DECL@
    static State& Instance() { static State value; return value; }
};
@SCOPE@
struct ReflexHooks {
    static inline int updates = 0;
    static void update(bool active, bool vulkan) {
        require(!active && vulkan, "native Vulkan update arguments changed"); ++updates;
    }
};
void presentUpdate(bool dxgiBacked) {
    (void) dxgiBacked;
@UPDATE@
}
struct Marker { uint64_t frameID; };
int _updatesWithoutMarker = 0;
uint64_t _lastAsyncMarkerFrameId = 0;
unsigned _FgNumFramesToGenerate = 3;
void processMarker(Marker* pSetLatencyMarkerParams) {
@EXPIRY@
}
'''
unit = (prefix.replace('@DECL@', declaration).replace('@SCOPE@', scope)
        .replace('@UPDATE@', update).replace('@EXPIRY@', expiry)
        + (HERE / 'cases.cpp').read_text())
flags = ['g++', '-std=c++20', '-O1', '-g', '-Wall', '-Wextra', '-Werror',
         '-fsanitize=address,undefined', '-fno-sanitize-recover=undefined', '-pthread', '-I', str(HERE)]
env = dict(os.environ, ASAN_OPTIONS='detect_leaks=0')
failures = []
with tempfile.TemporaryDirectory(prefix='proton-reflex-') as folder:
    folder = Path(folder)

    def build(name, source):
        cpp = folder / (name + '.cpp')
        cpp.write_text(source)
        binary = folder / name
        subprocess.run(flags + [str(cpp), '-o', str(binary)], check=True)
        return binary

    binary = build('production', unit)
    cases = ['nested', 'interleaved', 'dxgi', 'native', 'never-async', 'rtss', 'expiry']
    for case in cases:
        result = subprocess.run([str(binary), case], env=env, capture_output=True, text=True)
        print(f'{"PASS" if result.returncode == 0 else "FAIL"}: {case}', flush=True)
        if result.returncode:
            print(result.stderr)
            failures.append(case)
    if not failures:
        mutations = [
            ('global-scope', 'interleaved', declaration, '    bool vulkanCreatingSC = false;',
             'other thread inherited Vulkan scope'),
            ('duplicate-update', 'dxgi', update, '\nReflexHooks::update(false, true);\n',
             'DXGI-backed present updated Reflex again'),
            ('no-async-check', 'never-async', '_lastAsyncMarkerFrameId != 0 &&', '',
             'game without async markers lost FG count'),
            ('no-rtss-check', 'rtss', '!isRtssMarker &&', '', 'RTSS marker expired game async markers'),
        ]
        for name, case, old, new, reason in mutations:
            assert unit.count(old) == 1, name
            changed = unit.replace(old, new, 1)
            # Keep the now unused derived marker flag valid under -Werror.
            if name == 'no-rtss-check':
                changed = changed.replace('const bool isRtssMarker', '[[maybe_unused]] const bool isRtssMarker')
            mutant = build(name, changed)
            result = subprocess.run([str(mutant), case], env=env, capture_output=True, text=True)
            assert result.returncode != 0 and ('FAIL: ' + reason) in result.stderr, (name, result.stderr)
            print(f'PASS: rejected {name}: {reason}', flush=True)
if failures:
    raise SystemExit('Failed: ' + ', '.join(failures))

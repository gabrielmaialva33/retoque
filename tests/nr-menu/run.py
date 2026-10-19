#!/usr/bin/env python3
"""Compile actual pass-menu control flow with scripted ImGui events; no GPU/UI claims."""
from pathlib import Path
import subprocess
import tempfile
import re
import xml.etree.ElementTree as ET

root = Path(__file__).resolve().parents[2]
source = (root / 'retoque/dlssnr/DlssNr_Menu.cpp').read_text()
# The real presentation gate must precede every advanced section, after common status/profile lines.
menu = source[source.index('void RenderMenu('):]
assert 'if (RenderSimpleView(config, nativeVulkan, menuResScale))' in menu
assert menu.index('GameProfiles::Summary()') < menu.index('if (RenderSimpleView(') < menu.index('Localization::Tr("Source")')
assert re.search(r'if \(RenderSimpleView\(config, nativeVulkan, menuResScale\)\)\s*return;', menu)
start = source.index('static bool DeferredSlider(')
end = source.index('void RenderMenu(', start)
actual = source[start:end]
weight = source[source.index('static std::string WeightSetLabel('):source.index('// Whether peripheral compression')]
timing = source[source.index('static void RenderGpuTimingReadout('):source.index('// Historical confirmed samples')]
gpu = (root / 'retoque/dlssnr/DlssNr_GpuTiming.h').read_text()
gpu = gpu[gpu.index('struct Metadata'):gpu.index('// Collection never waits')]
gpu = 'namespace DlssNr::GpuTiming {\n' + gpu + '\nSnapshot snapshot; unsigned reads=0; Snapshot GetSnapshot() { ++reads; return snapshot; }\n}\n'
actual = weight + timing + actual
# Both branches reuse a single implementation; simple does not copy the advanced control body.
for helper in ('RenderDetailStrength(config);', 'RenderModelPreset(config);'):
    assert menu.count(helper) == 1
assert 'RenderPassControls(config, nativeVulkan);' in menu
assert 'RenderGpuTiming(config, nativeVulkan);' in menu
assert 'DlssNrShowAllSettings' not in (root / 'retoque/shaders/dlssnr/DlssNr_Dx12.cpp').read_text()
# A control writes a short Config transaction, never a public optional used by snapshots.
for field in ('Preset', 'Style', 'Intensity', 'LocalStructure', 'LocalTone', 'SkinStructure', 'AutoMask'):
    assert not re.search(r'config->DlssNr' + field + r'\s*=', source), field
    assert f'config->SetDlssNrMasterSetting(&Config::DlssNr{field},' in source, field
assert 'RetryAfterFailure' not in actual
# Register the runtime translation units, once, in both project files.
for name in ('OptiScaler.vcxproj', 'OptiScaler.vcxproj.filters'):
    tree = ET.parse(root / 'OptiScaler' / name)
    includes = [item.attrib.get('Include') for item in tree.iter()]
    for path in ('..\\retoque\\misc\\Localization.cpp', '..\\retoque\\dlssnr\\DlssNr_Submission.cpp'):
        assert includes.count(path) == 1, (name, path)
with tempfile.TemporaryDirectory(prefix='nr-menu-') as directory:
    generated = Path(directory) / 'menu.cpp'
    generated.write_text((root / 'tests/nr-menu/fakes.h').read_text() + '\n' + gpu + '\nnamespace DlssNr {\n' +
                         actual + '\n}\n' + (root / 'tests/nr-menu/cases.h').read_text())
    binary = Path(directory) / 'menu'
    subprocess.run(['g++', '-std=c++20', '-g', '-O1', '-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer', '-I', str(root / 'retoque'), '-I', str(root / 'OptiScaler'), str(generated), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
print('PASS: actual menu state transitions, short Config writes and project registration')

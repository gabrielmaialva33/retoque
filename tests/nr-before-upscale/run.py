#!/usr/bin/env python3
"""Compile actual pre-upscale boundary functions with strict host fakes and ASan/UBSan."""
from pathlib import Path
import subprocess
import sys
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / '_support'))
from nr_sources import renderer_with_internal_types as renderer_source  # noqa: E402  every DlssNr_Dx12*.cpp unit, in a fixed order

ROOT = Path(__file__).resolve().parents[2]
HERE = Path(__file__).resolve().parent
OUT = HERE / 'artifacts'
OUT.mkdir(exist_ok=True)
source = renderer_source()
header = (ROOT / 'retoque/dlssnr/DlssNrFeature_Dx12.h').read_text()


def between(text, start, end):
    assert text.count(start) == 1, f'instrumentation anchor changed: {start}'
    i = text.index(start)
    return text[i:text.index(end, i)]


declaration = between(header, 'class ScopedPreUpscale\n', '\n};') + '\n};\n'
allocation = between(source, 'ID3D12Resource* CreatePreUpscaleScratch(', '\nvoid Barrier(')
scope = between(source, 'ScopedPreUpscale::ScopedPreUpscale(', '// The pass. Resources in,')
compose = between(source, 'static bool EnsureCompose(', 'struct PresentList\n')
after = between(source, 'void EvaluateAfterUpscale(', '// ---------------------------------------------------------------------------------------------')
# The multi-view stand-down (DlssNr_MultiView.h). Before it existed every evaluate ran; that is the stub a source
# without it gets, so the cases below fail on the behaviour rather than on a missing anchor.
if source.count('// Several views in one colour allocation within a frame') == 1:
    views = between(source, '// Several views in one colour allocation within a frame', 'void EvaluateAfterUpscale(')
else:
    views = ('static DlssNr::MultiView::Detector s_multiViews;\n'
             'static const char* MultiViewStandDown(NVSDK_NGX_Parameter*, ID3D12Resource*) { return nullptr; }\n')
coverage = between(source, 'std::mutex g_coverageMutex;', '// Everything the model is reached through.')
resources = between(source, 'struct RestoreResourceState\n', '// The upscaler\'s own names differ')
internal = (ROOT / 'retoque/shaders/dlssnr/DlssNr_Dx12_Internal.h').read_text()
owner = between(internal, 'struct NrDeviceOwner\n', 'extern NrDeviceOwner')
resource_source = (ROOT / 'retoque/shaders/dlssnr/DlssNr_Dx12_Resources.cpp').read_text()
child_owner = between(resource_source, 'bool DeviceChildBelongsTo(ID3D12Device* device, ID3D12DeviceChild* child)',
                      '// Only the typeless guide families')  # both overloads: lists strict, resources under a wrapper
unit = '#include "fakes.h"\n' + owner + 'NrDeviceOwner g_nrDeviceOwner;\n' + child_owner + coverage + allocation + resources
spatial = between(source, 'void ReportSpatialContract(', 'DlssNrFrameInfo GatherGuideFrame(')
unit += '\nnamespace DlssNr {\n' + declaration + spatial + compose + views + after + scope + '\n}\n#include "cases.h"\n'
(OUT / 'boundary.cpp').write_text(unit)
subprocess.run(['g++', '-std=c++20', '-g', '-O1', '-fno-omit-frame-pointer',
                '-fsanitize=address,undefined', '-Wall', '-Wextra', '-Wno-unused-variable',
                '-I' + str(ROOT), '-I' + str(HERE), str(OUT / 'boundary.cpp'),
                '-o', str(OUT / 'boundary')], check=True)
subprocess.run([str(OUT / 'boundary')], check=True)

# The reduced-scale guide rules both stages share (docs/dlssnr/design/reduced-scale-guides.md): pure functions of
# sizes and flags, compiled straight from the production header.
subprocess.run(['g++', '-std=c++20', '-g', '-O1', '-fno-omit-frame-pointer',
                '-fsanitize=address,undefined', '-Wall', '-Wextra', '-Werror',
                '-I' + str(ROOT / 'retoque'), '-I' + str(ROOT / 'OptiScaler'), str(HERE / 'guide_match_cases.cpp'),
                '-o', str(OUT / 'guide_match')], check=True)
subprocess.run([str(OUT / 'guide_match')], check=True)

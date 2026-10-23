#!/usr/bin/env python3
"""Compile the actual render resolver and feature scope with poisoned-margin controls."""
from pathlib import Path
import subprocess
import tempfile
HERE=Path(__file__).resolve().parent
ROOT=HERE.parents[1]
with tempfile.TemporaryDirectory(prefix="nr-render-geometry-") as folder:
    binary=Path(folder)/"cases"
    subprocess.run(["g++","-std=c++20","-O1","-g","-Wall","-Wextra","-Werror",
                    "-fsanitize=address,undefined","-fno-omit-frame-pointer",
                    "-I",str(ROOT/"retoque"),"-I",str(ROOT/"OptiScaler"),
                    "-I",str(ROOT/"external/nvngx_dlss_sdk"),str(HERE/"cases.cpp"),"-o",str(binary)],check=True)
    subprocess.run([str(binary)],check=True)

# Pin the upstream integration: immutable snapshot before Init, success-only handle registration,
# release cleanup, and the same evaluate's scope before either NR stage.
hook=(ROOT/"OptiScaler/inputs/NVNGX_DLSS_Dx12.cpp").read_text()
create=hook[hook.index("NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D12_CreateFeature("):hook.index("NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D12_ReleaseFeature(")]
assert create.index("RenderGeometry::ReadCreation(InParameters)") < create.index("auto tryResult = TryCreateOptiFeature")
assert create.index("if (tryResult == NVSDK_NGX_Result_Success)") < create.index("RenderGeometry::Remember((*OutHandle)->Id, creation)")
assert "rememberGeometry ? DlssNr::RenderGeometry::ReadCreation" in create
release=hook[hook.index("NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D12_ReleaseFeature("):hook.index("NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D12_EvaluateFeature(")]
assert "RenderGeometry::Forget(handleId)" in release
evaluate=hook[hook.index("NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_D3D12_EvaluateFeature("):]
assert evaluate.index("RenderGeometry::Scope geometry(") < evaluate.index("DlssNr::ScopedPreUpscale")
assert "feature == NVSDK_NGX_Feature_SuperSampling ||" in evaluate
assert "feature == NVSDK_NGX_Feature_RayReconstruction" in evaluate
print("PASS: creation/evaluate/release integration guards")

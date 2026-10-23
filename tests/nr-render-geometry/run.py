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

    source=(ROOT/"retoque/shaders/dlssnr/DlssNr_Dx12.cpp").read_text()
    geometry=source[source.index("    const D3D12_RESOURCE_DESC guideDesc = depth->GetDesc();"):source.index("    // What the model is actually being shown")]
    references=source[source.index("    const unsigned int motionReferenceWidth ="):source.index("    if (peripheryActive)", source.index("    const unsigned int motionReferenceWidth ="))]
    harness=(HERE/"motion.cpp.in").read_text().replace("@GEOMETRY@",geometry).replace("@REFERENCES@",references)
    unit=Path(folder)/"motion.cpp"
    unit.write_text(harness)
    subprocess.run(["g++","-std=c++20","-O1","-g","-Wall","-Wextra","-Werror",
                    "-fsanitize=address,undefined","-I",str(ROOT/"retoque"),str(unit),"-o",str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
    inputs=(ROOT/"retoque/shaders/dlssnr/DlssNr_Dx12_Inputs.cpp").read_text()
    gather=inputs[inputs.index("DlssNrFrameInfo GatherGuideFrame("):inputs.index("DlssNrFrameInfo GatherFrame(")]
    unit.write_text((HERE/"gather.cpp.in").read_text().replace("@GATHER@",gather))
    subprocess.run(["g++","-std=c++20","-O1","-g","-Wall","-Wextra","-Werror",
                    "-fsanitize=address,undefined","-I",str(ROOT/"retoque"),"-I",str(HERE),
                    "-I",str(ROOT/"external/nvngx_dlss_sdk"),str(unit),"-o",str(binary)],check=True)
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

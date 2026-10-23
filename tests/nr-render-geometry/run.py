#!/usr/bin/env python3
"""Compile the actual render resolver and feature scope with poisoned-margin controls."""
from pathlib import Path
import subprocess
import tempfile
import re
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

    # Compile the production lifetime statements, retaining their position relative to backend Init.
    # Absent hooks on the baseline are no-ops; the real resolver/Gather/Dispatch refusal still runs.
    def body(text, signature):
        start = text.index(signature)
        begin = text.index("{", start)
        clean = re.sub(r'"(?:\\.|[^"\\])*"|//[^\n]*|/\*[\s\S]*?\*/',
                       lambda match: " " * len(match[0]), text)
        depth = 1
        end = begin + 1
        while depth:
            depth += (clean[end] == "{") - (clean[end] == "}")
            end += 1
        return text[start:end]

    functions = []
    for api, filename, symbol, create_name, factory in (
        ("D3D12", "Dx12", "D3D12", "CreateFeature", "auto tryResult = TryCreateOptiFeature"),
        ("D3D11", "Dx11", "D3D11", "CreateFeature", "FeatureProvider_Dx11::GetFeature("),
        ("Vulkan", "Vk", "VULKAN", "CreateFeature1", "FeatureProvider_Vk::GetFeature("),
    ):
        text = (ROOT / f"OptiScaler/inputs/NVNGX_DLSS_{filename}.cpp").read_text()
        create = body(text, f"NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_{symbol}_{create_name}(")
        evaluate = body(text, f"NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_{symbol}_EvaluateFeature(")
        release = body(text, f"NVSDK_NGX_API NVSDK_NGX_Result NVSDK_NGX_{symbol}_ReleaseFeature(")
        statements = []
        for pattern in (r"const bool rememberGeometry\s*=[\s\S]*?;",
                        r"const auto creation\s*=[\s\S]*?;",
                        r"DlssNr::RenderGeometry::Remember\([\s\S]*?;"):
            match = re.search(pattern, create)
            if match:
                statements.append((match.start(), match[0]))
        statements.append((create.index(factory), "NormalizeBackend(InParameters);"))
        statements.sort()
        snapshot = "\n".join(statement for _, statement in statements)
        scope = re.search(r"DlssNr::RenderGeometry::Scope geometry\([\s\S]*?;", evaluate)
        forget = re.search(r"DlssNr::RenderGeometry::Forget\([\s\S]*?;", release)
        functions.append(f"""
void Create{api}(FakeParams* InParameters, [[maybe_unused]] unsigned handleId) {{
    [[maybe_unused]] const auto& cfg = *Config::Instance();
    NVSDK_NGX_Handle handle {{ handleId }};
    auto* output = &handle;
    [[maybe_unused]] auto** OutHandle = &output;
    {snapshot}
}}
DlssNrFrameInfo Evaluate{api}(NVSDK_NGX_Parameter* InParameters, [[maybe_unused]] unsigned handleId) {{
    [[maybe_unused]] const auto& cfg = *Config::Instance();
    [[maybe_unused]] auto feature = NVSDK_NGX_Feature_SuperSampling;
    {scope[0] if scope else ""}
    return GatherGuideFrame(InParameters);
}}
void Release{api}([[maybe_unused]] unsigned handleId) {{ {forget[0] if forget else ""} }}
""")
        if api == "D3D11":
            # Init failure can still publish an accepted handle for deferred backend recovery.
            # Execute that return path's own Remember, not the normal success path's call.
            remembers = list(re.finditer(r"DlssNr::RenderGeometry::Remember\([\s\S]*?;", create))
            deferred = [(position, statement) for position, statement in statements
                        if not statement.startswith("DlssNr::RenderGeometry::Remember(")]
            if len(remembers) > 1:
                deferred.append((remembers[1].start(), remembers[1][0]))
            deferred.sort()
            deferred_snapshot = "\n".join(statement for _, statement in deferred)
            deferred_snapshot = deferred_snapshot.replace("const auto creation", "[[maybe_unused]] const auto creation")
            functions.append(f"""
void CreateDeferredD3D11(FakeParams* InParameters, [[maybe_unused]] unsigned handleId) {{
    {deferred_snapshot}
}}
""")
    guard = body(source, "    if (frame.GameGuides && (!frame.RenderSubrectWidth || !frame.RenderSubrectHeight))")
    unit.write_text((HERE/"inputs.cpp.in").read_text().replace("@GATHER@", gather)
                    .replace("@INPUTS@", "\n".join(functions)).replace("@GUARD@", guard))
    subprocess.run(["g++","-std=c++20","-O1","-g","-Wall","-Wextra","-Werror",
                    "-fsanitize=address,undefined","-I",str(ROOT/"retoque"),"-I",str(HERE),
                    "-I",str(ROOT/"external/nvngx_dlss_sdk"),str(unit),"-o",str(binary)],check=True)
    results = [subprocess.run([str(binary), api]).returncode for api in ("D3D11", "Vulkan", "all")]
    assert results == [0, 0, 0], f"input lifetime controls failed (D3D11/Vulkan/isolation): {results}"

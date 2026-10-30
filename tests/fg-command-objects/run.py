#!/usr/bin/env python3
"""FG D3D12 object lifetime after the HelixSR fixes: partial command-object creation and the HUD-less format gate,
sliced verbatim from production; source guards on DLSS-G's restore barrier and on the shared FG fences' ownership."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
FG = ROOT / "OptiScaler/framegen"
base_header = (FG / "IFGFeature_Dx12.h").read_text()
base = (FG / "IFGFeature_Dx12.cpp").read_text()
backends = {
    "DLSSG": (FG / "dlssg/DLSSG_Dx12.cpp").read_text(),
    "XeFG": (FG / "xefg/XeFG_Dx12.cpp").read_text(),
    "FSRFG": (FG / "ffx/FSRFG_Dx12.cpp").read_text(),
    "Reprojection": (FG / "reprojection/Reprojection_Dx12.cpp").read_text(),
}


def block(text, start):
    """From start through the brace that closes the first brace after it."""
    open_brace = text.index("{", start)
    depth, end = 1, open_brace + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[start:end]


def function(text, signature):
    return block(text, text.index(signature))


failures = []

# Behaviour: the members, the getters and both CreateObjects, verbatim
members = base_header[base_header.index("    ID3D12GraphicsCommandList* _scCommandList[BUFFER_COUNT]") :
                      base_header.index("    std::unordered_map<FG_ResourceType, Dx12Resource> _frameResources")]
parts = [function(base, "bool IFGFeature_Dx12::" + name + "(") for name in ("WaitForUIAllocator", "SubmitUICommandList")]
parts += [function(base, "ID3D12GraphicsCommandList* IFGFeature_Dx12::" + name + "(")
          for name in ("GetUICommandList", "GetSCCommandList")]
if "bool IFGFeature_Dx12::DropIncompleteCommandObjects(" in base:
    parts.append(function(base, "bool IFGFeature_Dx12::DropIncompleteCommandObjects("))
for name in ("DLSSG", "XeFG"):
    parts.append(function(backends[name], f"void {name}_Dx12::CreateObjects("))
    source = backends[name]
    marker = source.index("static DXGI_FORMAT lastFormat")
    gate = block(source, source.rindex("if (type == FG_ResourceType::HudlessColor)", 0, marker))
    parts.append(f"bool HudlessGate_{name}(FG_ResourceType type, FrameResource* fResource, int fIndex)\n{{\n"
                 f"    {gate}\n    return true;\n}}\n")

# Guard: DLSS-G never moves a COPY_SOURCE input to COPY_DEST (CopyResource restores its source itself), so its
# SetResource has no restore barrier, which also ran on a missing command list for a resource valid until present
dlssg_set = function(backends["DLSSG"], "bool DLSSG_Dx12::SetResource(")
if "ResourceBarrier(" in dlssg_set:
    failures.append("DLSSG_Dx12::SetResource records a barrier on the game's input (49a25a3d9a removed the "
                    "COPY_DEST->COPY_SOURCE restore: wrong before-state, possibly on a null command list)")

# Guard: the HelixSR fence release on destruction (fba8eb77d7) does not apply while FG objects live for the process:
# the shared UI/SC fences and events are created once per object and reused across ReleaseObjects/CreateObjects.
production = [p for root in ("OptiScaler", "retoque") for p in (ROOT / root).rglob("*") if p.suffix in (".cpp", ".h")
              and "external" not in p.parts and "include" not in p.relative_to(ROOT / root).parts[:1]]
for path in production:
    text = path.read_text(errors="replace")
    if re.search(r"delete\s+[^;]*\bcurrentFG\b|[.>]currentFG\s*=\s*nullptr|\bcurrentFG\.reset\(", text):
        failures.append(f"{path.relative_to(ROOT)} destroys the FG object: port fba8eb77d7 first (wait for the UI "
                        "fence, release only proven-complete fences, leak and log otherwise)")
shared = re.compile(r"\b(_uiFence|_scFence|_uiFenceEvent|_scFenceEvent)\b")
for name, source in backends.items():
    release = function(source, f"void {name}_Dx12::ReleaseObjects(")
    if shared.search(release):
        failures.append(f"{name}_Dx12::ReleaseObjects touches the shared fences: they would leak or be freed in use")
    create = function(source, f"void {name}_Dx12::CreateObjects(")
    guarded = len(re.findall(r"if \((_uiFence|_scFence) == nullptr\)", create))
    if create.count("CreateFence(") != guarded:
        failures.append(f"{name}_Dx12::CreateObjects creates a fence without reusing the existing one")

fakes = (HERE / "fakes.h").read_text().replace("    // @command-object-members@\n", members)
unit = fakes + "\n".join(parts) + (HERE / "cases.cpp").read_text()
with tempfile.TemporaryDirectory(prefix="retoque-fg-command-objects-") as folder:
    cpp, binary = Path(folder) / "cases.cpp", Path(folder) / "cases"
    cpp.write_text(unit)
    subprocess.run(["g++", "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                    "-Wno-unused-variable", "-Wno-unused-but-set-variable", "-Wno-sign-compare",
                    "-fsanitize=address,undefined",
                    "-fno-omit-frame-pointer", str(cpp), "-o", str(binary)], check=True)
    if subprocess.run([str(binary)], timeout=120).returncode != 0:
        failures.append("the command-object and format cases failed (see above)")

for failure in failures:
    print("FAIL:", failure, file=sys.stderr)
sys.exit(1 if failures else 0)

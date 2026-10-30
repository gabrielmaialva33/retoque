#!/usr/bin/env python3
"""Compile the D3D12 upscalers' shared legacy menu code, sliced verbatim from production, and race a draw, a creation
and destructors against it; then check every touch of the shared menu is under its lock."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
header = (ROOT / "OptiScaler/upscalers/IFeature_Dx12.h").read_text()
source = (ROOT / "OptiScaler/upscalers/IFeature_Dx12.cpp").read_text()


def function(text, signature):
    begin = text.index(signature)
    start = text.index("{", begin)
    depth, end = 1, start + 1
    while depth:
        depth += (text[end] == "{") - (text[end] == "}")
        end += 1
    return text[begin:end]


# The shared menu and whatever the header declares with it, verbatim
declarations = header[header.index("    static inline std::unique_ptr<Menu_Dx12> Imgui") :
                      header.index("    std::unique_ptr<OS_Dx12> OutputScaler")]
functions = {name: function(source, signature) for name, signature in (
    ("Init", "bool IFeature_Dx12::Init("),
    ("RenderLegacyMenu", "void IFeature_Dx12::RenderLegacyMenu("),
    ("destructor", "IFeature_Dx12::~IFeature_Dx12()"),
)}

failures = []

# Source guards: every use of the shared menu in the file sits in one of these functions, after its lock
uses = [m.start() for m in re.finditer(r"\bImgui\b", source)]
for position in uses:
    owner = next((name for name, body in functions.items()
                  if source.index(body) <= position < source.index(body) + len(body)), None)
    if owner is None:
        line = source.count("\n", 0, position) + 1
        failures.append(f"IFeature_Dx12.cpp:{line} touches the shared menu outside Init/RenderLegacyMenu/destructor")
for name, body in functions.items():
    lock = re.search(r"std::(lock_guard|scoped_lock|unique_lock)\b[^;]*\(\s*ImguiMutex\s*\)", body)
    first_use = re.search(r"\bImgui\b", body)
    if lock is None or (first_use is not None and first_use.start() < lock.start()):
        failures.append(f"{name} must take ImguiMutex before it touches the shared menu")
if "ImguiMutex" not in declarations or "order" not in declarations.lower():
    failures.append("IFeature_Dx12.h declares ImguiMutex beside the menu and states its lock order")

fakes = (HERE / "fakes.h").read_text().replace("    // @menu-declarations@\n", declarations)
unit = fakes + "\n".join(functions.values()) + (HERE / "cases.cpp").read_text()
with tempfile.TemporaryDirectory(prefix="retoque-upscaler-menu-lock-") as folder:
    cpp, binary = Path(folder) / "cases.cpp", Path(folder) / "cases"
    cpp.write_text(unit)
    subprocess.run(["g++", "-std=c++20", "-O1", "-g", "-pthread", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-parameter", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", str(cpp),
                    "-o", str(binary)], check=True)
    if subprocess.run([str(binary)], timeout=120).returncode != 0:
        failures.append("the race cases failed (see above)")

for failure in failures:
    print("FAIL:", failure, file=sys.stderr)
sys.exit(1 if failures else 0)

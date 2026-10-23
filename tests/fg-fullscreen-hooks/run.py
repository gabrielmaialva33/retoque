#!/usr/bin/env python3
"""Compile the FG swapchain's production fullscreen hooks against DXGI fakes and run the cases."""
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
source = (ROOT / "OptiScaler/hooks/FG_Hooks.cpp").read_text()


def between(start_marker, end_marker, start=0):
    begin = source.index(start_marker, start)
    return source[begin:source.index(end_marker, begin)]


parts = [
    between("HRESULT FGHooks::hkSetFullscreenState(", "HANDLE FGHooks::hkGetFrameLatencyWaitableObject("),
    # hkGetFullscreenDesc and hkGetFullscreenState, which follows it
    between("HRESULT FGHooks::hkGetFullscreenDesc(", "HRESULT FGHooks::hkResizeBuffers("),
    between("HRESULT FGHooks::hkResizeTarget(", "HRESULT FGHooks::hkResizeBuffers1("),
]
# The window cover at the end of both resize hooks, as functions of the resize's result
for hook, name in (("hkResizeBuffers(", "CoverAfterResizeBuffers"), ("hkResizeBuffers1(", "CoverAfterResizeBuffers1")):
    body = source.index("HRESULT FGHooks::" + hook)
    block = between("    // Resize window to cover the screen", "    return result;\n}", body)
    parts.append("void FGHooks::" + name + "(HRESULT result)\n{\n" + block + "}\n")
unit = (HERE / "fakes.h").read_text() + "\n".join(parts) + (HERE / "cases.cpp").read_text()
with tempfile.TemporaryDirectory(prefix="retoque-fg-fullscreen-hooks-") as folder:
    cpp, binary = Path(folder) / "cases.cpp", Path(folder) / "cases"
    cpp.write_text(unit)
    subprocess.run(["g++", "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                    "-Wno-unused-function", "-Wno-unused-variable", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", str(cpp),
                    "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

#!/usr/bin/env python3
"""Compile both production FG resize flag transactions against strict DXGI fakes."""
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
source = (ROOT / "OptiScaler/hooks/FG_Hooks.cpp").read_text()
parts = []
for name in ("hkResizeBuffers", "hkResizeBuffers1"):
    start = source.index("HRESULT FGHooks::" + name + "(")
    start = source.index("    if (State::Instance().activeFgOutput == FGOutput::XeFG)", start)
    end = source.index("    auto fgDx12 =", start)
    parts.append("void " + name + "(IDXGISwapChain* This, UINT& SwapChainFlags) {\n"
                 "UINT BufferCount=2, Width=1280, Height=720, NewFormat=0, Format=0;\n" + source[start:end] + "\n}")
unit = (HERE / "fakes.h").read_text() + "\n".join(parts) + (HERE / "cases.cpp").read_text()
with tempfile.TemporaryDirectory(prefix="retoque-fg-resize-flags-") as folder:
    cpp, binary = Path(folder) / "cases.cpp", Path(folder) / "cases"
    cpp.write_text(unit)
    subprocess.run(["g++", "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-variable", "-Wno-unused-parameter", "-fsanitize=address,undefined",
                    "-fno-omit-frame-pointer", str(cpp), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

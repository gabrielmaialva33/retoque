#!/usr/bin/env python3
"""Compile FGPresent's production DLSS-G block against Streamline/DXGI fakes and run the matrix."""
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
source = (ROOT / "OptiScaler/hooks/FG_Hooks.cpp").read_text()
present = source.index("HRESULT FGHooks::FGPresent(")
start = source.index("    if (willPresent && fgFeatureActive && state.activeFgOutput == FGOutput::DLSSG)\n", present)
end = source.index("    if (willPresent && fgFeatureActive)\n    {", start)
# FGPresent's locals the block reads, then the block itself
wrapper = ("void DlssgPresentStart(IDXGISwapChain* This, bool willPresent, bool fgFeatureActive, "
           "sl::FrameToken*& localToken, sl::Result& tokenResult)\n{\n"
           "    auto& state = State::Instance();\n    auto config = Config::Instance();\n    auto fg = &feature;\n"
           + source[start:end] + "}\n")
unit = (HERE / "fakes.h").read_text() + wrapper + (HERE / "cases.cpp").read_text()
with tempfile.TemporaryDirectory(prefix="retoque-fg-dlssg-backbuffer-index-") as folder:
    cpp, binary = Path(folder) / "cases.cpp", Path(folder) / "cases"
    cpp.write_text(unit)
    subprocess.run(["g++", "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                    "-Wno-unused-variable", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", str(cpp),
                    "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

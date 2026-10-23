#!/usr/bin/env python3
"""Compile production ReadableGuide; observe copies and retirement under ASan/UBSan."""
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
source = (ROOT / "retoque/shaders/dlssnr/DlssNr_Dx12_Resources.cpp").read_text()
start = source.index("ID3D12Resource* ReadableGuide(")
end = source.index("// The upscaler", start)
unit = (HERE / "fakes.h").read_text() + source[start:end] + (HERE / "cases.cpp").read_text()
with tempfile.TemporaryDirectory(prefix="retoque-guide-copy-contract-") as folder:
    cpp, binary = Path(folder) / "cases.cpp", Path(folder) / "cases"
    cpp.write_text(unit)
    subprocess.run(["g++", "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer", str(cpp), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

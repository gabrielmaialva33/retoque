#!/usr/bin/env python3
"""Run dllmain's [Libraries] NvngxDlssPath block on real folders and check what the NGX feature paths get from it."""
from pathlib import Path
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]

source = (ROOT / "OptiScaler/dllmain.cpp").read_text()
start = source.index("        if (Config::Instance()->NVNGX_DLSS_Library.has_value())\n")
end = source.index("        if (!State::Instance().NVNGX_DLSS_Path.has_value())\n", start)
block = source[start:end]

unit = (HERE / "fakes.h").read_text() + "\nstatic void ResolveOverride()\n{\n" + block + "}\n" + (
    HERE / "cases.cpp").read_text()
with tempfile.TemporaryDirectory(prefix="retoque-dlss-library-path-") as folder:
    cpp, binary = Path(folder) / "cases.cpp", Path(folder) / "cases"
    cpp.write_text(unit)
    subprocess.run(["g++", "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                    "-fno-omit-frame-pointer", str(cpp), "-o", str(binary)], check=True)
    subprocess.run([str(binary), str(Path(folder) / "games")], check=True)

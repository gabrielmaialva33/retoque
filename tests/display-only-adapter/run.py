#!/usr/bin/env python3
"""retoque/misc/DisplayOnlyAdapter.h against a fake D3DKMT, and IdentifyGpu's enumeration checked to call it."""
from pathlib import Path
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]

source = (ROOT / "OptiScaler/misc/IdentifyGpu.cpp").read_text()
loop = source.index("while (factory->EnumAdapterByGpuPreference(")
skip = source.find("isDisplayOnlyAdapter(adapterDesc.AdapterLuid)", loop)
store = source.find("localCachedInfo.push_back(", loop)
problems = []
if skip < 0 or store < 0 or skip > store:
    problems.append("IdentifyGpu.cpp: the enumeration does not skip display-only adapters before storing one")

with tempfile.TemporaryDirectory(prefix="retoque-display-only-adapter-") as folder:
    binary = Path(folder) / "cases"
    subprocess.run(["g++", "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                    "-fno-omit-frame-pointer", "-I", str(HERE / "fake_include"), "-I", str(ROOT / "retoque"),
                    str(HERE / "cases.cpp"), "-o", str(binary)], check=True)
    result = subprocess.run([str(binary)])

for p in problems:
    print("FAIL: " + p)
sys.exit(1 if problems or result.returncode != 0 else 0)

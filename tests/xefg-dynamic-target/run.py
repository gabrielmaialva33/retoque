#!/usr/bin/env python3
"""Run XeFG Dynamic MFG's production target and V-Sync signal against host fakes."""
from pathlib import Path
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
XEFG = ROOT / "OptiScaler/framegen/xefg"


def function(source, anchor):
    start = source.index(anchor)
    depth, end = 1, source.index("{", start) + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


target = function((XEFG / "XeFG_DynamicMFG.cpp").read_text(), "float XeFG_DynamicMFG::TargetFps(double nowMs, HWND window)")

# FGPresent from its ForceVsync block to where it records the final sync interval
hooks = (ROOT / "OptiScaler/hooks/FG_Hooks.cpp").read_text()
present = hooks[hooks.index("HRESULT FGHooks::FGPresent(") :]
start = present.index("    if (willPresent && config->ForceVsync.has_value())")
store = present.find("state.fgPresentVsync.store(", start)
signal = ""
missing = []
if store < 0:
    missing.append("FGPresent does not record its final sync interval (State::fgPresentVsync)")
else:
    signal = ("bool PresentVsync(UINT SyncInterval, UINT Flags, bool willPresent)\n{\n"
              "    auto& state = State::Instance();\n    auto config = Config::Instance();\n"
              + present[start : present.index("\n", store) + 1]
              + "    (void) Flags;\n    return state.fgPresentVsync.load();\n}\n")

unit = (HERE / "fakes.h").read_text() + "\n" + target + "\n" + signal + (HERE / "cases.cpp").read_text()
with tempfile.TemporaryDirectory(prefix="retoque-xefg-dynamic-target-") as folder:
    cpp, binary = Path(folder) / "cases.cpp", Path(folder) / "cases"
    cpp.write_text(unit)
    flags = ["-DHAVE_PRESENT_SLICE"] if signal else []
    subprocess.run(["g++", "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                    "-fno-sanitize-recover=all", "-fno-omit-frame-pointer", "-I", str(XEFG), *flags, str(cpp), "-o",
                    str(binary)], check=True)
    result = subprocess.run([str(binary)])

for line in missing:
    print("FAIL: " + line)
sys.exit(1 if missing or result.returncode != 0 else 0)

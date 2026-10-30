#!/usr/bin/env python3
"""The menu's key polling reads physical key state beneath other overlays' GetAsyncKeyState detours."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
INPUT = ROOT / "OptiScaler/menu/input"


def function(source, anchor):
    start = source.index(anchor)
    depth, end = 1, source.index("{", start) + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start:end]


problems = []
detours = (INPUT / "input_system_detours.cpp").read_text()
if "#include <menu/input/native_key_state.h>" not in detours:
    problems.append("input_system_detours.cpp does not include retoque/menu/input/native_key_state.h")

# Every poll the menu makes of a key or mouse button goes through RealGetAsyncKeyStateSafe
system = (INPUT / "input_system.cpp").read_text()
for anchor in ("void PollVirtualKeyLocked(int vk, DWORD time)", "bool PollMouseButtonLocked(int vk, int button, DWORD time)"):
    if "RealGetAsyncKeyStateSafe(vk)" not in function(system, anchor):
        problems.append(anchor + " no longer polls through RealGetAsyncKeyStateSafe")
if re.search(r"(?<![\w:])(?:::)?GetAsyncKeyState\(", system):
    problems.append("input_system.cpp polls GetAsyncKeyState directly, through the detour chain")

safe = function(detours, "SHORT RealGetAsyncKeyStateSafe(int vk)")
unit = (HERE / "cases.cpp").read_text() + "\nnamespace OptiInput\n{\n" + safe + "\n}\n" + (HERE / "main.cpp").read_text()

results = []
with tempfile.TemporaryDirectory(prefix="retoque-menu-native-key-state-") as folder:
    cpp, binary = Path(folder) / "cases.cpp", Path(folder) / "cases"
    cpp.write_text(unit)
    build = subprocess.run(["g++", "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=address,undefined", "-fno-sanitize-recover=all", "-fno-omit-frame-pointer",
                            "-I", str(HERE / "fake_include"), "-I", str(ROOT / "retoque"), str(cpp), "-o", str(binary)])
    if build.returncode != 0:
        problems.append("the production slice and header did not compile against the fakes")
    else:
        # One process per case: the reader caches what win32u exports
        results = [subprocess.run([str(binary), case]).returncode for case in ("native", "missing")]

for p in problems:
    print("FAIL: " + p)
sys.exit(1 if problems or any(results) else 0)

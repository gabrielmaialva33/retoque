#!/usr/bin/env python3
"""retoque/misc/InstanceClaim.h against a fake Win32, and dllmain's claim block sliced from production."""
from pathlib import Path
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]

dllmain = (ROOT / "OptiScaler/dllmain.cpp").read_text()
start = dllmain.index("        CheckForExcludedProcess();\n")
end = dllmain.index("        if (_passThruMode)\n        {", start)
block = dllmain[start:end]

# The first copy reports the others where it logs the GPU list, after every statically imported copy attached
identify = (ROOT / "OptiScaler/misc/IdentifyGpu.cpp").read_text()
gpu_list = identify.index("    spdlog::info(gpus);\n")
report = identify.find("InstanceClaim::InertCopies()", gpu_list)
problems = []
if report < 0 or report - gpu_list > 400:
    problems.append("IdentifyGpu.cpp: no InstanceClaim::InertCopies() report right after the GPU list is logged")

unit = f"""#include <misc/InstanceClaim.h>
#include <cstdio>
#include <filesystem>

static bool _passThruMode = false;
static bool g_excluded = false;
static std::wstring g_dllPath = L"C:\\\\game\\\\dxgi.dll";
static void CheckForExcludedProcess() {{ if (g_excluded) _passThruMode = true; }}
namespace Util {{ inline std::filesystem::path DllPath() {{ return g_dllPath; }} }}

// dllmain's attach from CheckForExcludedProcess() up to the pass-through branch, as shipped
static bool Attach(const wchar_t* path, bool excluded)
{{
    _passThruMode = false;
    g_excluded = excluded;
    g_dllPath = path;
{block}    return _passThruMode;
}}
""" + (HERE / "cases.cpp").read_text()

with tempfile.TemporaryDirectory(prefix="retoque-instance-claim-") as folder:
    cpp, binary = Path(folder) / "cases.cpp", Path(folder) / "cases"
    cpp.write_text(unit)
    subprocess.run(["g++", "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                    "-fno-omit-frame-pointer", "-I", str(HERE / "fake_include"), "-I", str(ROOT / "retoque"), str(cpp),
                    "-o", str(binary)], check=True)
    result = subprocess.run([str(binary)])

for p in problems:
    print("FAIL: " + p)
sys.exit(1 if problems or result.returncode != 0 else 0)

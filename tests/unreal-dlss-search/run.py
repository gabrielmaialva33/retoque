#!/usr/bin/env python3
"""retoque/misc/UnrealDlssSearch.h on real folders, and dllmain's block using it sliced and run against fakes."""
from pathlib import Path
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
FLAGS = ["g++", "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
         "-fno-omit-frame-pointer", "-I", str(ROOT / "retoque")]

FAKES = """#include <misc/UnrealDlssSearch.h>
#include <cstdio>
#include <fstream>
#include <optional>
#include <string>
#include <utility>

static int logged = 0;
#define LOG_INFO(...) (void) ++logged

struct FakeState
{
    std::optional<std::wstring> NVNGX_DLSS_Path, NVNGX_DLSSD_Path, NVNGX_DLSSG_Path;
};
struct State
{
    static FakeState& Instance()
    {
        static FakeState state;
        return state;
    }
};
"""

problems = []
source = (ROOT / "OptiScaler/dllmain.cpp").read_text()
if "#include <misc/UnrealDlssSearch.h>" not in source:
    problems.append("dllmain.cpp does not include misc/UnrealDlssSearch.h")
last_search = source.find('Util::FindFilePath(exePath, "nvngx_dlssg.dll");')
start = source.find("        // Unreal Engine plugin folders", max(last_search, 0))
decision = source.find('"nvngx_dlss.dll not found, disabling DLSS"')
block = None
if last_search < 0 or start < 0 or decision < 0 or not last_search < start < decision:
    problems.append("dllmain.cpp: no Unreal search after the existing searches and before the DLSS decision")
else:
    end = source.index("\n        }\n", start) + len("\n        }\n")
    block = source[start:end]
    if "UnrealDlssSearch::Find(exePath" not in block:
        problems.append("dllmain.cpp: the sliced block does not call UnrealDlssSearch::Find from the exe folder")

results = []
with tempfile.TemporaryDirectory(prefix="retoque-unreal-dlss-search-") as folder:
    folder = Path(folder)
    games = folder / "games"
    games.mkdir()
    subprocess.run(FLAGS + [str(HERE / "cases.cpp"), "-o", str(folder / "cases")], check=True)
    results.append(subprocess.run([str(folder / "cases"), str(games)]).returncode)
    if block is not None:
        unit = (FAKES + "\nstatic void RunUnrealSearch(const std::filesystem::path& exePath)\n{\n" + block + "}\n\n" +
                (HERE / "wiring.cpp").read_text())
        (folder / "wiring.cpp").write_text(unit)
        subprocess.run(FLAGS + [str(folder / "wiring.cpp"), "-o", str(folder / "wiring")], check=True)
        results.append(subprocess.run([str(folder / "wiring"), str(games)]).returncode)

for p in problems:
    print("FAIL: " + p)
sys.exit(1 if problems or any(results) else 0)

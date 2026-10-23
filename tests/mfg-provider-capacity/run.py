#!/usr/bin/env python3
"""Exercise both production SL hooks with real SDK, CustomOptional and INI serialization."""
from pathlib import Path
import argparse
import re
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
def function(source, anchor):
    begin = source.index(anchor)
    opening = source.index("{", begin)
    depth, end = 1, opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[begin:end]
header = (ROOT / "OptiScaler/Config.h").read_text()
config = (ROOT / "OptiScaler/Config.cpp").read_text()
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--baseline", help="read only the hooks from a pinned git ref for a regression control")
args = parser.parse_args()
hooks = (subprocess.check_output(["git", "-C", str(ROOT), "show",
                                  args.baseline + ":OptiScaler/hooks/Streamline_Hooks.cpp"], text=True)
         if args.baseline else (ROOT / "OptiScaler/hooks/Streamline_Hooks.cpp").read_text())
optional = header[header.index("enum HasDefaultValue"):header.index("constexpr inline int UnboundKey")]
read = re.search(r'FGDLSSGOverrideInterpolationCount.set_from_config\(readInt\("DLSSG", "OverrideInterpolationCount"\)\);', config).group()
save = re.search(r'ini.SetValue\("DLSSG", "OverrideInterpolationCount",.*?;', config, re.S).group()
unit = (HERE / "fakes.h").read_text().replace("// @optional@", optional)
unit = unit.replace("// @read@", read).replace("// @save@", save)
unit = unit.replace("// @serializer@", function(config, "template <typename T> std::string GetIntValue("))
unit += function(config, "std::optional<int> Config::readInt(")
for name in ("g_dlssgMfgGeneration", "g_dlssgMfgGetter"):
    declaration = re.search(r"static std::atomic<[^;]+ " + name + r"[^;]+;", hooks)
    if declaration:
        unit += declaration.group()
if "static void UpdateDlssgMfgMax(" in hooks:
    unit += function(hooks, "static void UpdateDlssgMfgMax(")
if "static void RefreshDlssgMfgMax(" in hooks:
    unit += function(hooks, "static void RefreshDlssgMfgMax(")
for name in ("hkslDLSSGSetOptions", "hkslDLSSGGetState"):
    unit += function(hooks, "sl::Result StreamlineHooks::" + name + "(")
unit += (HERE / "cases.cpp").read_text()
cases = ("setter-preference", "getter-preference", "setter-refresh", "getter-refresh", "downgrade",
         "query-failure", "old-structs", "menu-transitions", "stable-queries", "setter-rebind", "game-get-free", "patch-during-probe", "auto-off")
with tempfile.TemporaryDirectory(prefix="retoque-mfg-capacity-") as folder:
    cpp, binary = Path(folder) / "cases.cpp", Path(folder) / "cases"
    cpp.write_text(unit)
    subprocess.run(["g++", "-std=c++23", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-variable", "-Wno-unused-parameter", "-Wno-class-memaccess", "-Wno-narrowing",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    "-isystem", str(ROOT / "external/streamline"), "-isystem", str(ROOT / "external/simpleini"),
                    str(cpp), "-o", str(binary)], check=True)
    results = [subprocess.run([str(binary), case]).returncode for case in cases]
    if any(results):
        raise SystemExit(1)

# Compile the real generation publisher/accessor separately, without Windows patching dependencies.
patcher = (ROOT / "retoque/framegen/dlssg/MfgUnlock.cpp").read_text()
publisher = "#include <atomic>\n#include <cstdint>\n#include <cassert>\n#include <iostream>\nusing HMODULE = void*;\n"
for name in ("g_mfgProviderGeneration", "g_mfgLastProvider"):
    publisher += re.search(r"std::atomic<[^>]+> " + name + r"[^;]+;", patcher).group() + "\n"
publisher += function(patcher, "static void PublishProviderGeneration(")
publisher += "namespace MfgUnlock { uint64_t ProviderGeneration(); }\n"
publisher += function(patcher, "uint64_t MfgUnlock::ProviderGeneration(")
publisher += r"""
int main()
{
    int first=0, second=0;
    assert(MfgUnlock::ProviderGeneration()==0);
    PublishProviderGeneration(nullptr,false);
    assert(MfgUnlock::ProviderGeneration()==0);
    PublishProviderGeneration(&first,false);
    assert(MfgUnlock::ProviderGeneration()==1);
    PublishProviderGeneration(&first,false);
    assert(MfgUnlock::ProviderGeneration()==1);
    PublishProviderGeneration(&first,true);
    assert(MfgUnlock::ProviderGeneration()==2);
    PublishProviderGeneration(&second,false);
    assert(MfgUnlock::ProviderGeneration()==3);
    PublishProviderGeneration(nullptr,true);
    assert(MfgUnlock::ProviderGeneration()==4);
    std::cout << "PASS: production generation publisher (new/repeated provider, patch and wrapper)\n";
}
"""
with tempfile.TemporaryDirectory(prefix="retoque-mfg-generation-") as folder:
    cpp, binary = Path(folder) / "cases.cpp", Path(folder) / "cases"
    cpp.write_text(publisher)
    subprocess.run(["g++", "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                    str(cpp), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

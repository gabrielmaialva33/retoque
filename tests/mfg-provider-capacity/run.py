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
if "static void UpdateDlssgMfgMax(" in hooks:
    unit += function(hooks, "static void UpdateDlssgMfgMax(")
for name in ("hkslDLSSGSetOptions", "hkslDLSSGGetState"):
    unit += function(hooks, "sl::Result StreamlineHooks::" + name + "(")
unit += (HERE / "cases.cpp").read_text()
cases = ("setter-preference", "getter-preference", "setter-refresh", "getter-refresh", "downgrade",
         "query-failure", "old-structs", "menu-transitions", "auto-off")
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

#!/usr/bin/env python3
"""Compile CheckForRealObject and the three DLSS-G factory sites against a Streamline-shaped proxy and run the
matrix, then check that no caller anywhere releases the borrowed object CheckForRealObject hands back."""
from pathlib import Path
import re
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def cut(text, start_marker, end_marker, after=0):
    start = text.index(start_marker, after)
    end = text.index(end_marker, start + len(start_marker))
    return text[start:end]


util = (ROOT / "OptiScaler/Util.cpp").read_text()
check = cut(util, "bool Util::CheckForRealObject(", "\n}\n") + "\n}\n"
check = check.replace("bool Util::CheckForRealObject(", "bool CheckForRealObject(", 1)

dlssg = (ROOT / "OptiScaler/framegen/dlssg/DLSSG_Dx12.cpp").read_text()
first = dlssg.index("bool DLSSG_Dx12::CreateSwapchainInternal(")
second = dlssg.index("bool DLSSG_Dx12::CreateSwapchain1Internal(")
site0 = cut(dlssg, "    IDXGIFactory* slFactory = nullptr;", "    StreamlineProxy::SetFeatureLoaded()", first)
site1 = cut(dlssg, "        IDXGIFactory* slFactory = nullptr;", "        IDXGIFactory2* factory2", second)

hooks = (ROOT / "OptiScaler/hooks/DxgiFactory_Hooks.cpp").read_text()
site2 = cut(hooks, "void DxgiFactoryHooks::HookToDLSSGFactory(IDXGIFactory* pFactory)\n{", "    LOG_FUNC();")
site2 = site2.replace("void DxgiFactoryHooks::HookToDLSSGFactory(", "void SiteHookToDLSSGFactory(", 1)

unit = (HERE / "fakes.h").read_text() + f"""
static GUID streamlineRiid {{}};
namespace Util
{{
{check}
}} // namespace Util

static void* o_DLSSGCreateSwapChain = nullptr;
{site2}    hooked = true;
}}

namespace DxgiFactoryHooks
{{
inline void HookToDLSSGFactory(IDXGIFactory* factory)
{{
    ++hookCalls;
    SiteHookToDLSSGFactory(factory);
}}
}} // namespace DxgiFactoryHooks

static void SiteCreateSwapchain(IDXGIFactory* factory)
{{
{site0}}}

static void SiteCreateSwapchain1(IDXGIFactory* factory)
{{
{site1}}}
""" + (HERE / "cases.cpp").read_text()

with tempfile.TemporaryDirectory(prefix="retoque-fg-streamline-real-object-") as folder:
    cpp, binary = Path(folder) / "cases.cpp", Path(folder) / "cases"
    cpp.write_text(unit)
    subprocess.run(["g++", "-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter",
                    "-Wno-unused-variable", "-fsanitize=address,undefined", "-fno-omit-frame-pointer", str(cpp),
                    "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

# Every caller: the object CheckForRealObject hands back is borrowed, so the caller never releases it.
call = re.compile(r"CheckForRealObject\([^;]*?\(IUnknown\*\*\)\s*&(\w+)\)|CheckForRealObject\([^;]*?,\s*&(\w+)\)")
offenders = []
for path in sorted(list((ROOT / "OptiScaler").rglob("*.cpp")) + list((ROOT / "retoque").rglob("*.cpp"))):
    if "external" in path.parts:
        continue
    text = path.read_text(errors="replace")
    for match in call.finditer(text):
        name = match.group(1) or match.group(2)
        # Up to the end of the enclosing function: the next line that closes a top-level brace
        end = text.find("\n}\n", match.end())
        body = text[match.end(): end if end != -1 else len(text)]
        if re.search(rf"\b{re.escape(name)}\s*->\s*Release\s*\(", body):
            line = text.count("\n", 0, match.start()) + 1
            offenders.append(f"{path.relative_to(ROOT)}:{line}: {name}->Release() after CheckForRealObject")

if offenders:
    print("CheckForRealObject already released the reference its query added; these release it again:")
    print("\n".join(offenders))
    sys.exit(1)
print("fg-streamline-real-object: no caller releases the borrowed object")

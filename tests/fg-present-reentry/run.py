#!/usr/bin/env python3
"""Compile the wrapped swapchain's present lock, sliced verbatim from production, and re-enter it on one thread and
contend for it from another."""
from pathlib import Path
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
source = (ROOT / "OptiScaler/wrapped/wrapped_swapchain.cpp").read_text()


def function(signature):
    begin = source.index(signature)
    start = source.index("{", begin)
    depth, end = 1, start + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[begin:end]


prefix = "HRESULT STDMETHODCALLTYPE WrappedIDXGISwapChain4::"
parts = [function(prefix + name + "(") for name in ("Present", "Present1", "SetFullscreenState")]
# Both resizes up to their device work: the local-mutex wait and the FG mutex acquisition, verbatim. The release half
# and its lexical scope are fg-locks' subject.
for name in ("ResizeBuffers", "ResizeBuffers1"):
    body = function(prefix + name + "(")
    parts.append(body[: body.index("    HRESULT result")] + "    HRESULT result = resizeWork();\n    return result;\n}\n")

mutex = (ROOT / "OptiScaler/OwnedMutex.h").read_text()
mutex = mutex[mutex.index("class OwnedMutex") :]
fakes = (HERE / "fakes.h").read_text().replace("// @owned-mutex@", mutex)
unit = fakes + "\n".join(parts) + (HERE / "cases.cpp").read_text()
with tempfile.TemporaryDirectory(prefix="retoque-fg-present-reentry-") as folder:
    cpp, binary = Path(folder) / "cases.cpp", Path(folder) / "cases"
    cpp.write_text(unit)
    subprocess.run(["g++", "-std=c++20", "-O1", "-g", "-pthread", "-Wall", "-Wextra", "-Werror",
                    "-Wno-unused-parameter", "-Wno-unused-function", "-fsanitize=address,undefined",
                    "-fno-omit-frame-pointer", str(cpp), "-o", str(binary)], check=True)
    sys.exit(subprocess.run([str(binary)], timeout=90).returncode)

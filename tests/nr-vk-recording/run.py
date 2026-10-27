#!/usr/bin/env python3
"""Host ownership and bounded-slot tests; this suite does not execute Vulkan."""

import os
from pathlib import Path
import shlex
import subprocess
import sys
import tempfile


HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]


def main():
    with tempfile.TemporaryDirectory(prefix="nr-vk-recording-") as directory:
        executable = Path(directory) / "cases"
        compiler = shlex.split(os.environ.get("CXX", "g++"))
        subprocess.run(
            compiler + ["-std=c++20", "-O1", "-g", "-Wall", "-Wextra", "-Werror", "-pthread",
                        "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-fno-pie", "-no-pie",
                        "-I" + str(ROOT / "retoque"), str(HERE / "cases.cpp"), "-o", str(executable)],
            check=True,
        )
        env = dict(os.environ)
        env.setdefault("ASAN_OPTIONS", "detect_leaks=1:abort_on_error=1")
        env.setdefault("UBSAN_OPTIONS", "halt_on_error=1:print_stacktrace=1")
        subprocess.run([str(executable)], env=env, check=True, timeout=30)
    subprocess.run([sys.executable, str(HERE / "integration.py")], check=True, timeout=150)
    subprocess.run([sys.executable, str(HERE / "hooks_test.py")], check=True, timeout=90)
    subprocess.run([sys.executable, str(HERE / "install_test.py")], check=True, timeout=90)
    subprocess.run([sys.executable, str(HERE / "oom_test.py")], check=True, timeout=90)
    print("PASS: Vulkan recording ownership, production dispatch, lifecycle hooks and OOM boundaries (host only)")


if __name__ == "__main__":
    main()

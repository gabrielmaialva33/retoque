#!/usr/bin/env python3
"""Exercise the moving GPU fixture's evidence checks without Wine or a model."""
from pathlib import Path
import subprocess
import sys
import tempfile

here = Path(__file__).resolve().parents[1] / "dlssnr-loopback"
with tempfile.TemporaryDirectory(prefix="retoque-sweep-test-") as tmp:
    binary = Path(tmp) / "sweep"
    subprocess.run(["g++", "-std=c++20", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined",
                    "-fno-omit-frame-pointer", "-I", str(here), str(here / "test_sweep.cpp"), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)
subprocess.run([sys.executable, here / "test_sweep_consumer.py"], check=True)
subprocess.run([sys.executable, here / "test_compiler_entrypoints.py"], check=True)

subprocess.run([sys.executable, Path(__file__).resolve().parents[1] /
                'dlssnr-loopback/test_moving.py'], check=True)
subprocess.run([sys.executable, Path(__file__).resolve().parents[1] /
                'dlssnr-loopback/test_runner_guard.py'], check=True)

subprocess.run([sys.executable, Path(__file__).resolve().parents[1] /
                'dlssnr-loopback/test_present_guides_probe.py'], check=True)
subprocess.run([sys.executable, Path(__file__).resolve().parents[1] /
                'dlssnr-loopback/test_dx11_present_probe.py'], check=True)

#!/usr/bin/env python3
"""Actual stacked Vulkan hooks and the real deferred owner queue."""
from pathlib import Path
import subprocess
subprocess.run(["python3", str(Path(__file__).resolve().parents[1]/"upstream-review/vulkan.py")],check=True)

#!/usr/bin/env python3
"""Heap-attribution counterexample using production memory selection; no GPU."""
from pathlib import Path
import subprocess
import sys
import tempfile

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[1]
sys.path.insert(0, str(ROOT / 'tests/nr-vk-recording'))
from hooks_test import function

source = (ROOT / 'retoque/dlssnr/DlssNrFeature_Vk.cpp').read_text()
production = function(source, 'uint32_t FindMemoryTypeIndexOn(')
with tempfile.TemporaryDirectory(prefix='nr-vk-budget-boundary-') as temporary:
    directory = Path(temporary)
    cpp = directory / 'cases.cpp'
    cpp.write_text((HERE / 'budget_boundary.cpp').read_text().replace('// @production@', production))
    binary = directory / 'cases'
    subprocess.run(['g++', '-std=c++20', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-fno-pie', '-no-pie', '-g',
                    '-I', str(HERE), '-I', str(ROOT / 'retoque/dlssnr'),
                    '-I', str(ROOT / 'external/vulkan/include'), str(cpp), '-o', str(binary)], check=True)
    subprocess.run([str(binary)], check=True, timeout=30)

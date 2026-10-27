#!/usr/bin/env python3
"""Execute host production-body and embedded SPIR-V contract regressions."""
from pathlib import Path
import subprocess
import sys
from integration import main

if __name__ == '__main__':
    here = Path(__file__).resolve().parent
    root = here.parents[1]
    # Mirror integration's optional alternate source root.
    for index, argument in enumerate(sys.argv[1:], 1):
        if argument == '--root':
            root = Path(sys.argv[index+1]).resolve()
        elif argument.startswith('--root='):
            root = Path(argument.split('=', 1)[1]).resolve()
    shader = root/'retoque/shaders/dlssnr/precompile'
    checked = subprocess.run([sys.executable, str(here/'shader_contract.py'),
                              '--header', str(shader/'DlssNr_Scaler_Shaders_Vk.h'),
                              '--spv-dir', str(shader/'scaler')], check=False)
    raise SystemExit(checked.returncode or main())

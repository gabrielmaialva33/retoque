#!/usr/bin/env python3
"""Build/run the standalone LSFG D3D12 study. All artifacts live outside git."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys

HERE = Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(ROOT))
from tools.build_local import prefix_lease, run_step
DEFAULT = Path.home() / 'Projects/personal/OptiScaler_DLSSNR/Tmp/lsfg-study'
MSVC = Path.home() / '.local/opt/msvc/bin/x64'
PREFIX = Path.home() / '.local/opt/msvc-wineprefix'


def command(args, env, log, cwd, timeout):
    print('+', ' '.join(map(str, args)), flush=True)
    with prefix_lease():
        result = run_step(list(map(str, args)), env=env, cwd=cwd, log_path=log,
                          inactivity_timeout=timeout, hard_timeout=timeout)
    if result:
        raise RuntimeError(f'exit {result}: {log}\n' + log.read_text(errors='replace')[-6000:])


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    modes = ap.add_mutually_exclusive_group()
    modes.add_argument('--build-only', action='store_true')
    modes.add_argument('--run-only', action='store_true')
    ap.add_argument('--artifacts', type=Path, default=DEFAULT)
    ap.add_argument('--output', type=Path)
    ap.add_argument('--shaders', type=Path, help='DXIL directory, <rid>.cso (default <artifacts>/dxil)')
    ap.add_argument('--multiplier', type=int, choices=(2, 3, 4), default=2)
    ap.add_argument('--fp16', action='store_true', help='select original FP16 shader variants (+48)')
    ap.add_argument('--performance', action='store_true')
    ap.add_argument('--flow-scale', type=float, default=1.0)
    ap.add_argument('--benchmark', action='store_true')
    ap.add_argument('--width', type=int, default=1920)
    ap.add_argument('--height', type=int, default=1080)
    ap.add_argument('--iterations', type=int, default=400)
    ap.add_argument('--warmup', type=int, default=100)
    ap.add_argument('--debug', action='store_true', help='require D3D12 debug layer')
    ap.add_argument('--gpu-validation', action='store_true', help='require GPU based validation')
    ap.add_argument('inputs', nargs='*', type=Path, help='one folder of numbered PNGs or two PNG paths')
    args = ap.parse_args()
    base = args.artifacts.resolve()
    work = base / 'harness'
    work.mkdir(parents=True, exist_ok=True)
    env = dict(os.environ, WINEPREFIX=str(PREFIX), WINEDEBUG='-all', WINE_MSVC_RAW_STDOUT='1')
    if not args.run_only:
        command([MSVC / 'cl', '/nologo', '/std:c++20', '/EHsc', '/MD', '/W4', '/O2',
                 '/DUNICODE', '/D_UNICODE', HERE / 'main.cpp', '/Fo' + str(work / 'main.obj'),
                 '/Fe' + str(work / 'lsfg-harness.exe'), '/link', 'd3d12.lib', 'dxgi.lib', 'dxguid.lib',
                 'windowscodecs.lib', 'ole32.lib'], env, work / 'build.log', work, 180)
        candidates = [Path.home() / '.local/share/Steam/steamapps/common/Proton - Experimental/files/lib/wine/vkd3d-proton/x86_64-windows',
                      Path.home() / '.local/share/Steam/steamapps/common/Proton Hotfix/files/lib/wine/vkd3d-proton/x86_64-windows']
        candidates += sorted((Path.home() / '.local/share/Steam/compatibilitytools.d').glob('GE-Proton*/files/lib/wine/vkd3d-proton/x86_64-windows'), reverse=True)
        runtime = next(p for p in candidates if (p / 'd3d12.dll').exists())
        files = [runtime / name for name in ('d3d12.dll', 'd3d12core.dll')]
        files += list(sorted(MSVC.parents[1].glob('VC/Redist/MSVC/*/x64/Microsoft.VC143.CRT'))[-1].glob('*.dll'))
        files += [ROOT / 'OptiScaler/shaders/shader_tools/dxcompiler.dll']
        for src in files:
            dst = work / src.name
            dst.unlink(missing_ok=True)
            shutil.copy2(src, dst)
    if args.build_only:
        return
    if not args.benchmark and len(args.inputs) not in (1, 2):
        ap.error('supply a folder or two PNG paths')
    output = (args.output or base / 'out').resolve()
    output.mkdir(parents=True, exist_ok=True)
    win = lambda p: 'Z:' + str(p.resolve())
    shaders = (args.shaders or base / 'dxil').resolve()
    cmd = ['wine', work / 'lsfg-harness.exe', '--shaders', win(shaders), '--out', win(output),
           '--multiplier', str(args.multiplier), '--flow-scale', str(args.flow_scale)]
    if args.benchmark:
        for name in ('width', 'height', 'iterations', 'warmup'):
            cmd += ['--' + name, str(getattr(args, name))]
    for flag in ('fp16', 'performance', 'benchmark', 'debug', 'gpu_validation'):
        if getattr(args, flag):
            cmd += ['--' + flag.replace('_', '-')]
    cmd += list(map(win, args.inputs))
    env['WINEDLLOVERRIDES'] = 'd3d12=n;d3d12core=n;dxcompiler=n;vcruntime140=n;vcruntime140_1=n;msvcp140=n'
    command(cmd, env, output / 'run.log', work, 300)
    print((output / 'run.log').read_text(errors='replace'))


if __name__ == '__main__':
    try:
        main()
    except (OSError, RuntimeError, StopIteration, subprocess.SubprocessError) as error:
        print('FAIL:', error, file=sys.stderr)
        sys.exit(1)

#!/usr/bin/env python3
"""Check the inherited X11/XWayland connection under the rendering supervisor."""
from __future__ import annotations

import argparse
import ctypes
import os
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]/'tests'))
from _suite_result import result_run, write_result


def probe_display(environ, *, load_library=ctypes.CDLL):
    display = environ.get('DISPLAY')
    result = {
        'status': 'FAIL', 'backend': 'x11', 'display': display,
        'probe': 'XOpenDisplay/XCloseDisplay', 'connected': False,
        'runtime_coverage': False,
        'scope': 'X11/XWayland connection and authorization only; no native Wayland or GPU/runtime coverage',
    }
    if not isinstance(display, str) or not display or '\0' in display:
        return dict(result, reason='DISPLAY is missing, empty or invalid; no display was selected')
    try:
        library = load_library('libX11.so.6')
        library.XOpenDisplay.argtypes = [ctypes.c_char_p]
        library.XOpenDisplay.restype = ctypes.c_void_p
        library.XCloseDisplay.argtypes = [ctypes.c_void_p]
        library.XCloseDisplay.restype = ctypes.c_int
        connection = library.XOpenDisplay(os.fsencode(display))
        if not connection:
            return dict(result, reason=f'XOpenDisplay could not connect/authorize inherited DISPLAY={display!r}')
        closed = library.XCloseDisplay(connection)
        if closed != 0:
            return dict(result, reason=f'XCloseDisplay failed for inherited DISPLAY={display!r}')
    except (OSError, AttributeError, TypeError, ValueError) as error:
        return dict(result, reason=f'X11 connection probe unavailable for DISPLAY={display!r}: {error}')
    return dict(result, status='PASS', connected=True,
                reason=f'opened and closed inherited DISPLAY={display!r}')


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--report', required=True, type=Path)
    args = parser.parse_args(argv)
    with result_run(args.report):
        result = write_result(args.report, probe_display(os.environ))
        print(f"{result['status']}: {result['scope']}; {result['reason']}", flush=True)
        return 0 if result['status'] == 'PASS' else 1


if __name__ == '__main__':
    sys.exit(main())

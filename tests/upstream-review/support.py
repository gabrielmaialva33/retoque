"""Compile production slices, optionally from a git ref, without checking it out."""
from pathlib import Path
import os
import subprocess
import tempfile
ROOT = Path(__file__).resolve().parents[2]
REF = os.environ.get('RETOQUE_REVIEW_REF')
def source(path):
    if REF:
        return subprocess.check_output(['git', 'show', f'{REF}:{path}'], cwd=ROOT, text=True)
    return (ROOT / path).read_text()
def function(text, signature):
    start = text.index(signature); begin = text.index('{', start); end = begin + 1; depth = 1
    while depth:
        depth += (text[end] == '{') - (text[end] == '}'); end += 1
    return text[start:end]
def stripped(text):
    return '\n'.join(x for x in text.splitlines() if not x.startswith(('#include', '#pragma once')))
def execute(name, text, args=()):
    with tempfile.TemporaryDirectory(prefix='review-'+name+'-') as folder:
        cpp = Path(folder)/'test.cpp'; exe = Path(folder)/'test'; cpp.write_text(text)
        subprocess.run(['g++','-std=c++20','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer',str(cpp),'-o',str(exe)],check=True)
        failed = False
        for arg in args or ('',):
            r = subprocess.run([str(exe), *([arg] if arg else [])],capture_output=True,text=True)
            print(f'{"FAIL" if r.returncode else "PASS"}: {name} {arg}',flush=True)
            if r.returncode: print(r.stderr,flush=True); failed=True
        if failed: raise SystemExit(1)

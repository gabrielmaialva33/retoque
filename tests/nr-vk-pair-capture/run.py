#!/usr/bin/env python3
"""Production capture rules and independent safety mutants. No GPU or Wine."""
from pathlib import Path
import os, shutil, subprocess, sys
ROOT=Path(__file__).resolve().parents[2]
HERE=Path(__file__).resolve().parent
OUT=HERE/'artifacts'
OUT.mkdir(exist_ok=True)
header=ROOT/'retoque/dlssnr/DlssNr_VkPairCapture.h'
if not header.exists():
    sys.exit('FAIL: Vulkan pair-capture rules not implemented')
def run(include, name, expected=0):
    binary=OUT/name
    subprocess.run(['g++','-std=c++20','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
                    '-fno-omit-frame-pointer','-I',str(HERE/'fakes'),'-I',str(ROOT/'retoque/dlssnr'),'-I',str(include),'-I',str(ROOT/'retoque'),str(HERE/'test.cpp'),'-o',str(binary)],check=True)
    p=subprocess.run([str(binary)],capture_output=True,text=True)
    (OUT/(name+'.log')).write_text(p.stdout+p.stderr)
    if expected==0 and p.returncode: raise RuntimeError(p.stdout+p.stderr)
    if expected!=0 and (p.returncode==0 or 'Assertion' not in p.stderr): raise RuntimeError('mutant did not fail an assertion: '+name+'\n'+p.stderr)
    print(('PASS: ' if expected==0 else 'KILLED: ')+name)
run(ROOT/'retoque','rules')
source=header.read_text()
mutants=[
 ('ignore-retirement','if (!e.retired.load(std::memory_order_acquire))','if (false)'),
 ('ignore-abandon','if (e.invalid.load(std::memory_order_acquire))','if (false)'),
 ('ignore-marker','return marker() == e.cookie ? Result::Readable : Result::Discarded;','return (void)marker, Result::Readable;'),
 ('ignore-budget','bytes > kBudgetBytes - used_','false'),
 ('allow-simultaneous','t.reuse == VkRecording::Reuse::Serial','t.reuse != VkRecording::Reuse::Unknown'),
]
for name,old,new in mutants:
    if source.count(old)!=1: raise RuntimeError('mutation anchor: '+name)
    folder=OUT/name
    # Executables use names above, mutation roots use a separate suffix.
    folder=OUT/(name+'-src'); (folder/'dlssnr').mkdir(parents=True,exist_ok=True)
    (folder/'dlssnr/DlssNr_VkPairCapture.h').write_text(source.replace(old,new))
    run(folder,name,1)

shared = ROOT/'retoque/dlssnr/DlssNr_PairCapture.h'
folder = OUT/'untracked-copy-src'
(folder/'dlssnr').mkdir(parents=True,exist_ok=True)
text = shared.read_text()
old = 'if (!backend.Track(recording, next.ticket))'
assert text.count(old) == 1
(folder/'dlssnr/DlssNr_PairCapture.h').write_text(text.replace(old, 'if ((void)backend.Track(recording, next.ticket), false)'))
run(folder, 'untracked-copy', 1)

backend = OUT/'backend'
subprocess.run(['g++','-std=c++20','-O1','-g','-Wall','-Wextra','-Werror','-Wno-missing-field-initializers','-Wno-format-truncation',
                '-fsanitize=address,undefined','-fno-omit-frame-pointer','-I',str(HERE/'fakes'),
                '-I',str(ROOT/'retoque'),'-I',str(ROOT/'external/vulkan/include'),
                str(HERE/'backend_test.cpp'),'-o',str(backend)],check=True)
subprocess.run([str(backend)],check=True)

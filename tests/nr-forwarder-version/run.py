#!/usr/bin/env python3
"""The forwarder's release version follows OptiScaler/resource.h in both build routes."""
from pathlib import Path
import re
import subprocess
import tempfile
import xml.etree.ElementTree as ET

ROOT = Path(__file__).resolve().parents[2]
FORWARDER = ROOT / 'retoque/dlssnr/forwarder'
NS = {'m': 'http://schemas.microsoft.com/developer/msbuild/2003'}


def check(rc, project, cmake):
    assert '#include "../../../OptiScaler/resource.h"' in rc, 'version source must be OptiScaler/resource.h'
    assert re.search(r'^FILEVERSION\s+VER_FILE_VERSION\s*$', rc, re.M), 'file version must follow shared macro'
    assert re.search(r'^PRODUCTVERSION\s+VER_PRODUCT_VERSION\s*$', rc, re.M), 'product version must follow shared macro'
    for key in ('FileVersion', 'ProductVersion'):
        assert re.search(r'VALUE\s+"' + key + r'",\s*VER_FILE_VERSION_STR', rc), key
    assert not re.search(r'#\s*define\s+VER_', rc), 'forwarder must not redefine versions'
    root = ET.fromstring(project)
    assert any(node.get('Include') == 'dlssnr_forwarder.rc' for node in root.findall('.//m:ResourceCompile', NS)), \
        'MSBuild omits version resource'
    assert re.search(r'enable_language\(RC\)', cmake), 'CMake must enable resource compiler'
    assert re.search(r'target_sources\(dlssnr_forwarder PRIVATE dlssnr_forwarder\.rc\)', cmake), \
        'CMake omits version resource'


rc_path = FORWARDER / 'dlssnr_forwarder.rc'
assert rc_path.exists(), 'forwarder has no version resource'
rc = rc_path.read_text()
project = (FORWARDER / 'dlssnr_forwarder.vcxproj').read_text()
cmake = (FORWARDER / 'CMakeLists.txt').read_text()
check(rc, project, cmake)
print('PASS: shared version source and both build routes')

header = (ROOT / 'OptiScaler/resource.h').read_text()
main_rc = (ROOT / 'OptiScaler/OptiScaler.rc').read_text(encoding='utf-16')
with tempfile.TemporaryDirectory(prefix='nr-forwarder-version-') as directory:
    folder = Path(directory)
    # SDK symbols do not affect the version expressions. This is preprocessing, not an RC compiler test.
    (folder / 'winver.h').write_text('')
    (folder / 'winres.h').write_text('')
    local_rc = rc.replace('../../../OptiScaler/resource.h', 'resource.h')
    for name, contents in [('current', header), ('bumped', re.sub(r'#define VER_MAJOR_VERSION \d+',
                                                                '#define VER_MAJOR_VERSION 73', header))]:
        (folder / 'resource.h').write_text(contents)
        expanded = []
        for source in (main_rc, local_rc):
            result = subprocess.run(['g++', '-E', '-P', '-x', 'c', '-I', str(folder), '-D_DEBUG',
                                     '-DOPTISCALER_BUILD_METADATA', '-'], input=source, text=True,
                                    capture_output=True, check=True)
            versions = []
            for key in ('FILEVERSION', 'PRODUCTVERSION'):
                match = re.search(r'\b' + key + r'\s+(\d+\s*,\s*\d+\s*,\s*\d+\s*,\s*\d+)', result.stdout)
                assert match, (key, result.stdout)
                versions.append(tuple(map(int, match.group(1).split(','))))
            expanded.append(versions)
        assert expanded[0] == expanded[1], (name, expanded)
        if name == 'bumped':
            assert expanded[1][0][0] == 73
        print(f'PASS: {name} fixed file/product versions match: {expanded[1]}')

mutations = [
    ('numeric-file', rc.replace('FILEVERSION VER_FILE_VERSION', 'FILEVERSION 1,2,3,4'), project, cmake),
    ('numeric-product', rc.replace('PRODUCTVERSION VER_PRODUCT_VERSION', 'PRODUCTVERSION 1,2,3,4'), project, cmake),
    ('msbuild-omitted', rc, project.replace('<ResourceCompile Include="dlssnr_forwarder.rc" />', ''), cmake),
    ('cmake-omitted', rc, project, cmake.replace('target_sources(dlssnr_forwarder PRIVATE dlssnr_forwarder.rc)', '')),
]
for name, changed_rc, changed_project, changed_cmake in mutations:
    assert (changed_rc, changed_project, changed_cmake) != (rc, project, cmake), name
    try:
        check(changed_rc, changed_project, changed_cmake)
    except AssertionError:
        print(f'PASS: rejected {name}')
    else:
        raise AssertionError(f'mutant accepted: {name}')

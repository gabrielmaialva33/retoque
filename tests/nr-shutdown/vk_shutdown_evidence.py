#!/usr/bin/env python3
"""Judge one existing tests/vulkan-present-host case's artifacts as Vulkan core-shutdown evidence.

It runs nothing. An executor runs the existing case (`tests/vulkan-present-host/run.py --only <case>`) with Wine's
`+loaddll` trace in the case's run log and, once the runner has finished, writes a receipt for the case: a run id, the
case folder and the Proton prefix it ran in, the application process's own return code, the DLL's hash, and the path
and hash of each artifact (the facts, the OptiScaler log, the run log with the trace, the runner's results). This
reads that receipt and the artifacts it names, and says whether they prove, for that one run, that the consumer named
called the driver core's Shutdown1 on a live core and the process exited 0:

  host    nr-control: the NR present host brought the core up on the device (NVNGXProxy::InitVulkan result 1), and
          its ShutdownPresentCore called it (NVNGXProxy::ShutdownVulkanDevice's entry and result, outside any public
          Shutdown1) before the host's line and the device's destruction. No application NGX reached the core.
  public  nr-route-quiet with an SR DLL linked (DLSS on): the application's init reached the core
          (NVNGXProxy::VULKAN_Init* result 1), then NVSDK_NGX_VULKAN_Shutdown1's entry, the adapter's entry and
          result, and its return, in that order, before the device's destruction. With DLSS disabled no application
          call reaches the core, and this refuses.

Only the adapter's pair proves a call: the host's "shut down" line is written whether or not one was made, and both
consumers discard the core's result (the public return is NVSDK_NGX_VULKAN_Shutdown's, not the core's). A PASS covers
this run only.
"""
import argparse
import hashlib
import json
import re
import sys
from pathlib import Path

CORE_SHA256 = 'dabe18b0dc559750113b589d06f672141c8c49a2a571e0a5a7ca7b3e584a7ac3'
# What each existing case must show for its own scenario, and the consumer it can prove.
SCENARIOS = {
    'nr-control': {'modes': {'host'}, 'swapchains': 4, 'phases': 0},
    'nr-route-quiet': {'modes': {'public'}, 'swapchains': 1, 'phases': 1},
}
ARTIFACTS = ('result', 'log', 'trace', 'runner')
HOST_UP = "the driver's NGX core brought up on it"
HOST_DOWN = "DLSS-NR Vulkan present host: the driver's NGX core shut down on the device before it goes"
HOST_FORGOTTEN = "the driver's NGX core on the device is forgotten"
PUBLIC_INIT = re.compile(r'NVNGXProxy::VULKAN_Init(?:_Ext2|_Ext|_ProjectID_Ext|_with_ProjectID)? result: ([0-9A-Fa-f]+)\s*$')
DLSS_DISABLED = 'nvngx_dlss.dll not found, disabling DLSS'
CORE_LOADED = 'Getting nvngx method addresses'
CORE_PATH = r'C:\windows\system32\_nvngx.dll'
ADAPTER = 'NVNGXProxy::ShutdownVulkanDevice device '
PUBLIC = 'NVSDK_NGX_VULKAN_Shutdown1 device '
CRASH = re.compile(r'c0000005|unhandled exception|page fault on (?:read|write)', re.I)
LIMIT = ('in this run the consumer called the core\'s Shutdown1 on a live core (the adapter logged entry and a result of '
         '1) and the process exited 0; the public SDK return does not carry the core\'s result, and nothing beyond this '
         'run, driver core or Proton is shown')


def sha256(path):
    digest = hashlib.sha256()
    with open(path, 'rb') as f:
        for block in iter(lambda: f.read(1 << 20), b''):
            digest.update(block)
    return digest.hexdigest()


def windows_path(path):
    """How Wine names a host path: Z: and backslashes, case-folded for comparison."""
    return ('Z:' + str(Path(path).resolve()).replace('/', '\\')).casefold()


def native_loads(run_log):
    """The modules Wine's +loaddll trace says it loaded native, as case-folded Windows paths."""
    loads = set()
    for line in run_log.splitlines():
        found = re.search(r'trace:loaddll:\S* Loaded L"([^"]+)" at [0-9A-Fa-f]+: native', line)
        if found:
            loads.add(found.group(1).replace('\\\\', '\\').casefold())
    return loads


def index(lines, predicate):
    return [i for i, line in enumerate(lines) if predicate(line)]


def pairs(lines, prefix, device, result_pattern):
    """`prefix + device` entry lines each followed by its own result line, as (entry, result line, values). An entry
    without its result (the process died inside), or a result without its entry, is refused."""
    found = []
    for i, line in enumerate(lines):
        if prefix + device not in line:
            continue
        tail = line.split(prefix + device, 1)[1].strip()
        match = re.fullmatch(result_pattern, tail)
        if tail == '':
            if found and found[-1][1] is None:
                raise ValueError(f'a second entry before the first returned: {line.strip()}')
            found.append([i, None, None])
        elif match and found and found[-1][1] is None:
            found[-1][1:] = [i, match.groups()]
        else:
            raise ValueError(f'a line out of order: {line.strip()}')
    if any(entry[1] is None for entry in found):
        raise ValueError(f'{prefix.strip()} never returned')
    return found


def validate(mode, receipt_path, dll, run_id, core_sha256=CORE_SHA256):
    receipt = json.loads(Path(receipt_path).read_text())
    if receipt.get('run_id') != run_id:
        raise ValueError('the receipt is from another run')
    case_dir = Path(receipt.get('case_dir', ''))
    case = receipt.get('case')
    if case_dir.name != case:
        raise ValueError('the receipt names a case folder of another case')
    scenario = SCENARIOS.get(case)
    if scenario is None:
        raise ValueError(f'{case!r} is not a case this judge knows')
    if mode not in scenario['modes']:
        raise ValueError(f'{case} cannot prove the {mode} consumer')

    # The executor's receipt: this DLL, the application's own exit (0 and nothing else: no kill), and every artifact
    # exactly as it was when the run finished.
    dll_sha = sha256(dll)
    if receipt.get('dll_sha256') != dll_sha:
        raise ValueError('the receipt is for another DLL')
    code = receipt.get('returncode')
    if type(code) is not int or code != 0:
        raise ValueError(f'the application did not exit 0 (receipt {code!r})')
    files = {}
    for name in ARTIFACTS:
        entry = receipt.get('files', {}).get(name) or {}
        path = Path(entry.get('path', ''))
        if not entry.get('path') or not path.is_file():
            raise ValueError(f'missing {name}: {path}')
        if sha256(path) != entry.get('sha256'):
            raise ValueError(f'{name} is not the artifact this run recorded')
        files[name] = path
    if files['result'] != case_dir / 'result.json' or files['log'] != case_dir / 'OptiScaler.log':
        raise ValueError("the receipt's facts or log are not the case folder's")
    staged = case_dir / 'dxgi.dll'
    if not staged.is_file() or sha256(staged) != dll_sha:
        raise ValueError('the staged dxgi.dll is not the DLL under test')
    core = Path(receipt.get('prefix', '')) / 'pfx/drive_c/windows/system32/_nvngx.dll'
    if not receipt.get('prefix') or not core.is_file() or sha256(core) != core_sha256:
        raise ValueError("the run's prefix core is missing or not the inspected build")

    runner = json.loads(files['runner'].read_text())
    if runner.get('status') != 'PASS' or runner.get('dll_sha256') != dll_sha or \
            runner.get('cases', {}).get(case, {}).get('status') != 'PASS':
        raise ValueError("the runner's final verdict for this run is not PASS")
    facts = json.loads(files['result'].read_text())
    if facts.get('status') != 'PASS':
        raise ValueError(f"harness status {facts.get('status')!r}, not PASS")
    if facts.get('validation_active') is not True or facts.get('validation_errors') != 0:
        raise ValueError('validation was not proved active and clean')
    device = facts.get('device')
    swapchains, phases = facts.get('swapchains'), facts.get('phases')
    if not isinstance(device, str) or not re.fullmatch(r'0x[0-9A-F]+', device):
        raise ValueError(f'no device in the facts ({device!r})')
    if not isinstance(swapchains, list) or len(swapchains) < scenario['swapchains'] or \
            not isinstance(phases, list) or len(phases) < scenario['phases']:
        raise ValueError(f'the facts are not a complete {case} run')

    log = files['log'].read_text(errors='replace')
    trace = files['trace'].read_text(errors='replace')
    for name, text in (('OptiScaler.log', log), ('run log', trace)):
        if fault := CRASH.search(text):
            raise ValueError(f'{name} records a fault: {fault.group(0)}')
    folder = re.search(r'Setting DllPath to (.+?)\s*$', log, re.M)
    if not folder or folder.group(1).casefold() != windows_path(case_dir):
        raise ValueError(f'the OptiScaler log is not from {case_dir}')
    if CORE_LOADED not in log or f'Loaded from {CORE_PATH}' not in log:
        raise ValueError('the proxy never loaded the system core')
    loads = native_loads(trace)
    if windows_path(staged) not in loads:
        raise ValueError("Wine's loader trace does not show this case folder's dxgi.dll loaded")
    if CORE_PATH.casefold() not in loads:
        raise ValueError("Wine's loader trace does not show the prefix's system core loaded")

    # Order and origin on the facts' device: bring-up, the consumer's call (entry, result), the device's destruction.
    lines = log.splitlines()
    destroyed = index(lines, lambda l: f'device {device} #' in l and 'destroyed;' in l)
    if len(destroyed) != 1:
        raise ValueError(f'device {device} was not destroyed exactly once in the log')
    destroyed = destroyed[0]
    calls = pairs(lines, ADAPTER, device, r': core result 0x([0-9A-Fa-f]+), (\d+) remaining')
    public = pairs(lines, PUBLIC, device, r': returning 0x([0-9A-Fa-f]+)')
    if not calls:
        raise ValueError(f'no call reached the core on device {device} (no adapter entry)')
    if len(calls) != 1:
        raise ValueError(f'expected one core call on device {device}, found {len(calls)}')
    entry, returned, (core_result, _) = calls[0]
    if int(core_result, 16) != 1:
        raise ValueError(f'the core did not answer success: 0x{core_result}')
    if not returned < destroyed:
        raise ValueError('the core call came after the device was destroyed')
    inside = [p for p in public if p[0] < entry and returned < p[1]]
    inits = {i: m.group(1).upper() for i, m in enumerate(map(PUBLIC_INIT.search, lines)) if m}
    reached = [i for i, value in inits.items() if value == '1']
    if mode == 'host':
        if inside:
            raise ValueError("the core call was the application's Shutdown1, not the host's")
        if reached:
            raise ValueError('the application initialised the core: the public path, not the host, owns it')
        up = index(lines, lambda l: 'NVNGXProxy::InitVulkan result: ' in l)
        if not up or any(not lines[i].rstrip().endswith('result: 1') for i in up) or not up[-1] < entry:
            raise ValueError('the present host did not bring the core up before the call')
        if not index(lines[:entry], lambda l: f'initialised on device {device} (cold' in l and HOST_UP in l):
            raise ValueError('no cold bring-up by the present host on this device before the call')
        if HOST_FORGOTTEN in log:
            raise ValueError('the host forgot the core instead of shutting it down')
        down = index(lines, lambda l: HOST_DOWN in l)
        if len(down) != 1 or not returned < down[0] < destroyed:
            raise ValueError("the host's shutdown line does not follow its call before the device went")
    else:
        if DLSS_DISABLED in log:
            raise ValueError('DLSS was disabled: the application calls never reach the core in this run')
        if len(inside) != 1:
            raise ValueError("the core call is not inside the application's Shutdown1")
        if not reached or any(value != '1' for value in inits.values()):
            raise ValueError('the application init did not reach the core')
        if not reached[0] < inside[0][0]:
            raise ValueError("the application init did not precede its Shutdown1")
        if not inside[0][1] < destroyed:
            raise ValueError("the application's Shutdown1 returned after the device was destroyed")
    return {'mode': mode, 'case': case, 'run_id': run_id, 'status': 'PASS', 'dll_sha256': dll_sha,
            'core_sha256': core_sha256, 'device': device, 'limit': LIMIT}


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('mode', choices=('host', 'public'))
    parser.add_argument('--receipt', required=True, help="the executor's receipt for the case")
    parser.add_argument('--dll', required=True, help='the OptiScaler.dll the case was run with')
    parser.add_argument('--run-id', required=True, help="the executor's id for this run")
    args = parser.parse_args()
    try:
        record = validate(args.mode, args.receipt, args.dll, args.run_id)
    except (ValueError, OSError, json.JSONDecodeError) as exc:
        print(f'FAIL: {exc}', file=sys.stderr)
        return 1
    print('PASS: ' + json.dumps(record))
    return 0


if __name__ == '__main__':
    sys.exit(main())

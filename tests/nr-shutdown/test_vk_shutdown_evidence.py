#!/usr/bin/env python3
"""Host-only checks that vk_shutdown_evidence.py accepts a complete run and refuses each missing proof.

The synthetic artifacts follow the shape of real tests/vulkan-present-host runs (OptiScaler.log lines, result.json
facts, the runner's results.json, Wine's +loaddll trace, the executor's receipt); they are not runtime evidence.
"""
import hashlib
import json
import sys
import tempfile
from pathlib import Path

sys.dont_write_bytecode = True
sys.path.insert(0, str(Path(__file__).resolve().parent))
import vk_shutdown_evidence as evidence  # noqa: E402

DEVICE = '0x76ADE0'
RUN = 'run-1'
CALL = [f'[15:38:46.329700] [D] NVNGXProxy::ShutdownVulkanDevice device {DEVICE}',
        f'[15:38:46.329770] [D] NVNGXProxy::ShutdownVulkanDevice device {DEVICE}: core result 0x1, 0 remaining']
ENTRY = f'[15:38:46.329600] [D] NVSDK_NGX_VULKAN_Shutdown1 device {DEVICE}'
RETURN = f'[15:38:46.329900] [D] NVSDK_NGX_VULKAN_Shutdown1 device {DEVICE}: returning 0x1'
INIT = '[15:38:36.300001] [I] NVSDK_NGX_VULKAN_Init NVNGXProxy::VULKAN_Init result: 1'
DOWN = f'[15:38:46.339778] [I] Log {evidence.HOST_DOWN}'
GONE = f'[15:38:46.339818] [I] OnDestroyDevice DLSS-NR Vulkan present host: device {DEVICE} #4 destroyed; freed: 0'
COLD = (f"[15:38:37.344081] [I] Log DLSS-NR Vulkan present host: the model initialised on device {DEVICE} (cold: no "
        "application NGX initialisation; the driver's NGX core brought up on it)")


def wine(path):
    return 'Z:' + str(Path(path).resolve()).replace('/', '\\')


def opti_log(work, mode):
    lines = [f'[15:38:36.000000] [I] Setting DllPath to {wine(work)}']
    if mode == 'host':
        lines.append('[15:38:36.100000] [W] nvngx_dlss.dll not found, disabling DLSS')
    lines += ['[15:38:36.200000] [I] NVNGXProxy::InitNVNGX Loaded from C:\\windows\\system32\\_nvngx.dll',
              '[15:38:36.200001] [I] NVNGXProxy::InitNVNGX Getting nvngx method addresses',
              '[15:38:37.139972] [D] NVNGXProxy::InitVulkan result: 1', COLD]
    if mode == 'public':
        lines += [INIT, '[15:38:45.000000] [I] TryDestroyNGXParameters Calling NVFree result: 1', ENTRY] + CALL + \
                 [RETURN, DOWN, GONE]
    else:
        lines += CALL + [DOWN, GONE]
    return '\n'.join(lines) + '\n'


def trace(work):
    escaped = wine(work / 'dxgi.dll').replace('\\', '\\\\')
    return (f'0024:trace:loaddll:build_module Loaded L"{escaped}" at 00006FFFF0000000: native\n'
            '0024:trace:loaddll:build_module Loaded L"C:\\\\windows\\\\system32\\\\_nvngx.dll" at 00006FFFE0000000: '
            'native\n')


def receipt_for(root, case, work, **extra):
    files = {'result': work / 'result.json', 'log': work / 'OptiScaler.log', 'trace': root / f'{case}.log',
             'runner': root / 'results.json'}
    data = {'run_id': RUN, 'case': case, 'case_dir': str(work), 'prefix': str(root / 'prefix'), 'returncode': 0,
            'dll_sha256': evidence.sha256(root / 'OptiScaler.dll'),
            'files': {k: {'path': str(v), 'sha256': evidence.sha256(v)} for k, v in files.items()}}
    data.update(extra)
    (root / 'receipt.json').write_text(json.dumps(data))


def check(mode, case, expect, before=None, after=None):
    """`before` changes the artifacts and the receipt records them (a consistent run with that content); `after`
    changes something once the receipt was written (artifacts mixed or replaced after the run)."""
    with tempfile.TemporaryDirectory(prefix='vk-shutdown-evidence-') as directory:
        root = Path(directory)
        work = root / 'cases' / case
        work.mkdir(parents=True)
        core = root / 'prefix/pfx/drive_c/windows/system32'
        core.mkdir(parents=True)
        (core / '_nvngx.dll').write_bytes(b'core')
        (root / 'OptiScaler.dll').write_bytes(b'dll under test')
        (work / 'dxgi.dll').write_bytes(b'dll under test')
        scenario = evidence.SCENARIOS.get(case, {'swapchains': 1, 'phases': 1})
        (work / 'result.json').write_text(json.dumps({
            'status': 'PASS', 'validation_active': True, 'validation_errors': 0, 'device': DEVICE,
            'swapchains': ['s'] * scenario['swapchains'], 'phases': ['p'] * scenario['phases']}))
        (work / 'OptiScaler.log').write_text(opti_log(work, mode))
        (root / f'{case}.log').write_text('Proton: Upgrading prefix\n' + trace(work) + 'ntsync: up and running.\n')
        (root / 'results.json').write_text(json.dumps({'status': 'PASS', 'dll_sha256': evidence.sha256(
            root / 'OptiScaler.dll'), 'cases': {case: {'status': 'PASS'}}}))
        receipt_for(root, case, work)
        if before:
            # The run had that content: the receipt records the artifacts as they now are, under the paths it names.
            before(root, work)
            data = json.loads((root / 'receipt.json').read_text())
            data['files'] = {k: {'path': v['path'], 'sha256': evidence.sha256(v['path'])}
                             for k, v in data['files'].items() if Path(v['path']).exists()}
            (root / 'receipt.json').write_text(json.dumps(data))
        if after:
            after(root, work)
        core_sha = hashlib.sha256(b'core').hexdigest()  # the inspected build in this synthetic prefix
        try:
            evidence.validate(mode, root / 'receipt.json', root / 'OptiScaler.dll', RUN, core_sha)
        except (ValueError, OSError) as exc:
            assert expect is not None and expect in str(exc), f'{mode} {case}: expected {expect!r}, got {exc}'
            return
        assert expect is None, f'{mode} {case}: accepted, expected refusal {expect!r}'


def edit(path, old, new):
    text = path.read_text()
    assert old in text, (path, old)
    path.write_text(text.replace(old, new))


def put(path, **values):
    data = json.loads(path.read_text())
    data.update(values)
    path.write_text(json.dumps(data))


def log_of(work):
    return work / 'OptiScaler.log'


CASES = (('host', 'nr-control'), ('public', 'nr-route-quiet'))

# The coordinator's first repro: an unknown case. And each case proves only its own consumer.
check('public', 'unrelated-case', 'not a case this judge knows')
check('host', 'nr-route-quiet', 'cannot prove the host consumer')
check('public', 'nr-control', 'cannot prove the public consumer')

# Complete runs.
for mode, case in CASES:
    check(mode, case, None)

for mode, case in CASES:
    def run(expect, before=None, after=None):
        check(mode, case, expect, before, after)

    # The receipt: this run, this case, this DLL, exit 0 exactly (a kill after PASS is not an exit).
    run('from another run', after=lambda root, work: put(root / 'receipt.json', run_id='run-2'))
    run('did not exit 0', before=lambda root, work: put(root / 'receipt.json', returncode=-9))
    run('did not exit 0', before=lambda root, work: put(root / 'receipt.json', returncode=5))
    run('did not exit 0', before=lambda root, work: put(root / 'receipt.json', returncode=False))
    run('another case', after=lambda root, work: put(root / 'receipt.json', case='nr'))
    run('another DLL', after=lambda root, work: put(root / 'receipt.json', dll_sha256='0' * 64))
    # Mixed artifacts: any of them replaced after the run (another run's log, trace, facts or runner results).
    for name in ('result', 'log', 'trace', 'runner'):
        run(f'{name} is not the artifact this run recorded',
            after=lambda root, work, name=name: Path(json.loads((root / 'receipt.json').read_text())['files'][name][
                'path']).write_text(Path(json.loads((root / 'receipt.json').read_text())['files'][name][
                    'path']).read_text() + ' '))
    run('missing trace', after=lambda root, work: (root / f'{case}.log').unlink())
    run("facts or log are not the case folder's", before=lambda root, work: (
        (root / 'other').mkdir(), (root / 'other/OptiScaler.log').write_text(log_of(work).read_text()),
        receipt_for(root, case, work), put(root / 'receipt.json', files={
            **json.loads((root / 'receipt.json').read_text())['files'],
            'log': {'path': str(root / 'other/OptiScaler.log'),
                    'sha256': evidence.sha256(root / 'other/OptiScaler.log')}})))
    # The runner's final verdict: aggregate and case, for this DLL.
    run('final verdict', before=lambda root, work: put(root / 'results.json', status='FAIL'))
    run('final verdict', before=lambda root, work: put(root / 'results.json', cases={case: {'status': 'FAIL'}}))
    run('final verdict', before=lambda root, work: put(root / 'results.json', dll_sha256='0' * 64))
    # The facts of the scenario.
    run('not PASS', before=lambda root, work: put(work / 'result.json', status='FAIL'))
    run('validation was not proved', before=lambda root, work: put(work / 'result.json', validation_errors=2))
    run('not a complete', before=lambda root, work: put(work / 'result.json', swapchains=[]))
    # Modules: the staged DLL, the prefix's core, and exact loaded paths.
    run('not the DLL under test', after=lambda root, work: (work / 'dxgi.dll').write_bytes(b'other'))
    run('prefix core', after=lambda root, work: (root / 'prefix/pfx/drive_c/windows/system32/_nvngx.dll'
                                                 ).write_bytes(b'other'))
    run('prefix core', after=lambda root, work: put(root / 'receipt.json', prefix=str(root / 'elsewhere')))
    run("this case folder's dxgi.dll", before=lambda root, work: edit(
        root / f'{case}.log', wine(work / 'dxgi.dll').replace('\\', '\\\\'),
        wine(root / 'old' / 'cases' / case / 'dxgi.dll').replace('\\', '\\\\')))  # same suffix, another folder
    run("prefix's system core", before=lambda root, work: edit(root / f'{case}.log', '_nvngx.dll" at 00006FFFE0000000: '
                                                               'native', '_nvngx.dll" at 00006FFFE0000000: builtin'))
    run('not from', before=lambda root, work: edit(log_of(work), f'Setting DllPath to {wine(work)}',
                                                   f'Setting DllPath to {wine(root / "old" / "cases" / case)}'))
    run('never loaded the system core', before=lambda root, work: edit(log_of(work), 'Loaded from C:', 'From C:'))
    run('records a fault', before=lambda root, work: edit(root / f'{case}.log', 'ntsync',
                                                          'wine: Unhandled page fault on write\nntsync'))
    # The core call: absent or bypassed, died inside, failing, repeated, after the device went, another device.
    run('no call reached the core', before=lambda root, work: edit(log_of(work), CALL[0] + '\n' + CALL[1] + '\n', ''))
    run('never returned', before=lambda root, work: edit(log_of(work), CALL[1] + '\n', ''))
    run('did not answer success', before=lambda root, work: edit(log_of(work), 'core result 0x1,',
                                                                 'core result 0xBAD00002,'))
    run('expected one core call', before=lambda root, work: edit(log_of(work), GONE, CALL[0] + '\n' + CALL[1] + '\n'
                                                                 + GONE))
    run('after the device was destroyed', before=lambda root, work: (
        edit(log_of(work), CALL[0] + '\n' + CALL[1] + '\n', ''),
        edit(log_of(work), GONE + '\n', GONE + '\n' + CALL[0] + '\n' + CALL[1] + '\n')))
    run('no call reached the core', before=lambda root, work: edit(log_of(work), f'ShutdownVulkanDevice device {DEVICE}',
                                                                   'ShutdownVulkanDevice device 0x1'))
    run('out of order', before=lambda root, work: edit(log_of(work), CALL[0] + '\n', ''))
    run('destroyed exactly once', before=lambda root, work: edit(log_of(work), GONE, 'device going'))

# The host: brought up before the call, not by the application, its call outside any public Shutdown1, and its line
# after the call (the line alone is written even when no call was made).
host = lambda expect, before: check('host', 'nr-control', expect, before)  # noqa: E731
host('did not bring the core up before', lambda root, work: edit(log_of(work), 'InitVulkan result: 1',
                                                                 'InitVulkan result: BAD00002'))
host('did not bring the core up before', lambda root, work: (
    edit(log_of(work), '[15:38:37.139972] [D] NVNGXProxy::InitVulkan result: 1\n', ''),
    edit(log_of(work), CALL[1] + '\n', CALL[1] + '\n[15:38:46.33] [D] NVNGXProxy::InitVulkan result: 1\n')))  # late
host('no cold bring-up', lambda root, work: edit(log_of(work), evidence.HOST_UP, 'brought up'))
host('public path, not the host', lambda root, work: edit(log_of(work), COLD, COLD + '\n' + INIT))
host("application's Shutdown1, not the host's", lambda root, work: (
    edit(log_of(work), CALL[0], ENTRY + '\n' + CALL[0]), edit(log_of(work), CALL[1], CALL[1] + '\n' + RETURN)))
host('forgot the core', lambda root, work: edit(log_of(work), DOWN, DOWN + '\n' + evidence.HOST_FORGOTTEN))
host("shutdown line does not follow its call", lambda root, work: (
    edit(log_of(work), DOWN + '\n', ''), edit(log_of(work), CALL[0], DOWN + '\n' + CALL[0])))
host("shutdown line does not follow its call", lambda root, work: edit(log_of(work), DOWN + '\n', ''))

# The public input: DLSS on; its init reached the core before its Shutdown1, and the call is inside that Shutdown1.
public = lambda expect, before: check('public', 'nr-route-quiet', expect, before)  # noqa: E731
public('DLSS was disabled', lambda root, work: edit(log_of(work), 'Getting nvngx',
                                                    'nvngx_dlss.dll not found, disabling DLSS\n[I] Getting nvngx'))
public("not inside the application's Shutdown1", lambda root, work: (edit(log_of(work), ENTRY + '\n', ''),
                                                                      edit(log_of(work), RETURN + '\n', '')))  # host's
public("not inside the application's Shutdown1", lambda root, work: (
    edit(log_of(work), ENTRY + '\n', ''), edit(log_of(work), CALL[1] + '\n', CALL[1] + '\n' + ENTRY + '\n')))
public('application init did not reach the core', lambda root, work: edit(log_of(work), 'VULKAN_Init result: 1',
                                                                          'VULKAN_Init result: BAD00001'))
public('application init did not reach the core', lambda root, work: edit(log_of(work), INIT + '\n', ''))
public('application init did not precede', lambda root, work: (
    edit(log_of(work), INIT + '\n', ''), edit(log_of(work), RETURN, RETURN + '\n' + INIT)))  # late init
public('never returned', lambda root, work: edit(log_of(work), RETURN + '\n', ''))
public('returned after the device was destroyed', lambda root, work: (
    edit(log_of(work), RETURN + '\n', ''), edit(log_of(work), GONE, GONE + '\n' + RETURN)))

print('PASS: Vulkan shutdown evidence accepts complete host/public runs of the known cases and refuses unknown cases, '
      'the wrong consumer, other runs, non-zero or killed exits, mixed or replaced artifacts, failed runner verdicts, '
      'incomplete facts, modules loaded from another folder or prefix, faults, missing, bypassed, failing, repeated or '
      'late core calls, late or missing init, and public runs with DLSS disabled')

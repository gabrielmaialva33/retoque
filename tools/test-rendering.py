#!/usr/bin/env python3
"""Serial rendering regressions with invocation-bound, retained evidence."""
from __future__ import annotations

import argparse
from dataclasses import dataclass
from datetime import datetime, timezone
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import signal
import sys
import threading
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tests'))
import run_all as runner
from _suite_result import atomic_json

VK_CASES = ('nr', 'nr-route-quiet', 'nr-route-quiet-wdx12', 'nr-route-guides')


def duration_arg(value):
    try:
        seconds = int(value)
    except ValueError as error:
        raise argparse.ArgumentTypeError('duration must be an integer in 5..120 seconds') from error
    if not 5 <= seconds <= 120:
        raise argparse.ArgumentTypeError('duration must be an integer in 5..120 seconds')
    return seconds


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--host-only', action='store_true')
    parser.add_argument('--no-build', action='store_true')
    parser.add_argument('--fg-steady-seconds', type=duration_arg, default=0, metavar='N',
                        help='explicit FG qualification duration per active phase, integer 5..120 (suggested 15)')
    parser.add_argument('--report', type=Path, help='consolidated JSON report')
    parser.add_argument('--log-dir', type=Path, help='parent directory for retained invocation logs')
    parser.add_argument('--strict', action='store_true', help='compatibility flag; rendering coverage is always strict')
    return parser.parse_args(argv)


def validate_vulkan(data, dll_sha256):
    findings = []
    if data.get('status') != 'PASS':
        findings.append('Vulkan aggregate is not PASS')
    if data.get('dll_sha256') != dll_sha256 or not dll_sha256:
        findings.append('Vulkan DLL hash does not match selected build')
    cases = data.get('cases')
    if not isinstance(cases, dict) or set(cases) != set(VK_CASES):
        findings.append('Vulkan required case set is missing, duplicated, or unexpected')
    if isinstance(cases, dict):
        for name in VK_CASES:
            case = cases.get(name)
            if not isinstance(case, dict) or case.get('status') != 'PASS' or case.get('problems') != []:
                findings.append(f'Vulkan case {name} did not pass without reported problems')
    return findings


def validate_host(data, required):
    findings = []
    if (data.get('schema') != 'retoque-test-run' or type(data.get('version')) is not int
            or data['version'] != 1 or not isinstance(data.get('run_id'), str) or not data['run_id']
            or data.get('status') != 'PASS'):
        findings.append('strict host aggregate is missing a terminal PASS receipt')
    selected = data.get('selected')
    results = data.get('results')
    if not required or not isinstance(selected, list) or sorted(selected) != sorted(required):
        findings.append('host selected suite set does not match all registered host suites')
    if (not isinstance(results, list) or len(results) != len(required)
            or any(not isinstance(row, dict) or row.get('status') != 'PASS' for row in results)
            or sorted(row.get('name', '') for row in results if isinstance(row, dict)) != sorted(required)):
        findings.append('host evidence contains skipped, missing, repeated, or unsuccessful suites')
    return findings


def validate_suite(data, dll_sha256, *, steady_seconds=0):
    findings = []
    if data.get('status') != 'PASS' or data.get('runtime_coverage') is not True:
        findings.append('provider did not report complete runtime PASS')
    if not dll_sha256 or data.get('dll_sha256') != dll_sha256:
        findings.append('provider DLL hash does not match selected build')
    if steady_seconds and (data.get('requested_steady_seconds') != steady_seconds
                           or (data.get('qualification') or {}).get('status') != 'PASS'):
        findings.append('requested FG duration qualification did not PASS')
    return findings


def validate_hashes(expected, before, after):
    return [f'{name} build hash missing or changed during runtime step'
            for name in ('dll', 'forwarder')
            if not expected.get(name) or before.get(name) != expected[name] or after.get(name) != expected[name]]


def validate_moving(data, expected):
    findings = []
    def number(value, minimum=0):
        return type(value) in (int, float) and math.isfinite(value) and value >= minimum
    def digest(value):
        return isinstance(value, str) and re.fullmatch(r'[0-9a-fA-F]{64}', value) is not None
    timing = data.get('timing')
    trials = ('off-a', 'on-a', 'on-b', 'off-b')
    if type(data.get('schema')) is not int or data['schema'] != 1 or not isinstance(timing, dict) or set(timing) != set(trials):
        findings.append('moving report is missing complete four-trial timing evidence')
    if isinstance(timing, dict):
        for name in trials:
            row = timing.get(name)
            samples = row.get('samples_ms') if isinstance(row, dict) else None
            valid_samples = isinstance(samples, list) and len(samples) == 16 and all(number(x) and x > 0 for x in samples)
            if (not isinstance(row, dict) or type(row.get('n')) is not int or row['n'] != 16
                    or not valid_samples or not number(row.get('p50_ms')) or not number(row.get('p95_ms'))
                    or row['p50_ms'] != sorted(samples)[7] or row['p95_ms'] != sorted(samples)[15]):
                findings.append(f'moving trial {name} has invalid or contradictory timing samples/percentiles')
    controls = data.get('negative_controls')
    if (not isinstance(controls, list) or len(controls) != 2
            or {row.get('control') for row in controls if isinstance(row, dict)} != {'zero-motion', 'disabled-processing'}
            or any(not isinstance(row, dict) or not isinstance(row.get('reason'), str) or not row['reason'].strip()
                   for row in controls)):
        findings.append('moving report is missing its two required negative controls')
    for key, count in (('guide_oracle', 16), ('processing', 16), ('motion_compensated_edit_temporal', 15)):
        rows = data.get(key)
        if not isinstance(rows, list) or len(rows) != count:
            findings.append(f'moving report incomplete {key}')
            continue
        for index, row in enumerate(rows):
            valid = isinstance(row, dict)
            if valid and key == 'guide_oracle':
                valid = (type(row.get('samples')) is int and row['samples'] >= 20
                         and number(row.get('max_error_px')) and row['max_error_px'] <= .1
                         and number(row.get('mean_motion_px'), .02))
            elif valid and key == 'processing':
                valid = (number(row.get('mae')) and row['mae'] > 1e-6
                         and number(row.get('repeat_mae')) and row['repeat_mae'] < row['mae'])
            elif valid:
                valid = (type(row.get('frame')) is int and row['frame'] == 65+index
                         and type(row.get('channels')) is int and row['channels'] > 0
                         and number(row.get('residual_mae')))
            if not valid:
                findings.append(f'moving report invalid {key} sample{index}')
                break
    hashes = data.get('input_sha256')
    if (not isinstance(hashes, dict) or set(hashes) != {str(i) for i in range(64, 80)}
            or any(not isinstance(row, dict) or set(row) != {'source', 'depth', 'motion'}
                   or any(not digest(value) for value in row.values()) for row in hashes.values())):
        findings.append('moving report is missing measured input frame hashes')
    elif hashes['64']['source'] == hashes['79']['source']:
        findings.append('moving source frame hashes describe a frozen source')
    provenance = data.get('provenance')
    files = provenance.get('files') if isinstance(provenance, dict) else None
    files = files if isinstance(files, dict) else {}
    for key, filename in (('dll', 'OptiScaler.dll'), ('forwarder', 'nvngx.dll_dlssnr.dll')):
        record = files.get(filename)
        if (not digest(expected.get(key)) or not isinstance(record, dict)
                or record.get('sha256') != expected[key]):
            findings.append(f'moving staged {key} hash does not match selected build')
    return findings


def validate_replay(data, dll_sha256):
    findings = []
    if (data.get('schema') != 'retoque-replay-seam-report' or type(data.get('version')) is not int
            or data['version'] != 1 or not dll_sha256
            or (data.get('provenance') or {}).get('dll_sha256') != dll_sha256):
        findings.append('replay report schema/build provenance is missing or inconsistent')
    required = {'off-leaves-every-byte', 'off-records-nothing',
                'every-a-model-every-scored-frame', 'every-b-model-every-scored-frame',
                'cadence2-carries', 'cadence4-carries', 'adaptive-reports-intervals', 'repeat-decisions-identical'}
    required.update(f'{name}-no-scored-frame-skipped' for name in
                    ('every-a', 'every-b', 'cadence2', 'cadence4', 'adaptive4'))
    gates = data.get('gates')
    if not isinstance(gates, dict) or not required.issubset(gates):
        findings.append('replay required default scenario gates are missing')
    if isinstance(gates, dict):
        for name, gate in gates.items():
            informational = name == 'repeat-pixels-identical-frames' and isinstance(gate, dict) and gate.get('ok') is None
            if not informational and (not isinstance(gate, dict) or gate.get('ok') is not True):
                findings.append(f'replay gate {name} is unsuccessful or inconclusive')
    return findings


@dataclass
class Step:
    name: str
    command: list[str]
    entry: dict
    kind: str
    receipt: Path | None = None
    required: tuple[str, ...] = ()
    log_paths: tuple[Path, ...] = ()
    steady_seconds: int = 0


def sha(path):
    return hashlib.sha256(Path(path).read_bytes()).hexdigest()


def runtime_hashes():
    paths = {'dll': ROOT/'x64/out/OptiScaler.dll', 'forwarder': ROOT/'x64/out/nvngx.dll_dlssnr.dll'}
    return {name: sha(path) if path.is_file() else None for name, path in paths.items()}


def read_json(path):
    if path.is_symlink() or path.stat().st_size > 2*1024*1024:
        raise ValueError('receipt is a symlink or exceeds 2MiB')
    def finite(value):
        number = float(value)
        if not math.isfinite(number):
            raise ValueError('nonfinite receipt number')
        return number
    data = json.loads(path.read_text(encoding='utf-8'), parse_float=finite, parse_constant=finite)
    if not isinstance(data, dict):
        raise ValueError('receipt must be a JSON object')
    return data


def snapshot(source, target):
    target.parent.mkdir(parents=True, exist_ok=True)
    shutil.copy2(source, target)
    return {'source': str(source), 'path': str(target), 'sha256': sha(target), 'bytes': target.stat().st_size}


def retain_logs(paths, folder):
    records, lines, matched = [], [], 0
    files = [file for path in paths for file in (sorted(path.rglob("*.log")) if path.is_dir() else [path])]
    for index, source in enumerate(dict.fromkeys(files)):
        if not source.is_file() or source.is_symlink():
            continue
        record = snapshot(source, folder/f'{index:03d}-{source.name}')
        records.append(record)
        with source.open(errors='replace') as handle:
            for number, line in enumerate(handle, 1):
                if re.search(r'validation|VUID-|SYNC-HAZARD', line, re.I):
                    matched += 1
                    if len(lines) < 200:
                        lines.append({'log': record['path'], 'line': number, 'text': line.rstrip()[:4096]})
    return records, {'lines': lines, 'matched_line_count': matched, 'truncated': matched > len(lines),
                     'scope': 'bounded raw matching log lines, not a universal validation-clean verdict; full logs retained',
                     'limit': 200}


def validate_inputs(data):
    cases = data.get('cases') or {}
    required = {'current': 'PASS', 'no-camera': 'EXPECTED_FAIL',
                'camera-order': 'EXPECTED_FAIL', 'zero-origin': 'EXPECTED_FAIL'}
    if (data.get('status') != 'PASS' or set(cases) != set(required)
            or any(not isinstance(cases[name], dict) or cases[name].get('status') != status
                   for name, status in required.items())):
        return ['FG input current/mutation cases did not all complete as required']
    return []


def run_step(step, log_dir, expected_hashes, *, stop=None):
    folder = log_dir/step.name
    folder.mkdir(parents=True, exist_ok=True)
    before = runtime_hashes()
    if step.command[0] == "flock":
        Path(step.command[3]).parent.mkdir(parents=True, exist_ok=True)
    # Legacy receipts cannot self-identify the invocation. Remove the generated
    # record before launch and preserve its replacement before any later mode runs.
    if step.receipt is not None:
        step.receipt.unlink(missing_ok=True)
    outcome = runner.run_suite(step.name, step.entry, folder, False, stop=stop, command=step.command)
    after = runtime_hashes()
    findings = []
    data, saved_receipt = outcome.evidence, None
    if step.receipt is not None:
        try:
            if step.receipt.is_symlink():
                raise ValueError('receipt symlink is not accepted')
            if data is None:
                data = read_json(step.receipt)
            saved_receipt = snapshot(step.receipt, folder/'receipt.json')
        except (OSError, ValueError, TypeError) as error:
            findings.append(f'missing or invalid fresh receipt: {error}')
    if outcome.status != 'PASS':
        findings.append(f'runner {outcome.status}: {outcome.note}')
    if step.kind not in ('host', 'build'):
        findings.extend(validate_hashes(expected_hashes, before, after))
    if isinstance(data, dict):
        try:
            if step.kind == 'host':
                findings.extend(validate_host(data, step.required))
            elif step.kind == 'vulkan':
                findings.extend(validate_vulkan(data, expected_hashes.get('dll')))
            elif step.kind == 'provider':
                findings.extend(validate_suite(data, expected_hashes.get('dll'), steady_seconds=step.steady_seconds))
            elif step.kind == 'inputs':
                findings.extend(validate_inputs(data))
            elif step.kind == 'moving':
                findings.extend(validate_moving(data, expected_hashes))
            elif step.kind == 'replay':
                findings.extend(validate_replay(data, expected_hashes.get('dll')))
        except (ValueError, TypeError, AttributeError, KeyError) as error:
            findings.append(f'invalid structured metadata fields: {error}')
    if step.kind == 'build' and any(not after.get(name) for name in ('dll', 'forwarder')):
        findings.append('Release build did not publish both DLL and forwarder')
    logs, validation = retain_logs((outcome.log, *step.log_paths) if outcome.log else step.log_paths,
                                    folder/'logs')
    status = outcome.status if outcome.status in ('ABORTED', 'TIMEOUT') else 'FAIL' if findings else 'PASS'
    return {'name': step.name, 'kind': step.kind, 'command': step.command,
            'execution_anchor': str(step.entry.get('path', step.name)),
            'status': status, 'runner_status': outcome.status, 'returncode': outcome.returncode,
            'run_id': outcome.run_id, 'seconds': outcome.seconds, 'runner_note': outcome.note,
            'fixture_status': data.get('status') if isinstance(data, dict) else None,
            'evidence': data, 'receipt_snapshot': saved_receipt, 'retained_logs': logs,
            'hashes_before': before, 'hashes_after': after, 'findings': findings,
            'validation_findings': validation,
            'evidence_scope': 'required runtime/scenario and seam gates; optional pixel verdicts are separate',
            'quality_qualification': {'status': 'NOT_ISSUED_BY_ORCHESTRATOR',
                                      'original_judge': data.get('judge') if isinstance(data, dict) else None}}


def build_steps(args, log_dir, manifest):
    python = sys.executable
    host_names = tuple(sorted(name for name, entry in manifest.items() if entry['tier'] == 'host'))
    def entry(path, timeout, result=None):
        value = {'path': path, 'tier': 'wine', 'requires': [], 'timeout': timeout}
        if result:
            value['result'] = result
        return value
    anchor = 'fg-provider-d3d12'
    host_report = log_dir/'host-summary.json'
    steps = [Step('host', [python, str(ROOT/'tests/run_all.py'), '--tier', 'host', '--jobs', '4',
                           '--strict', '--report', str(host_report), '--log-dir', str(log_dir/'host-suites')],
                  entry(anchor, 3600), 'host', host_report, host_names)]
    if args.host_only:
        return steps
    dll = str(ROOT/'x64/out/OptiScaler.dll')
    if not args.no_build:
        steps.append(Step('build', [str(ROOT/'build-local.sh'), 'Release'], entry(anchor, 2400), 'build'))
    inputs = ROOT/'tests/fg-inputs-d3d12/artifacts'
    steps.append(Step('fg-inputs', [python, str(ROOT/'tests/fg-inputs-d3d12/run.py')],
                      entry('fg-inputs-d3d12', 1500, 'artifacts/result.json'), 'inputs', inputs/'result.json',
                      log_paths=tuple(inputs/f'{name}-{kind}.log' for name in
                                      ('current', 'no-camera', 'camera-order', 'zero-origin')
                                      for kind in ('build', 'run'))))
    loopback = ROOT/'tests/dlssnr-loopback'
    for name, flag, directory, receipt in (
            ('nr-moving', '--moving-scene', 'moving-run', 'moving-report.json'),
            ('nr-replay', '--replay-nr', 'replay-run', 'replay-report.json')):
        work = loopback/'artifacts'/directory
        steps.append(Step(name, [python, str(loopback/'run.py'), '--dll', dll, '--runtime', 'proton', flag],
                          entry('dlssnr-loopback', 3600), 'moving' if name == 'nr-moving' else 'replay',
                          work/receipt, log_paths=(work,)))
    vulkan = ROOT/'tests/vulkan-present-host'
    lock = Path(os.environ.get('XDG_RUNTIME_DIR') or f'/run/user/{os.getuid()}')/'msvc-prefix.lock'
    command = ['flock', '-n', '-o', str(lock), python, str(vulkan/'run.py'), '--dll', dll,
               '--only', ','.join(VK_CASES)]
    steps.append(Step('vulkan', command, entry('vulkan-present-host', 4200), 'vulkan',
                      vulkan/'artifacts/results.json',
                      log_paths=tuple(path for name in VK_CASES for path in
                                      (vulkan/'artifacts'/f'{name}.log', vulkan/'artifacts/cases'/name))))
    provider = ROOT/'tests/fg-provider-d3d12'
    command = [python, str(provider/'run.py'), '--strict', '--dll', dll]
    if args.fg_steady_seconds:
        command += ['--steady-seconds', str(args.fg_steady_seconds)]
    timeout = 840 + 3*(args.fg_steady_seconds+5) if args.fg_steady_seconds else 840
    steps.append(Step('fg-provider', command, entry('fg-provider-d3d12', timeout, 'artifacts/result.json'),
                      'provider', provider/'artifacts/result.json', steady_seconds=args.fg_steady_seconds,
                      log_paths=(provider/'artifacts/build.log', provider/'artifacts/proton.log',
                                 provider/'artifacts/run/fg-provider.log', provider/'artifacts/run/OptiScaler.log')))
    return steps


def main(argv=None):
    args = parse_args(argv)
    invocation = uuid.uuid4().hex
    log_dir = (args.log_dir or ROOT/'artifacts/rendering').resolve()/invocation
    report_path = args.report.resolve() if args.report else log_dir/'summary.json'
    plan, rows = [], []
    stop = threading.Event()
    interrupted, sealed = 0, False
    expected = {}
    started = datetime.now(timezone.utc).isoformat()

    def interrupt(number, _frame):
        nonlocal interrupted
        if not sealed:
            interrupted = number
            stop.set()

    def persist(status, error=None):
        payload = {'schema': 'retoque-rendering-run', 'version': 1, 'run_id': invocation,
                   'status': status, 'started_at': started,
                   'finished_at': None if status == 'RUNNING' else datetime.now(timezone.utc).isoformat(),
                   'log_dir': str(log_dir), 'requested_fg_steady_seconds': args.fg_steady_seconds,
                   'host_only': args.host_only, 'build_requested': not args.no_build and not args.host_only,
                   'expected_runtime_hashes': expected, 'steps': rows,
                   'runtime_coverage': (not args.host_only and any(row.get('kind') not in ('host', 'build') for row in rows)
                                        and all(row['status'] == 'PASS' for row in rows if row.get('kind') not in ('host', 'build'))),
                   'evidence_scope': {'overall': ('host checks only; runtime not run' if args.host_only else
                                                   'required runtime/scenario and seam gates only'),
                                      'quality_qualification': 'no general quality approval; original optional pixel judgments retained',
                                      'validation': 'raw matching log findings retained; no universal validation-clean claim'}}
        if error is not None:
            payload['error'] = str(error)
        atomic_json(report_path, payload)

    def finish(status, code):
        nonlocal sealed
        written = 'ABORTED' if interrupted else status
        persist(written)
        sealed = True
        final_signal = interrupted
        if final_signal and written != 'ABORTED':
            persist('ABORTED')
        return 128+final_signal if final_signal else code

    previous = {}
    try:
        for number in (signal.SIGINT, signal.SIGTERM):
            previous[number] = signal.signal(number, interrupt)
        log_dir.mkdir(parents=True, exist_ok=False)
        persist('RUNNING')
        manifest = runner.load_manifest()
        plan = build_steps(args, log_dir, manifest)
        rows = [{'name': step.name, 'kind': step.kind, 'command': step.command, 'status': 'NOT_RUN'} for step in plan]
        expected = runtime_hashes()
        persist('RUNNING')
        for index, step in enumerate(plan):
            if stop.is_set():
                break
            rows[index] = run_step(step, log_dir, expected, stop=stop)
            if step.kind == 'build' and rows[index]['status'] == 'PASS':
                expected = rows[index]['hashes_after']
            persist('RUNNING')
            print(f"{rows[index]['status']}: {step.name}", flush=True)
            if rows[index]['status'] != 'PASS':
                break
        passed = all(row['status'] == 'PASS' for row in rows)
        code = finish('PASS' if passed else 'FAIL', 0 if passed else 1)
        print(f'rendering report: {report_path}', flush=True)
        return code
    except BaseException as error:
        stop.set()
        persist('ABORTED' if interrupted or isinstance(error, KeyboardInterrupt) else 'FAIL', error)
        raise
    finally:
        for number, handler in previous.items():
            signal.signal(number, handler)


if __name__ == '__main__':
    sys.exit(main())

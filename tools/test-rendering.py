#!/usr/bin/env python3
"""Serial rendering regressions with invocation-bound, retained evidence."""
from __future__ import annotations

import argparse
from contextlib import contextmanager
from dataclasses import dataclass
from datetime import datetime, timezone
import errno
import hashlib
import json
import math
import os
from pathlib import Path
import re
import shutil
import signal
import stat
import sys
import threading
import uuid

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT/'tests'))
import run_all as runner
from _suite_result import atomic_json

VK_CASES = ('nr', 'nr-route-quiet', 'nr-route-quiet-wdx12', 'nr-route-guides', 'nr-route-scale125')
REPLAY_ROOT_ARTIFACTS = ('calibration.json', 'policy.json', 'corpus.json', 'moving-plan.txt')
REPLAY_VARIANT_ARTIFACTS = ('result.json', 'frames.jsonl', 'contact-sheet.png', 'diff-heatmap.png')
REPLAY_ARTIFACT_LIMITS = {'variants': 32, 'files': 132, 'file_bytes': 16*1024*1024,
                          'summary_bytes': 2*1024*1024, 'total_bytes': 128*1024*1024}


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
    parser.add_argument('--replay-quality', action='store_true',
                        help='select temporal-v1: 480 frames at logical 60 Hz, including a scored cut')
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


def validate_preflight(data, run_id):
    if (data.get('schema') != 'retoque-suite-result' or type(data.get('version')) is not int
            or data['version'] != 1 or not run_id or data.get('run_id') != run_id
            or data.get('status') != 'PASS' or data.get('runtime_coverage') is not False
            or data.get('backend') != 'x11' or data.get('probe') != 'XOpenDisplay/XCloseDisplay'
            or data.get('connected') is not True or not isinstance(data.get('display'), str)
            or not data['display'] or '\0' in data['display']):
        return ['X11 preflight lacks a current versioned connection PASS without runtime coverage']
    return []


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


def validate_replay(data, dll_sha256, *, profile=None):
    findings = []
    if (data.get('schema') != 'retoque-replay-seam-report' or type(data.get('version')) is not int
            or data['version'] != 1 or not dll_sha256
            or (data.get('provenance') or {}).get('dll_sha256') != dll_sha256):
        findings.append('replay report schema/build provenance is missing or inconsistent')
    if profile is not None:
        corpus = data.get('corpus')
        if (not isinstance(corpus, dict) or corpus.get('schema') != 'retoque-replay-corpus-summary'
                or type(corpus.get('version')) is not int or corpus['version'] != 1
                or corpus.get('profile') != profile):
            findings.append('replay corpus does not identify the requested profile')
        elif profile == 'temporal-v1':
            resets = corpus.get('reset_frame_ids')
            if (corpus.get('frames') != 480 or corpus.get('scored_frames') != 400
                    or resets != [1, 361] or any(type(value) is not int for value in resets)):
                findings.append('temporal replay corpus has incomplete frame/scoring/reset coverage')
    required = {'off-leaves-every-byte', 'off-records-nothing',
                'every-a-model-every-scored-frame', 'every-b-model-every-scored-frame',
                'cadence2-carries', 'cadence4-carries', 'adaptive-reports-intervals', 'repeat-decisions-identical'}
    required.update(f'{name}-no-scored-frame-skipped' for name in
                    ('every-a', 'every-b', 'cadence2', 'cadence4', 'adaptive4'))
    if profile == 'temporal-v1':
        required.update(f'{name}-scored-reset-runs-model' for name in
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
    artifact_root: Path | None = None
    replay_profile: str | None = None


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


class ArtifactError(ValueError):
    def __init__(self, reason, detail):
        super().__init__(detail)
        self.reason = reason


@contextmanager
def artifact_directory(path, *, create=False):
    """Open each path component without following symlinks, including ancestors."""
    path = Path(path).absolute()
    if '..' in path.parts or not hasattr(os, 'O_NOFOLLOW') or not hasattr(os, 'O_DIRECTORY'):
        raise ArtifactError('UNSAFE_PATH', 'artifact paths require no-follow directory descriptors')
    flags = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW
    descriptor = os.open(path.anchor, flags)
    try:
        for part in path.parts[1:]:
            if create:
                try:
                    os.mkdir(part, dir_fd=descriptor)
                except FileExistsError:
                    pass
            child = os.open(part, flags, dir_fd=descriptor)
            os.close(descriptor)
            descriptor = child
        yield descriptor
    finally:
        os.close(descriptor)


def artifact_identity(info):
    return (info.st_dev, info.st_ino, info.st_mode, info.st_size, info.st_mtime_ns, info.st_ctime_ns)


def artifact_reason(error):
    if isinstance(error, ArtifactError):
        return error.reason
    if isinstance(error, FileNotFoundError):
        return 'MISSING'
    if isinstance(error, OSError) and error.errno in (errno.ELOOP, errno.ENOTDIR):
        return 'SYMLINK_OR_NOT_DIRECTORY'
    return 'IO_ERROR'


def replay_artifact_inventory(root):
    """Record bounded pre-execution identities; never open raw-frame trees."""
    files, issues, variants = {}, [], []
    limits = REPLAY_ARTIFACT_LIMITS
    def record(directory, filename, relative):
        try:
            files[relative] = artifact_identity(os.stat(filename, dir_fd=directory, follow_symlinks=False))
        except FileNotFoundError:
            pass
        except OSError as error:
            issues.append({'relative_path': relative, 'reason': artifact_reason(error), 'detail': str(error)})
    try:
        with artifact_directory(Path(root)/'judge') as judge:
            for filename in REPLAY_ROOT_ARTIFACTS:
                record(judge, filename, f'judge/{filename}')
            # Even unrelated entries cannot make inventory an unbounded directory scan.
            scan_limit = limits['files'] + limits['variants'] + len(REPLAY_ROOT_ARTIFACTS)
            with os.scandir(judge) as entries:
                for count, entry in enumerate(entries, 1):
                    if count > scan_limit:
                        raise ArtifactError('INVENTORY_LIMIT', f'judge directory exceeds {scan_limit} entries')
                    if entry.name in REPLAY_ROOT_ARTIFACTS:
                        continue
                    if not entry.is_dir(follow_symlinks=False) and not entry.is_symlink():
                        continue
                    if re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_.-]{0,127}', entry.name) is None:
                        raise ArtifactError('UNSAFE_VARIANT', 'unsafe variant directory name')
                    variants.append(entry.name)
                    if len(variants) > limits['variants']:
                        raise ArtifactError('VARIANT_LIMIT', 'judge directory exceeds variant limit')
            for name in sorted(variants):
                try:
                    directory = os.open(name, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW, dir_fd=judge)
                    try:
                        for filename in REPLAY_VARIANT_ARTIFACTS:
                            record(directory, filename, f'judge/{name}/{filename}')
                    finally:
                        os.close(directory)
                except OSError as error:
                    issues.append({'relative_path': f'judge/{name}', 'reason': artifact_reason(error),
                                   'detail': str(error)})
    except FileNotFoundError:
        pass  # A safely observed absent judge directory contains no previous artifacts.
    except (OSError, ArtifactError) as error:
        issues.append({'relative_path': 'judge', 'reason': artifact_reason(error), 'detail': str(error)})
    return {'files': files, 'issues': issues, 'complete': not issues}


def verify_replay_publication(target, output_directory, output, expected_bytes, expected_digest):
    """Bind the reported pathname to the still-owned output and its actual bytes."""
    parent = os.fstat(output_directory)
    expected = artifact_identity(os.fstat(output))
    def check_parent(directory):
        current = os.fstat(directory)
        if (current.st_dev, current.st_ino) != (parent.st_dev, parent.st_ino):
            raise ArtifactError('DESTINATION_CHANGED', 'published artifact parent pathname was replaced')
    try:
        with artifact_directory(target.parent) as named_directory:
            check_parent(named_directory)
            named = os.open(target.name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=named_directory)
            try:
                info = os.fstat(named)
                if (not stat.S_ISREG(info.st_mode) or info.st_size != expected_bytes
                        or artifact_identity(info) != expected):
                    raise ArtifactError('DESTINATION_CHANGED', 'published artifact pathname names a different file')
                total, digest = 0, hashlib.sha256()
                while True:
                    content = os.read(named, min(64*1024, expected_bytes-total+1))
                    if not content:
                        break
                    total += len(content)
                    if total > expected_bytes:
                        raise ArtifactError('DESTINATION_CHANGED', 'published artifact grew during verification')
                    digest.update(content)
                if (total != expected_bytes or digest.hexdigest() != expected_digest
                        or artifact_identity(os.fstat(named)) != expected):
                    raise ArtifactError('DESTINATION_CHANGED', 'published artifact content changed')
                # A rename while verifying bytes must not detach the pathname from
                # either owned descriptor. Reopen every ancestor without following links.
                with artifact_directory(target.parent) as current_directory:
                    check_parent(current_directory)
                    current = os.stat(target.name, dir_fd=current_directory, follow_symlinks=False)
                    if artifact_identity(current) != expected:
                        raise ArtifactError('DESTINATION_CHANGED', 'published artifact pathname changed during verification')
            finally:
                os.close(named)
    except (OSError, ArtifactError) as error:
        raise ArtifactError('DESTINATION_CHANGED', f'published destination verification failed: {error}') from error


def copy_replay_artifact(source, target, before, *, file_limit, remaining_bytes):
    """Copy a fresh stable regular file through descriptors, with bounded memory/I/O."""
    with artifact_directory(source.parent) as directory:
        info = os.stat(source.name, dir_fd=directory, follow_symlinks=False)
        if stat.S_ISLNK(info.st_mode):
            raise ArtifactError('SYMLINK', 'artifact file is a symlink')
        if not stat.S_ISREG(info.st_mode):
            raise ArtifactError('NOT_REGULAR', 'artifact is not a regular file')
        identity = artifact_identity(info)
        if identity == before:
            raise ArtifactError('STALE', 'artifact identity is unchanged from before this invocation')
        if info.st_size > file_limit:
            raise ArtifactError('FILE_LIMIT', f'artifact exceeds {file_limit} bytes')
        if info.st_size > remaining_bytes:
            raise ArtifactError('TOTAL_LIMIT', 'artifact would exceed retained byte limit')
        descriptor = os.open(source.name, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK, dir_fd=directory)
        try:
            if artifact_identity(os.fstat(descriptor)) != identity:
                raise ArtifactError('CHANGED_BEFORE_OPEN', 'artifact changed between inspection and opening')
            with artifact_directory(target.parent, create=True) as output_directory:
                temporary = f'.{target.name}-{uuid.uuid4().hex}.tmp'
                output = os.open(temporary, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW,
                                 0o600, dir_fd=output_directory)
                total, digest = 0, hashlib.sha256()
                try:
                    while True:
                        content = os.read(descriptor, min(64*1024, info.st_size-total+1))
                        if not content:
                            break
                        total += len(content)
                        if total > info.st_size:
                            raise ArtifactError('CHANGED_DURING_COPY', 'artifact grew during copy')
                        digest.update(content)
                        pending = memoryview(content)
                        while pending:
                            written = os.write(output, pending)
                            if written <= 0:
                                raise OSError('artifact copy made no progress')
                            pending = pending[written:]
                    if total != info.st_size or artifact_identity(os.fstat(descriptor)) != identity:
                        raise ArtifactError('CHANGED_DURING_COPY', 'artifact changed during copy')
                    os.fsync(output)
                    os.replace(temporary, target.name, src_dir_fd=output_directory, dst_dir_fd=output_directory)
                    verify_replay_publication(target, output_directory, output, total, digest.hexdigest())
                finally:
                    os.close(output)
                    try:
                        os.unlink(temporary, dir_fd=output_directory)
                    except FileNotFoundError:
                        pass
        finally:
            os.close(descriptor)
    return {'source': str(source), 'path': str(target), 'sha256': digest.hexdigest(), 'bytes': total}


def retain_replay_artifacts(root, folder, before, evidence):
    limits = dict(REPLAY_ARTIFACT_LIMITS)
    records, excluded, findings = [], [], []
    total = 0
    def reject(relative, reason, detail, *, required=True):
        excluded.append({'relative_path': relative, 'reason': reason, 'detail': detail, 'required': required})
        if required or reason not in ('MISSING', 'STALE'):
            findings.append(f'{relative}: {reason}: {detail}')
    judge = evidence.get('judge') if isinstance(evidence, dict) else None
    verdicts = judge.get('verdicts') if isinstance(judge, dict) else None
    variants = []
    if not isinstance(verdicts, dict) or not verdicts:
        reject('judge', 'MISSING_VERDICTS', 'current replay report has no judged variants')
    elif len(verdicts) > limits['variants']:
        reject('judge', 'VARIANT_LIMIT', f'more than {limits["variants"]} reported variants')
    else:
        for name in verdicts:
            if not isinstance(name, str) or re.fullmatch(r'[A-Za-z0-9][A-Za-z0-9_.-]{0,127}', name) is None:
                reject('judge', 'UNSAFE_VARIANT', 'report contains an unsafe variant name')
            else:
                variants.append(name)
    if not before.get('complete'):
        reject('judge', 'INVENTORY_INCOMPLETE', 'pre-execution freshness inventory was incomplete')
        excluded.extend(before.get('issues', []))
    else:
        paths = [(f'judge/{name}', name != 'moving-plan.txt') for name in REPLAY_ROOT_ARTIFACTS]
        paths.extend((f'judge/{name}/{filename}', True) for name in sorted(variants)
                     for filename in REPLAY_VARIANT_ARTIFACTS)
        for relative, required in paths:
            try:
                # A missing optional plan does not consume a file slot.
                if not required:
                    with artifact_directory(Path(root)/'judge') as directory:
                        os.stat('moving-plan.txt', dir_fd=directory, follow_symlinks=False)
                if len(records) >= limits['files']:
                    raise ArtifactError('COUNT_LIMIT', f'more than {limits["files"]} retained files')
                limit = limits['summary_bytes'] if relative.endswith('.json') else limits['file_bytes']
                record = copy_replay_artifact(Path(root)/relative, Path(folder)/relative,
                                              before['files'].get(relative), file_limit=limit,
                                              remaining_bytes=limits['total_bytes']-total)
                record['relative_path'] = relative
                records.append(record)
                total += record['bytes']
            except (OSError, ArtifactError) as error:
                reason = artifact_reason(error)
                if required or reason != 'MISSING':
                    reject(relative, reason, str(error), required=required)
                if reason == 'DESTINATION_CHANGED':
                    # The rejected published copy may still occupy its admitted
                    # budget. Do not accumulate further uncharged copies.
                    break
    return records, {'status': 'FAIL' if findings else 'PASS', 'bytes': total, 'files': len(records),
                     'limits': limits, 'excluded': excluded, 'findings': findings,
                     'scope': 'fresh allowlisted judge artifacts only; raw frames and unreported variants are not copied'}


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
    artifact_before = replay_artifact_inventory(step.artifact_root) if step.artifact_root is not None else None
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
    if step.kind not in ('host', 'build', 'preflight'):
        findings.extend(validate_hashes(expected_hashes, before, after))
    if isinstance(data, dict):
        try:
            if step.kind == 'host':
                findings.extend(validate_host(data, step.required))
            elif step.kind == 'preflight':
                findings.extend(validate_preflight(data, outcome.run_id))
            elif step.kind == 'vulkan':
                findings.extend(validate_vulkan(data, expected_hashes.get('dll')))
            elif step.kind == 'provider':
                findings.extend(validate_suite(data, expected_hashes.get('dll'), steady_seconds=step.steady_seconds))
            elif step.kind == 'inputs':
                findings.extend(validate_inputs(data))
            elif step.kind == 'moving':
                findings.extend(validate_moving(data, expected_hashes))
            elif step.kind == 'replay':
                findings.extend(validate_replay(data, expected_hashes.get('dll'), profile=step.replay_profile))
        except (ValueError, TypeError, AttributeError, KeyError) as error:
            findings.append(f'invalid structured metadata fields: {error}')
    if step.kind == 'build' and any(not after.get(name) for name in ('dll', 'forwarder')):
        findings.append('Release build did not publish both DLL and forwarder')
    functional_status = outcome.status if outcome.status in ('ABORTED', 'TIMEOUT') else 'FAIL' if findings else 'PASS'
    artifacts, retention = [], None
    if step.artifact_root is not None:
        artifacts, retention = retain_replay_artifacts(step.artifact_root, folder/'artifacts', artifact_before, data)
        findings.extend(f'replay artifact retention: {finding}' for finding in retention['findings'])
    logs, validation = retain_logs((outcome.log, *step.log_paths) if outcome.log else step.log_paths,
                                    folder/'logs')
    status = outcome.status if outcome.status in ('ABORTED', 'TIMEOUT') else 'FAIL' if findings else 'PASS'
    return {'name': step.name, 'kind': step.kind, 'command': step.command,
            'execution_anchor': str(step.entry.get('path', step.name)),
            'status': status, 'runner_status': outcome.status, 'returncode': outcome.returncode,
            'functional_status': functional_status,
            'run_id': outcome.run_id, 'seconds': outcome.seconds, 'runner_note': outcome.note,
            'fixture_status': data.get('status') if isinstance(data, dict) else None,
            'evidence': data, 'receipt_snapshot': saved_receipt, 'retained_logs': logs,
            'retained_artifacts': artifacts, 'artifact_retention': retention,
            'hashes_before': before, 'hashes_after': after, 'findings': findings,
            'validation_findings': validation,
            'evidence_scope': ('X11/XWayland connection only; no native Wayland or GPU/runtime coverage'
                               if step.kind == 'preflight' else
                               'required runtime/scenario and seam gates; numerical pixel verdicts are separate'),
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
    preflight_receipt = log_dir/'graphics-preflight/result.json'
    steps.insert(0, Step('graphics-preflight',
                         [python, str(ROOT/'tools/graphics_preflight.py'), '--report', str(preflight_receipt)],
                         {'path': 'rendering-orchestration', 'tier': 'host', 'requires': [], 'timeout': 8},
                         'preflight', preflight_receipt))
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
    replay_flag = '--replay-quality' if args.replay_quality else '--replay-nr'
    replay_directory = 'replay-quality-run' if args.replay_quality else 'replay-run'
    for name, flag, directory, receipt in (
            ('nr-moving', '--moving-scene', 'moving-run', 'moving-report.json'),
            ('nr-replay', replay_flag, replay_directory, 'replay-report.json')):
        work = loopback/'artifacts'/directory
        replay = name == 'nr-replay'
        timeout = 5400 if replay and args.replay_quality else 3600
        profile = ('temporal-v1' if args.replay_quality else 'legacy') if replay else None
        steps.append(Step(name, [python, str(loopback/'run.py'), '--dll', dll, '--runtime', 'proton', flag],
                          entry('dlssnr-loopback', timeout), 'replay' if replay else 'moving',
                          work/receipt, log_paths=(work,), artifact_root=work if replay else None,
                          replay_profile=profile))
    vulkan = ROOT/'tests/vulkan-present-host'
    lock = Path(os.environ.get('XDG_RUNTIME_DIR') or f'/run/user/{os.getuid()}')/'msvc-prefix.lock'
    command = ['flock', '-n', '-o', str(lock), python, str(vulkan/'run.py'), '--dll', dll,
               '--only', ','.join(VK_CASES)]
    steps.append(Step('vulkan', command, entry('vulkan-present-host', 4200), 'vulkan',
                      vulkan/'artifacts/results.json',
                      log_paths=tuple(path for name in VK_CASES for path in
                                      (vulkan/'artifacts'/f'{name}.log', vulkan/'artifacts/cases'/name,
                                       vulkan/'artifacts/cases'/name/'validation.txt',
                                       vulkan/'artifacts/cases'/name/'result.json'))))
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
                   'requested_replay_profile': 'temporal-v1' if args.replay_quality else 'legacy',
                   'host_only': args.host_only, 'build_requested': not args.no_build and not args.host_only,
                   'expected_runtime_hashes': expected, 'steps': rows,
                   'runtime_coverage': (not args.host_only and any(row.get('kind') not in ('host', 'build', 'preflight') for row in rows)
                                        and all(row['status'] == 'PASS' for row in rows if row.get('kind') not in ('host', 'build', 'preflight'))),
                   'evidence_scope': {'overall': ('host checks only; runtime not run' if args.host_only else
                                                   'required runtime/scenario and seam gates only'),
                                      'quality_qualification': 'no general quality approval; original numerical pixel judgments retained',
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

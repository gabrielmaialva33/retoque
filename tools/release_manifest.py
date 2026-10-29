#!/usr/bin/env python3
"""Identify a release archive and validate source-test evidence; never infer runtime PASS."""
from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import re
import stat
import subprocess
import sys
import tempfile
import tomllib
import zipfile

ROOT = Path(__file__).resolve().parents[1]
STATUSES = ('PASS', 'FAIL', 'SKIP', 'TIMEOUT', 'ABORTED', 'INCONCLUSIVE')
MAX_REPORT_BYTES = 4 * 1024 * 1024
MAX_MEMBER_BYTES = 512 * 1024 * 1024
MAX_ARCHIVE_BYTES = 2 * 1024 * 1024 * 1024
MAX_TOTAL_BYTES = 2 * 1024 * 1024 * 1024
MAX_MEMBERS = 4096
REQUIRED_FILES = {'OptiScaler.dll', 'nvngx.dll_dlssnr.dll', 'OptiScaler.ini',
                  'Licenses/RenoDX_ATTRIBUTION.txt'}


def validate_host_report(value: dict, commit: str, required: set[str],
                         expected_run_id: str | None = None) -> dict:
    """A terminal PASS must account for every host suite at the clean selected commit."""
    if not isinstance(value, dict):
        raise ValueError('host report must be an object')
    if (value.get('schema') != 'retoque-test-run' or type(value.get('version')) is not int
            or value['version'] != 1):
        raise ValueError('unsupported host report schema/version')
    if (not re.fullmatch(r'[0-9a-f]{40}(?:[0-9a-f]{24})?', commit)
            or value.get('head') != commit or value.get('dirty') is not False):
        raise ValueError('host report must identify the same clean source commit')
    run_id = value.get('run_id')
    if (not isinstance(run_id, str) or not re.fullmatch(r'[0-9a-f]{32}', run_id)
            or (expected_run_id is not None and run_id != expected_run_id)):
        raise ValueError('host report run identity is absent or mismatched')
    if value.get('status') != 'PASS' or value.get('error') is not None:
        raise ValueError('host report is not a terminal PASS')
    try:
        started = datetime.fromisoformat(value['started_at'])
        finished = datetime.fromisoformat(value['finished_at'])
        if started.utcoffset() is None or finished.utcoffset() is None or finished < started:
            raise ValueError('invalid terminal timestamps')
    except (KeyError, TypeError, ValueError) as error:
        raise ValueError('host report requires ordered timezone-aware terminal timestamps') from error
    selected = value.get('selected')
    if (not required or not isinstance(selected, list) or any(not isinstance(n, str) for n in selected)
            or len(selected) != len(set(selected)) or set(selected) != required):
        raise ValueError('host report selection is not the complete registered host tier')
    results = value.get('results')
    if not isinstance(results, list) or len(results) != len(required):
        raise ValueError('host report has incomplete suite results')
    seen = set()
    for result in results:
        if not isinstance(result, dict):
            raise ValueError('invalid host suite result')
        name = result.get('name')
        if not isinstance(name, str) or name not in required or name in seen:
            raise ValueError('host results contain unknown or duplicate suites')
        seen.add(name)
        if (result.get('tier') != 'host' or result.get('status') != 'PASS'
                or type(result.get('returncode')) is not int or result['returncode'] != 0):
            raise ValueError(f'host suite {name} did not pass')
    counts = value.get('counts')
    expected_counts = {status: len(required) if status == 'PASS' else 0 for status in STATUSES}
    if (not isinstance(counts, dict) or set(counts) != set(STATUSES)
            or any(type(counts[key]) is not int or counts[key] != expected_counts[key] for key in STATUSES)):
        raise ValueError('host aggregate counts disagree with complete suite results')
    # Never export local log paths, arbitrary nested evidence or free-text diagnostics.
    return {'status': 'PASS', 'scope': 'source-contract checks only; not binary/runtime qualification',
            'head': commit, 'run_id': run_id, 'selected': sorted(required), 'counts': expected_counts,
            'started_at': started.astimezone(timezone.utc).isoformat(),
            'finished_at': finished.astimezone(timezone.utc).isoformat()}


def _hash_stream(stream, limit: int) -> tuple[str, int]:
    digest = hashlib.sha256()
    total = 0
    while block := stream.read(1024 * 1024):
        total += len(block)
        if total > limit:
            raise ValueError('artifact exceeds size limit')
        digest.update(block)
    return digest.hexdigest(), total


def archive_inventory(path: Path) -> dict:
    """Hash the same open ZIP and its members, without extracting or trusting path names."""
    with path.open('rb') as stream:
        before = os.fstat(stream.fileno())
        if before.st_size > MAX_ARCHIVE_BYTES:
            raise ValueError('archive exceeds size limit')
        archive_hash, archive_size = _hash_stream(stream, MAX_ARCHIVE_BYTES)
        stream.seek(0)
        files = []
        with zipfile.ZipFile(stream) as archive:
            entries = archive.infolist()
            if len(entries) > MAX_MEMBERS:
                raise ValueError('archive has too many members')
            seen = set()
            total = 0
            for entry in entries:
                name = entry.filename
                stripped = name[:-1] if entry.is_dir() else name
                parts = stripped.split('/')
                if (not stripped or name.startswith('/') or '\\' in name or ':' in name
                        or any(ord(c) < 32 for c in name)
                        or any(p in ('', '.', '..') or p.endswith((' ', '.')) for p in parts)):
                    raise ValueError(f'unsafe archive member: {name!r}')
                key = stripped.casefold()
                if key in seen:
                    raise ValueError(f'case-insensitive archive collision: {name!r}')
                seen.add(key)
                mode = entry.external_attr >> 16
                kind = stat.S_IFMT(mode)
                if kind not in (0, stat.S_IFREG, stat.S_IFDIR) or entry.flag_bits & 1:
                    raise ValueError('archive contains a symlink, special file or encrypted member')
                if parts[-1].casefold() == 'nvngx_dlssnr.dll':
                    raise ValueError('NVIDIA model must never ship in a release')
                if entry.is_dir():
                    continue
                total += entry.file_size
                if entry.file_size > MAX_MEMBER_BYTES or total > MAX_TOTAL_BYTES:
                    raise ValueError('archive contents exceed size limits')
                with archive.open(entry) as member:
                    digest, size = _hash_stream(member, MAX_MEMBER_BYTES)
                if size != entry.file_size:
                    raise ValueError('archive member size mismatch')
                files.append({'path': name, 'size': size, 'sha256': digest})
        present = {entry['path']: entry['size'] for entry in files}
        if any(not present.get(name) for name in REQUIRED_FILES):
            raise ValueError('archive is missing a nonempty required DLL, INI or attribution')
        after = os.fstat(stream.fileno())
        identity = lambda s: (s.st_dev, s.st_ino, s.st_size, s.st_mtime_ns, s.st_ctime_ns)
        if identity(before) != identity(after):
            raise ValueError('archive changed while it was being inspected')
    return {'name': path.name, 'size': archive_size, 'sha256': archive_hash,
            'files': sorted(files, key=lambda entry: entry['path'])}


def _read_report(path: Path) -> tuple[dict, str]:
    with path.open('rb') as stream:
        raw = stream.read(MAX_REPORT_BYTES + 1)
    if len(raw) > MAX_REPORT_BYTES:
        raise ValueError('host report exceeds size limit')

    def unique_pairs(pairs):
        result = {}
        for key, value in pairs:
            if key in result:
                raise ValueError('host report has duplicate JSON keys')
            result[key] = value
        return result

    def reject_number(value):
        raise ValueError(f'nonfinite JSON constant: {value}')

    value = json.loads(raw, object_pairs_hook=unique_pairs, parse_constant=reject_number)
    return value, hashlib.sha256(raw).hexdigest()


def source_identity(root: Path) -> dict:
    def git(*args):
        result = subprocess.run(['git', '-C', str(root), *args], check=True,
                                capture_output=True, text=True, timeout=15)
        return result.stdout.strip()
    head = git('rev-parse', 'HEAD')
    if not re.fullmatch(r'[0-9a-f]{40}(?:[0-9a-f]{24})?', head):
        raise ValueError('cannot establish packaging checkout identity')
    return {'commit': head, 'dirty': bool(git('status', '--porcelain', '--untracked-files=normal')),
            'identity_scope': 'packaging checkout, not inferred from DLLs'}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, default=ROOT)
    parser.add_argument('--archive', type=Path, required=True)
    parser.add_argument('--version', required=True)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--host-report', type=Path)
    parser.add_argument('--require-host', action='store_true')
    parser.add_argument('--expected-host-run-id')
    parser.add_argument('--configuration', choices=('Release', 'Debug', 'ReleaseDebug'), default='Release')
    parser.add_argument('--build-mode', choices=('existing-output', 'workflow-build', 'local-build'),
                        default='existing-output')
    parser.add_argument('--msbuild-version', help='declared numeric MSBuild version, not independent provenance')
    args = parser.parse_args()
    try:
        if not re.fullmatch(r'retoque-v[0-9]+\.[0-9]+\.[0-9]+(?:-[A-Za-z0-9][A-Za-z0-9.-]*)?', args.version):
            raise ValueError('invalid release version')
        if args.msbuild_version and not re.fullmatch(r'[0-9]+(?:\.[0-9]+){1,3}', args.msbuild_version):
            raise ValueError('MSBuild version must be numeric')
        if (args.require_host or args.expected_host_run_id) and args.host_report is None:
            raise ValueError('host report is required')
        output = args.output or Path(str(args.archive) + '.manifest.json')
        inputs = [args.archive, args.host_report] if args.host_report else [args.archive]
        if output.is_symlink() or any(output.resolve() == item.resolve()
                                     or (output.exists() and os.path.samefile(output, item)) for item in inputs):
            raise ValueError('manifest output must not replace an input or a symbolic link')
        source = source_identity(args.root)
        host = {'status': 'NOT_SUPPLIED', 'scope': 'source tests; no report supplied'}
        if args.host_report:
            if source['dirty']:
                raise ValueError('host evidence cannot qualify a dirty packaging checkout')
            with (args.root / 'tests/suites.toml').open('rb') as stream:
                suites = tomllib.load(stream)
            required = {name for name, entry in suites.items() if entry['tier'] == 'host'}
            value, digest = _read_report(args.host_report)
            host = validate_host_report(value, source['commit'], required, args.expected_host_run_id)
            host['report_sha256'] = digest
        inventory = archive_inventory(args.archive)
        manifest = {'schema': 'retoque-release-manifest', 'version': 1,
                    'release': args.version, 'created_at': datetime.now(timezone.utc).isoformat(),
                    'source': source, 'archive': inventory,
                    'build': {'configuration': args.configuration, 'mode': args.build_mode,
                              'msbuild_version': args.msbuild_version,
                              'provenance': 'declared build context; not inferred from archive bytes'},
                    'host_tests': host,
                    'runtime': {'status': 'NOT_RUN',
                                'reason': 'no binary-bound GPU/native Windows/game qualification supplied'}}
        output.parent.mkdir(parents=True, exist_ok=True)
        temporary = None
        try:
            with tempfile.NamedTemporaryFile(mode='w', encoding='utf-8', newline='\n',
                                             dir=output.parent, prefix='.manifest-', delete=False) as stream:
                temporary = Path(stream.name)
                json.dump(manifest, stream, indent=2, sort_keys=True, allow_nan=False)
                stream.write('\n')
                stream.flush()
                os.fsync(stream.fileno())
            os.replace(temporary, output)
        finally:
            if temporary and temporary.exists():
                temporary.unlink()
        print(f'PASS: archive identity recorded in {output}; runtime qualification NOT_RUN')
        return 0
    except (OSError, ValueError, KeyError, TypeError, subprocess.SubprocessError,
            zipfile.BadZipFile, NotImplementedError) as error:
        print(f'REFUSING: {error}', file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())

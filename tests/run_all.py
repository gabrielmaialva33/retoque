#!/usr/bin/env python3
"""Run the repository's test suites and report one summary.

Every suite under tests/ stays a standalone `python3 tests/<name>/run.py`; this
driver only discovers them, decides which can run here, and collects results.

    python3 tests/run_all.py                 # host tier, the default
    python3 tests/run_all.py --list          # registry with tiers, run nothing
    python3 tests/run_all.py --tier all      # host and wine
    python3 tests/run_all.py --only nr-menu,nr-multipass
    python3 tests/run_all.py --skip nr-localization -v

Exit status is 0 when nothing failed. Skips are not failures: a suite whose
toolchain is absent is reported as skipped with the reason. A directory that is
not registered in suites.toml, or a registry entry with no directory, is a
failure, because that is how the suite list silently drifts out of date.
"""

from __future__ import annotations

import argparse
import concurrent.futures
import dataclasses
from datetime import datetime, timezone
import json
import math
import os
import pathlib
import re
import shutil
import signal
import subprocess
import sys
import time
import threading
import uuid
import tomllib

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from _suite_result import atomic_json, write_result

ROOT = pathlib.Path(__file__).resolve().parents[1]
TESTS = ROOT / "tests"
MANIFEST = TESTS / "suites.toml"
TIERS = ("host", "wine", "wip")
STATUSES = ("PASS", "FAIL", "SKIP", "TIMEOUT", "ABORTED", "INCONCLUSIVE")


@dataclasses.dataclass
class Result:
    name: str
    tier: str
    status: str
    seconds: float
    note: str = ""
    log: pathlib.Path | None = None
    run_id: str | None = None
    evidence: dict | None = None
    evidence_source: str = "runner"
    returncode: int | None = None

    @property
    def failed(self) -> bool:
        return self.status in ("FAIL", "TIMEOUT", "ABORTED", "INCONCLUSIVE")


def load_manifest() -> dict[str, dict]:
    with MANIFEST.open("rb") as handle:
        manifest = tomllib.load(handle)
    for name, entry in manifest.items():
        tier = entry.get("tier")
        if tier not in TIERS:
            raise SystemExit(f"suites.toml: {name} has tier {tier!r}, expected one of {TIERS}")
    return manifest


def check_registry(manifest: dict[str, dict]) -> list[str]:
    """Registry and filesystem must agree. Returns human-readable problems."""
    problems = []
    directories = set()
    for name, entry in sorted(manifest.items()):
        try:
            directory = suite_directory(name, entry)
            suite_result_path(directory, entry)
            if directory in directories:
                problems.append(f"{name}: duplicate suite path")
            directories.add(directory)
            domain = entry.get("domain", "legacy")
            if not isinstance(domain, str) or not domain:
                problems.append(f"{name}: domain must be a nonempty string")
            requirements = entry.get("requires", default_requirements(entry["tier"]))
            if (not isinstance(requirements, list) or
                    any(r not in ("python", "cxx", "wine-toolchain") for r in requirements)):
                problems.append(f"{name}: unknown/invalid requires")
        except ValueError as error:
            problems.append(f"{name}: {error}")
            continue
        runner = directory / "run.py"
        if not directory.is_dir():
            problems.append(f"{name}: suite directory does not exist")
        if entry["tier"] == "wip":
            if runner.exists():
                problems.append(f"{name} is tier wip but has a runner; promote it")
        elif not runner.exists():
            problems.append(f"{name} is tier {entry['tier']} but has no run.py")
    for child in TESTS.iterdir():
        if child.is_dir() and not child.name.startswith(("_", ".")):
            if not any(p == child.resolve() or p.is_relative_to(child.resolve()) for p in directories):
                problems.append(f"{child.name}/ is not registered in suites.toml")
    for root, children, files in os.walk(TESTS):
        children[:] = [name for name in children
                       if not name.startswith(("_", ".")) and name != "artifacts"]
        if "run.py" in files:
            runner = pathlib.Path(root) / "run.py"
            if runner.parent.resolve() not in directories:
                problems.append(f"{runner.relative_to(TESTS)}: unregistered runner")
    return problems


def suite_directory(name: str, entry: dict) -> pathlib.Path:
    if not isinstance(name, str) or re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9_.-]*", name) is None:
        raise ValueError("unsafe suite name")
    value = entry.get("path", name)
    if (not isinstance(value, str) or not value or "\\" in value or ":" in value or
            pathlib.PurePosixPath(value).is_absolute() or ".." in pathlib.PurePosixPath(value).parts):
        raise ValueError("unsafe suite path")
    path = (TESTS / value).resolve()
    if path == TESTS.resolve() or not path.is_relative_to(TESTS.resolve()):
        raise ValueError("suite path escapes tests/")
    return path


def default_requirements(tier: str) -> list[str]:
    return ["python", "cxx"] if tier == "host" else ["python", "wine-toolchain"]


def entry_blocker(entry: dict) -> str:
    if entry["tier"] == "wip":
        return "no runner yet"
    for requirement in entry.get("requires", default_requirements(entry["tier"])):
        if requirement == "cxx":
            reason = tier_blocker("host")
        elif requirement == "wine-toolchain":
            reason = tier_blocker("wine")
        elif requirement == "python":
            reason = ""  # This runner is already executing under Python.
        else:
            reason = f"unknown prerequisite: {requirement}"
        if reason:
            return reason
    return ""


def tier_blocker(tier: str) -> str:
    """Why this tier cannot run here, or an empty string if it can."""
    if tier == "wip":
        return "no runner yet"
    if tier == "host":
        if not (shutil.which("g++") and shutil.which("clang++")):
            return "needs g++ and clang++ on PATH"
        return ""
    prefix = pathlib.Path(os.environ.get("WINEPREFIX", "~/.local/opt/msvc-wineprefix")).expanduser()
    msvc = pathlib.Path(os.environ.get("MSVC_BIN", "~/.local/opt/msvc/bin/x64")).expanduser()
    if not shutil.which("wine"):
        return "needs wine on PATH"
    if not prefix.is_dir():
        return f"no Wine prefix at {prefix}"
    if not msvc.is_dir():
        return f"no msvc-wine toolchain at {msvc}"
    return ""


def suite_result_path(directory: pathlib.Path, entry: dict) -> pathlib.Path | None:
    value = entry.get("result")
    if value is None:
        return None
    if (not isinstance(value, str) or not value or "\\" in value or ":" in value or
            pathlib.PurePosixPath(value).is_absolute() or
            any(part in (".", "..") for part in value.split("/"))):
        raise ValueError("unsafe structured result path")
    path = (directory / value).resolve()
    if path == directory.resolve() or not path.is_relative_to(directory.resolve()):
        raise ValueError("structured result path escapes suite")
    return path


def _group_members(group: int) -> dict[int, int]:
    """Identities in a group we created; never select processes by executable name."""
    members = {}
    proc = pathlib.Path("/proc")
    if not proc.is_dir():
        return members
    for path in proc.iterdir():
        if not path.name.isdigit():
            continue
        try:
            fields = (path / "stat").read_text().rsplit(") ", 1)[1].split()
            if int(fields[2]) == group and fields[0] != "Z":
                members[int(path.name)] = int(fields[19])
        except (OSError, ValueError, IndexError):
            continue
    return members


def _stop_child(process: subprocess.Popen, *, grace_seconds: float = 4.0) -> None:
    # A reaped leader no longer reserves its PID/PGID for this invocation.
    if process.returncode is not None:
        return
    # Keep the leader unreaped until the final signal. With default SIGCHLD and
    # this runner as its sole reaper, its PID cannot be reused in this interval.
    try:
        if os.name == "posix":
            os.killpg(process.pid, signal.SIGTERM)
        else:
            process.terminate()
    except ProcessLookupError:
        pass
    if os.name == "posix":
        try:
            proc_pid = int(pathlib.Path("/proc/self/stat").read_text().split(" ", 1)[0])
            can_observe = proc_pid == os.getpid()
        except (OSError, ValueError):
            can_observe = False
        deadline = time.monotonic() + grace_seconds
        while time.monotonic() < deadline:
            if can_observe and not _group_members(process.pid):
                break
            time.sleep(0.02)
        # No poll()/wait() above: even an exited leader still reserves the ID.
        # Without Linux procfs, use the entire grace period before this signal.
        try:
            os.killpg(process.pid, signal.SIGKILL)
        except ProcessLookupError:
            pass
    else:
        try:
            process.wait(timeout=grace_seconds)
        except subprocess.TimeoutExpired:
            process.kill()
    process.wait(timeout=3)


def _read_result(path: pathlib.Path, run_id: str) -> dict:
    if path.stat().st_size > 2 * 1024 * 1024:
        raise ValueError("structured result exceeds 2 MiB")
    def finite_float(value):
        number = float(value)
        if not math.isfinite(number):
            raise ValueError("nonfinite number in structured result")
        return number
    payload = json.loads(path.read_text(encoding="utf-8"),
                         parse_float=finite_float, parse_constant=finite_float)
    if not isinstance(payload, dict):
        raise ValueError("structured result must be an object")
    if (payload.get("schema") != "retoque-suite-result" or
            type(payload.get("version")) is not int or payload["version"] != 1):
        raise ValueError("unsupported structured result schema/version")
    if payload.get("run_id") != run_id:
        raise ValueError("stale structured result: invocation ID does not match")
    if payload.get("status") not in STATUSES:
        raise ValueError("missing or nonterminal structured result status")
    for field in ("reason", "updated_at", "terminated_by"):
        if field in payload and not isinstance(payload[field], str):
            raise ValueError(f"structured result {field} must be a string")
    if "missing" in payload and (not isinstance(payload["missing"], list) or
                                 any(not isinstance(value, str) for value in payload["missing"])):
        raise ValueError("structured result missing must be a list of strings")
    if "runtime_coverage" in payload and not isinstance(payload["runtime_coverage"], bool):
        raise ValueError("structured result runtime_coverage must be a boolean")
    return payload


def run_suite(name: str, entry: dict, log_dir: pathlib.Path, stream: bool,
              *, stop: threading.Event | None = None,
              command: list[str] | None = None) -> Result:
    tier = entry["tier"]
    run_id = uuid.uuid4().hex
    if command is not None and (not isinstance(command, list) or not command or
                                any(not isinstance(value, str) or not value or "\0" in value for value in command)):
        return Result(name, tier, "FAIL", 0.0, "command must be a nonempty list of nonempty strings", run_id=run_id)
    try:
        directory = suite_directory(name, entry)
        result_path = suite_result_path(directory, entry)
    except ValueError as error:
        return Result(name, tier, "FAIL", 0.0, str(error), run_id=run_id)
    if stop is not None and stop.is_set():
        return Result(name, tier, "ABORTED", 0.0, "interrupted before launch", run_id=run_id)
    blocker = entry_blocker(entry)
    if blocker:
        return Result(name, tier, "SKIP", 0.0, blocker, run_id=run_id)
    if os.name == "posix" and signal.getsignal(signal.SIGCHLD) != signal.SIG_DFL:
        return Result(name, tier, "FAIL", 0.0,
                      "process supervision requires default SIGCHLD", run_id=run_id)

    def current_result_path():
        # The suite may have replaced directories/symlinks while it ran.
        current_directory = suite_directory(name, entry)
        if current_directory != directory:
            raise ValueError("suite directory changed during execution")
        return suite_result_path(current_directory, entry)

    log_path = log_dir / f"{name}.log"
    started = time.monotonic()
    interrupted = None
    try:
        with log_path.open("wb") as log:
            process = subprocess.Popen(
                command if command is not None else [sys.executable, str(directory / "run.py")], cwd=ROOT,
                env=dict(os.environ, RETOQUE_TEST_RUN_ID=run_id),
                stdout=None if stream else log, stderr=subprocess.STDOUT,
                start_new_session=(os.name == "posix"), close_fds=True,
            )
            try:
                while process.poll() is None:
                    if stop is not None and stop.is_set():
                        interrupted = "ABORTED"
                        break
                    if time.monotonic() - started >= entry["timeout"]:
                        interrupted = "TIMEOUT"
                        break
                    time.sleep(0.02)
            except BaseException:
                _stop_child(process)
                raise
            if interrupted:
                _stop_child(process)
                note = ("interrupted by runner" if interrupted == "ABORTED" else
                        f"exceeded {entry['timeout']}s")
                evidence = None
                if result_path is not None:
                    try:
                        evidence = write_result(current_result_path(), {
                            "status": interrupted, "reason": note, "runtime_coverage": False,
                            "terminated_by": "suite-runner",
                        }, run_id=run_id)
                    except (OSError, ValueError, TypeError) as error:
                        # A secondary receipt failure cannot erase the known
                        # supervision outcome or permit later serial GPU work.
                        note += f"; interruption receipt unavailable: {error}"
                return Result(name, tier, interrupted, time.monotonic() - started,
                              note, log_path, run_id, evidence, "runner", process.returncode)
            code = process.returncode
    except (OSError, subprocess.SubprocessError) as error:
        return Result(name, tier, "FAIL", time.monotonic() - started,
                      f"process error: {error}", log_path, run_id)
    elapsed = time.monotonic() - started
    evidence = None
    if result_path is not None:
        try:
            evidence = _read_result(current_result_path(), run_id)
        except (OSError, ValueError, TypeError) as error:
            prefix = f"exit {code}; " if code else ""
            return Result(name, tier, "FAIL", elapsed, f"{prefix}invalid result: {error}",
                          log_path, run_id, evidence_source="process-exit", returncode=code)
    source = "structured-result" if evidence is not None else "process-exit"
    if code:
        return Result(name, tier, "FAIL", elapsed, f"exit {code}", log_path, run_id, evidence, source, code)
    if result_path is None:
        return Result(name, tier, "PASS", elapsed, "process-exit evidence", log_path, run_id,
                      evidence_source=source, returncode=code)
    note = evidence.get("reason") or "; ".join(map(str, evidence.get("missing", [])))
    return Result(name, tier, evidence["status"], elapsed, str(note), log_path, run_id, evidence, source, code)


def selected(manifest: dict[str, dict], args) -> list[str]:
    names = sorted(manifest)
    if args.tier != "all":
        names = [n for n in names if manifest[n]["tier"] == args.tier]
    if args.only:
        wanted = {s.strip() for s in args.only.split(",") if s.strip()}
        unknown = wanted - manifest.keys()
        if unknown:
            raise SystemExit(f"--only names no such suite: {', '.join(sorted(unknown))}")
        names = [n for n in sorted(wanted)]
    domain = getattr(args, "domain", None)
    if domain is not None:
        wanted_domains = {part.strip() for part in domain.split(",") if part.strip()}
        known_domains = {entry.get("domain", "legacy") for entry in manifest.values()}
        unknown = wanted_domains - known_domains
        if not wanted_domains or unknown:
            raise SystemExit(f"--domain expects known domains ({', '.join(sorted(known_domains))}); "
                             f"got {domain!r}")
        names = [name for name in names if manifest[name].get("domain", "legacy") in wanted_domains]
    if args.skip:
        dropped = {s.strip() for s in args.skip.split(",") if s.strip()}
        unknown = dropped - manifest.keys()
        if unknown:
            raise SystemExit(f"--skip names no such suite: {', '.join(sorted(unknown))}")
        names = [n for n in names if n not in dropped]
    return names


def registry_groups(manifest: dict[str, dict], names: list[str], group_by: str):
    """Deterministic groups, independent of this machine's available toolchains."""
    groups: dict[str, list[str]] = {}
    for name in sorted(names):
        key = manifest[name].get(group_by, "legacy")
        groups.setdefault(key, []).append(name)
    keys = sorted(groups, key=TIERS.index) if group_by == "tier" else sorted(groups)
    return [(key, groups[key]) for key in keys]


def registry_markdown(manifest: dict[str, dict], names: list[str], group_by: str = "domain") -> str:
    """Render a portable catalog with links relative to tests/, not live PASS claims."""
    from urllib.parse import quote

    def cell(value):
        return str(value).replace("\\", "\\\\").replace("|", "\\|").replace("\n", " ")

    lines = ["# Test suite catalog", "", "Generated from `suites.toml`; do not edit this table by hand.",
             "Regenerate with `python3 tools/check_docs.py --write-catalog` from the repository root.",
             "This is a coverage index, not evidence that any suite or hardware passed.", ""]
    for group, members in registry_groups(manifest, names, group_by):
        lines.extend([f"## {group}", "", "| Suite | Tier | Requirements | Evidence contract | Summary |",
                      "|---|---|---|---|---|"])
        for name in members:
            entry = manifest[name]
            path = quote(entry.get("path", name) + "/README.md", safe="/")
            requirements = ", ".join(entry.get("requires", default_requirements(entry["tier"])))
            evidence = "structured per invocation" if entry.get("result") else "process exit (see suite limits)"
            lines.append(f"| [{cell(name)}]({path}) | {cell(entry['tier'])} | {cell(requirements)} | "
                         f"{evidence} | {cell(entry['summary'])} |")
        lines.append("")
    return "\n".join(lines)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--tier", choices=(*TIERS, "all"), default=None,
                        help="tier to run (default: host; --list defaults to all)")
    parser.add_argument("--only", help="comma-separated suite names, overrides --tier")
    parser.add_argument("--skip", help="comma-separated suite names to leave out")
    parser.add_argument("--domain", help="comma-separated domains; intersects tier/--only selection")
    parser.add_argument("--group-by", choices=("tier", "domain"), default="tier",
                        help="group the --list output (default: tier)")
    parser.add_argument("--format", choices=("text", "markdown"), default="text",
                        help="--list output format; markdown is deterministic and relative to tests/")
    parser.add_argument("--jobs", type=int, default=4,
                        help="parallel host suites (default: 4); the wine tier is always serial")
    parser.add_argument("--list", action="store_true", help="print the registry and exit")
    parser.add_argument("-v", "--verbose", action="store_true",
                        help="stream suite output instead of capturing it to a log")
    parser.add_argument("--log-dir", type=pathlib.Path,
                        help="where to write per-suite logs (default: a temp dir under $TMPDIR)")
    parser.add_argument("--report", type=pathlib.Path,
                        help="atomic JSON report (default: <log-dir>/summary.json)")
    parser.add_argument("--strict", action="store_true",
                        help="fail when a selected suite is skipped")
    args = parser.parse_args()
    if not args.list and (args.format != "text" or args.group_by != "tier"):
        parser.error("--format and --group-by are listing options; use --list")
    if args.tier is None:
        args.tier = "all" if args.list else "host"

    manifest = load_manifest()
    problems = check_registry(manifest)
    if args.list and not problems:
        names = selected(manifest, args)
        if args.format == "markdown":
            print(registry_markdown(manifest, names, args.group_by), end="")
        else:
            for group, members in registry_groups(manifest, names, args.group_by):
                print(f"## {group}")
                for name in members:
                    entry = manifest[name]
                    blocker = entry_blocker(entry)
                    state = "ready" if not blocker else f"unavailable: {blocker}"
                    print(f"{name:<22} {entry['tier']:<5} {state}")
                    print(f"{'':<22} {'':<5} {entry['summary']}")
        return 0

    if problems:
        for problem in problems:
            print(f"registry: {problem}", file=sys.stderr)
        print("\nFix tests/suites.toml before running the suites.", file=sys.stderr)
        return 1

    names = selected(manifest, args)

    log_dir = args.log_dir or pathlib.Path(
        os.environ.get("TMPDIR", "/tmp")) / f"optiscaler-tests-{os.getpid()}"
    log_dir.mkdir(parents=True, exist_ok=True)

    parallel = [n for n in names if manifest[n]["tier"] == "host"]
    serial = [n for n in names if manifest[n]["tier"] != "host"]
    results: list[Result] = []
    report_path = args.report or log_dir / "summary.json"
    run_id = uuid.uuid4().hex
    started_at = datetime.now(timezone.utc).isoformat()
    stop = threading.Event()
    interrupted_signal = 0
    completion_sealed = False

    def interrupt(number, _frame):
        nonlocal interrupted_signal
        if completion_sealed:
            return
        interrupted_signal = number
        stop.set()

    previous_handlers = {}
    for number in (signal.SIGINT, signal.SIGTERM):
        previous_handlers[number] = signal.signal(number, interrupt)

    def git_value(*command):
        try:
            value = subprocess.run(["git", "-C", str(ROOT), *command], capture_output=True,
                                   text=True, timeout=3)
            return value.stdout.strip() if value.returncode == 0 else None
        except (OSError, subprocess.SubprocessError):
            return None

    head = git_value("rev-parse", "HEAD")
    dirty_text = git_value("status", "--porcelain")

    def persist(status, *, error=None):
        counts = {state: sum(result.status == state for result in results) for state in STATUSES}
        payload = {
            "schema": "retoque-test-run", "version": 1, "run_id": run_id,
            "status": status, "pid": os.getpid(), "head": head,
            "dirty": None if dirty_text is None else bool(dirty_text),
            "started_at": started_at,
            "finished_at": None if status == "RUNNING" else datetime.now(timezone.utc).isoformat(),
            "selected": names, "counts": counts,
            "results": [
                {**dataclasses.asdict(result),
                 "log": str(result.log) if result.log is not None else None}
                for result in results
            ],
        }
        if error is not None:
            payload["error"] = str(error)
        atomic_json(report_path, payload)

    def finish(status, exit_code):
        nonlocal completion_sealed
        written_status = "ABORTED" if interrupted_signal else status
        persist(written_status)
        # Close signal observation only after the first terminal write returns.
        # The assignment is the completion boundary: a handler before it records
        # interruption; a handler after it cannot alter this invocation's exit.
        # Snapshot after sealing so no signal can slip between snapshot/closure.
        completion_sealed = True
        final_signal = interrupted_signal
        if final_signal and written_status != "ABORTED":
            persist("ABORTED")
        return 128 + final_signal if final_signal else exit_code

    print(f"running {len(names)} suite(s); logs in {log_dir}\n", flush=True)
    try:
        persist("RUNNING")
        if not names:
            exit_code = finish("INCONCLUSIVE", 1 if args.strict else 0)
            print(f"nothing selected; report: {report_path}")
            return exit_code
        if parallel:
            workers = 1 if args.verbose else max(1, min(args.jobs, len(parallel)))
            with concurrent.futures.ThreadPoolExecutor(max_workers=workers) as pool:
                try:
                    futures = {pool.submit(run_suite, name, manifest[name], log_dir, args.verbose,
                                           stop=stop): name for name in parallel}
                    for future in concurrent.futures.as_completed(futures):
                        try:
                            result = future.result()
                        except Exception as error:
                            name = futures[future]
                            result = Result(name, manifest[name]["tier"], "FAIL", 0.0,
                                            f"runner error: {error}")
                            stop.set()
                        results.append(result)
                        print(f"  {result.status:<12} {result.name}", flush=True)
                        persist("RUNNING")
                except BaseException:
                    # Signal workers before executor.__exit__ waits for them.
                    stop.set()
                    raise
        for name in serial:
            result = run_suite(name, manifest[name], log_dir, args.verbose, stop=stop)
            results.append(result)
            print(f"  {result.status:<12} {result.name}", flush=True)
            persist("RUNNING")

        results.sort(key=lambda result: (TIERS.index(result.tier), result.name))
        width = max(len(result.name) for result in results)
        print("\n" + "-" * (width + 35))
        for result in results:
            note = f"  {result.note}" if result.note else ""
            print(f"{result.status:<12} {result.name:<{width}}  {result.seconds:6.1f}s{note}")
        print("-" * (width + 35))
        counts = {state: sum(result.status == state for result in results) for state in STATUSES}
        print(" ".join(f"{state.lower()}={count}" for state, count in counts.items() if count))

        failed = [result for result in results if result.failed]
        skipped = any(result.status == "SKIP" for result in results)
        status = "FAIL" if failed else "INCONCLUSIVE" if skipped else "PASS"
        exit_code = finish(status, 1 if failed or (args.strict and skipped) else 0)
        print(f"report: {report_path}")
        return exit_code
    except BaseException as error:
        stop.set()
        persist("ABORTED" if isinstance(error, KeyboardInterrupt) else "FAIL", error=error)
        raise
    finally:
        for number, handler in previous_handlers.items():
            signal.signal(number, handler)


if __name__ == "__main__":
    sys.exit(main())

"""Atomic, invocation-bound results shared by standalone rendering suites."""
from __future__ import annotations

from contextlib import contextmanager
from datetime import datetime, timezone
import json
import os
from pathlib import Path
import signal
import tempfile
import uuid

_STANDALONE_ID = uuid.uuid4().hex


def atomic_json(path: Path, payload: dict) -> None:
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = None
    try:
        with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=path.parent,
                                         prefix="." + path.name + "-", suffix=".tmp",
                                         delete=False) as handle:
            temporary = Path(handle.name)
            json.dump(payload, handle, indent=2, allow_nan=False)
            handle.write("\n")
            handle.flush()
            os.fsync(handle.fileno())
        os.replace(temporary, path)
    finally:
        if temporary is not None:
            temporary.unlink(missing_ok=True)


def write_result(path: Path, payload: dict, *, run_id: str | None = None) -> dict:
    result = dict(payload)
    result.update(schema="retoque-suite-result", version=1,
                  run_id=run_id or os.environ.get("RETOQUE_TEST_RUN_ID") or _STANDALONE_ID,
                  updated_at=datetime.now(timezone.utc).isoformat())
    atomic_json(path, result)
    return result


class _SuiteInterrupted(KeyboardInterrupt):
    def __init__(self, signum):
        self.signum = signum


@contextmanager
def result_run(path: Path):
    """Let fixture-owned finally blocks clean up before recording an interruption."""
    def interrupted(signum, _frame):
        raise _SuiteInterrupted(signum)

    previous = {}
    try:
        for number in (signal.SIGINT, signal.SIGTERM):
            previous[number] = signal.signal(number, interrupted)
        yield
    except KeyboardInterrupt as error:
        number = getattr(error, "signum", signal.SIGINT)
        write_result(path, {"status": "ABORTED", "reason": f"interrupted by signal {number}",
                            "runtime_coverage": False})
        raise SystemExit(128 + number)
    finally:
        for number, handler in previous.items():
            signal.signal(number, handler)

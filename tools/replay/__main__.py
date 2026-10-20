"""python3 -m tools.replay: inspect the v1 offline-replay design for wire details."""

import argparse
import json
from pathlib import Path
import sys
from .format import json_file
from .judge import calibrate, judge, clear_artifacts, write_report


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    calibration_mode = bool(argv and argv[0] == "calibrate")
    if calibration_mode:
        argv = argv[1:]
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture", required=True, type=Path)
    parser.add_argument("--reference", required=True, type=Path)
    parser.add_argument("--out", required=True, type=Path)
    parser.add_argument("--policy", type=Path)
    if calibration_mode:
        parser.add_argument("--repeat", required=True, action="append", type=Path)
    else:
        parser.add_argument("--candidate", required=True, type=Path)
        parser.add_argument("--calibration", type=Path)
    args = parser.parse_args(argv)
    inputs = [args.capture, args.reference]
    inputs += args.repeat if calibration_mode else [args.candidate]
    if args.policy:
        inputs.append(args.policy)
    if not calibration_mode and args.calibration:
        inputs.append(args.calibration)
    if calibration_mode:
        if args.out.resolve() in {p.resolve() for p in inputs}:
            parser.error("calibration output cannot overwrite an input manifest")
    elif args.out.resolve() in {p.resolve().parent for p in inputs}:
        parser.error("report directory must be separate from input directories")
    if calibration_mode:
        args.out.unlink(missing_ok=True)
        try:
            report = calibrate(
                args.capture,
                args.reference,
                args.repeat,
                json_file(args.policy) if args.policy else None,
            )
        except (
            ValueError,
            KeyError,
            TypeError,
            IndexError,
            OverflowError,
            OSError,
        ) as error:
            report = dict(
                schema="retoque-replay-calibration",
                version=1,
                verdict="INVALID",
                reasons=[str(error)],
            )
        args.out.parent.mkdir(parents=True, exist_ok=True)
        args.out.write_text(json.dumps(report, indent=2, allow_nan=False) + "\n")
        return 2 if report.get("verdict") == "INVALID" else 0
    clear_artifacts(args.out)
    try:
        policy = json_file(args.policy) if args.policy else None
        calibration = json_file(args.calibration) if args.calibration else None
        report = judge(
            args.capture, args.reference, args.candidate, calibration, policy
        )
    except (ValueError, OSError) as error:
        report = dict(
            schema="retoque-replay-result",
            version=1,
            verdict="INVALID",
            reasons=[str(error)],
            frames=[],
        )
    write_report(report, args.out)
    print(report["verdict"] + ": " + "; ".join(report["reasons"]))
    return {"PASS": 0, "FAIL": 1, "INVALID": 2, "INCONCLUSIVE": 3}[report["verdict"]]


if __name__ == "__main__":
    raise SystemExit(main())

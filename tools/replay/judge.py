"""Streaming paired-run analysis and explicitly calibrated, per-region acceptance."""

import json
from pathlib import Path
from .format import load_capture, load_run, read_plane, need, number, integer, sha
from .metrics import rgb, frame_metrics, validate_guides, mean, percentile
from .png import preview, heatmap, contact, write_png

ERROR_METRICS = (
    "golden_mae",
    "golden_max",
    "golden_bad_fraction",
    "ghost_mae",
    "disocclusion_mae",
    "detail_error",
    "pulse_p95",
    "temporal_mae",
)


def policy_values(policy=None):
    policy = {} if policy is None else policy
    need(isinstance(policy, dict), "policy must be object")
    need(
        set(policy) <= {"version", "min_coverage", "pixel_tolerance", "floors"},
        "unknown policy key",
    )
    need(
        type(policy.get("version", 1)) is int and policy.get("version", 1) == 1,
        "policy version",
    )
    coverage = number(policy.get("min_coverage", 0.25), "min_coverage", 0)
    tolerance = number(policy.get("pixel_tolerance", 0.01), "pixel_tolerance", 0)
    need(coverage <= 1, "min_coverage > 1")
    floors = policy.get("floors", {})
    need(
        isinstance(floors, dict) and set(floors) <= set(ERROR_METRICS),
        "unknown floor metric",
    )
    return dict(
        version=1,
        min_coverage=coverage,
        pixel_tolerance=tolerance,
        floors={
            key: number(floors.get(key, 0), "floor " + key, 0) for key in ERROR_METRICS
        },
    )


def analyze(capture_path, reference_path, candidate_path, policy=None):
    policy = policy_values(policy)
    capture = load_capture(capture_path)
    reference, candidate = (
        load_run(reference_path, capture),
        load_run(candidate_path, capture),
    )
    need(
        reference["environment"] == candidate["environment"],
        "model/driver/GPU/runtime mismatch",
    )
    need(
        all(f["kind"] == "every" and f["interval"] == 1 for f in reference["frames"]),
        "reference must run every frame",
    )
    capture["_pixel_tolerance"] = policy["pixel_tolerance"]
    previous = None
    rows = []
    first = last = worst = None
    worst_error = -1
    for source_f, ref_f, cand_f in zip(
        capture["frames"], reference["frames"], candidate["frames"]
    ):
        source = rgb(
            read_plane(capture["_root"], source_f["color"]),
            capture["color_space"],
            source_f["pre_exposure"],
        )
        depth = read_plane(capture["_root"], source_f["depth"])
        motion = read_plane(capture["_root"], source_f["motion"])
        if source_f["exposure"] is not None:
            exposure = read_plane(capture["_root"], source_f["exposure"])
            need(all(v > 0 for v in exposure.data), "invalid exposure texture")
        oracle = (
            read_plane(capture["_root"], source_f["motion_oracle"])
            if "motion_oracle" in source_f
            else None
        )
        validate_guides(
            capture,
            source_f,
            motion,
            depth,
            previous["frame"] if previous else None,
            oracle,
        )
        ref_image = rgb(
            read_plane(reference["_root"], ref_f["output"]),
            capture["color_space"],
            source_f["pre_exposure"],
        )
        output = rgb(
            read_plane(candidate["_root"], cand_f["output"]),
            capture["color_space"],
            source_f["pre_exposure"],
        )
        old = (
            previous
            if previous and previous["frame"]["score"] and source_f["score"]
            else None
        )
        metrics, retained = frame_metrics(
            capture, source_f, source, depth, motion, output, ref_image, old
        )
        old_kind = previous["kind"] if previous else None
        previous = dict(retained, kind=cand_f["kind"])
        if not source_f["score"]:
            continue
        row = dict(
            id=source_f["id"],
            present=source_f["present"],
            time_ms=source_f["time_ms"],
            kind=cand_f["kind"],
            interval=cand_f["interval"],
            transition=(old_kind + "->" + cand_f["kind"]) if old else None,
            gpu_ms=cand_f["gpu_ms"],
            reference_gpu_ms=ref_f["gpu_ms"],
            regions=metrics,
        )
        rows.append(row)
        visual = dict(
            id=source_f["id"],
            kind=cand_f["kind"],
            time_ms=source_f["time_ms"],
            panels=[preview(im) for im in (source, ref_image, output)],
        )
        if first is None:
            first = visual
        last = visual
        error = metrics["all"]["golden_max"]
        if error > worst_error:
            worst_error = error
            worst = dict(
                visual, heatmap=heatmap(output, ref_image, policy["pixel_tolerance"])
            )
    summary = {}
    pulse_groups = {}
    for region in ("all", *capture["regions"]):
        values = [r["regions"][region] for r in rows]

        def vals(key):
            return [v[key] for v in values if v[key] is not None]

        summary[region] = {
            key: mean(vals(key))
            for key in ERROR_METRICS
            if key not in ("golden_max", "pulse_p95")
        }
        summary[region]["golden_max"] = max(vals("golden_max"), default=None)
        pulse = [abs(v) for v in vals("pulse")]
        summary[region].update(
            pulse_p95=percentile(pulse, 0.95),
            coverage=mean(vals("coverage")) or 0.0,
            temporal_pairs=sum(v["temporal_mae"] is not None for v in values),
            pulse_pairs=len(pulse),
            candidate_gradient=mean(vals("candidate_gradient")),
            reference_gradient=mean(vals("reference_gradient")),
        )
        groups = {}
        for row in rows:
            value = row["regions"][region]["pulse"]
            if value is not None:
                for group in (row["kind"], row["transition"]):
                    groups.setdefault(group, []).append(abs(value))
        pulse_groups[region] = {
            key: dict(
                count=len(v),
                p50=percentile(v, 0.5),
                p95=percentile(v, 0.95),
                maximum=max(v),
            )
            for key, v in groups.items()
        }
    timings = {}
    for group in ("all", "every", "model", "carried"):
        values = [r["gpu_ms"] for r in rows if group == "all" or r["kind"] == group]
        timings[group] = dict(
            count=len(values),
            p50=percentile(values, 0.5),
            p95=percentile(values, 0.95),
            maximum=max(values, default=None),
        )
    visuals = []
    for image in (first, worst, last):
        if image and image["id"] not in [r["id"] for r in visuals]:
            visuals.append(image)
    return dict(
        schema="retoque-replay-result",
        version=1,
        capture_sha256=capture["_sha256"],
        reference_sha256=reference["_sha256"],
        candidate_sha256=candidate["_sha256"],
        environment=candidate["environment"],
        reference_producer=reference["producer"],
        candidate_producer=candidate["producer"],
        reference_settings=reference["settings"],
        candidate_settings=candidate["settings"],
        policy=policy,
        metrics=summary,
        counts=dict(scored=len(rows), **candidate["_counts"]),
        timing=timings,
        pulse_groups=pulse_groups,
        frames=rows,
        _visuals=visuals,
        _worst=worst,
        _candidate_every=all(f["kind"] == "every" for f in candidate["frames"]),
    )


def calibrate(capture, reference, repeats, policy=None):
    need(
        len(repeats) >= 5,
        "calibration needs at least five independent reference repeats",
    )
    observations = []
    identities = set()
    base = None
    for path in repeats:
        report = analyze(capture, reference, path, policy)
        need(report["_candidate_every"], "A/A repeats must run every frame")
        need(
            report["candidate_sha256"] != report["reference_sha256"]
            and report["candidate_sha256"] not in identities,
            "duplicate calibration trial",
        )
        identities.add(report["candidate_sha256"])
        need(
            report["reference_settings"] == report["candidate_settings"]
            and report["reference_producer"]["dll_sha256"]
            == report["candidate_producer"]["dll_sha256"],
            "A/A settings or DLL mismatch",
        )
        need(not insufficient(report), "calibration has insufficient temporal coverage")
        for region, metrics in report["metrics"].items():
            need(
                all(metrics[k] is not None for k in ERROR_METRICS),
                "calibration metric unavailable",
            )
        observations.append(
            dict(run_sha256=report["candidate_sha256"], metrics=report["metrics"])
        )
        base = report
    bounds = {
        region: {
            key: max(o["metrics"][region][key] for o in observations)
            + base["policy"]["floors"][key]
            for key in ERROR_METRICS
        }
        for region in base["metrics"]
    }
    return dict(
        schema="retoque-replay-calibration",
        version=1,
        capture_sha256=base["capture_sha256"],
        reference_sha256=base["reference_sha256"],
        environment=base["environment"],
        policy=base["policy"],
        samples=len(observations),
        observations=observations,
        bounds=bounds,
    )


def insufficient(report):
    reasons = []
    if report["counts"]["scored"] < 3:
        reasons.append("fewer than three scored frames")
    for name, metrics in report["metrics"].items():
        if metrics["temporal_pairs"] < 2 or metrics["pulse_pairs"] < 2:
            reasons.append(name + ": fewer than two usable temporal/pulse pairs")
        if metrics["coverage"] < report["policy"]["min_coverage"]:
            reasons.append(name + ": insufficient correspondence coverage")
    return reasons


def validate_calibration(calibration, report):
    need(
        isinstance(calibration, dict)
        and calibration.get("schema") == "retoque-replay-calibration"
        and type(calibration.get("version")) is int
        and calibration["version"] == 1,
        "invalid calibration schema",
    )
    for key in ("capture_sha256", "reference_sha256", "environment", "policy"):
        need(calibration.get(key) == report[key], "calibration mismatch: " + key)
    integer(calibration.get("samples"), "calibration samples", 5)
    observations = calibration.get("observations")
    need(
        isinstance(observations, list) and len(observations) == calibration["samples"],
        "calibration observations missing",
    )
    need(
        all(isinstance(o, dict) for o in observations),
        "calibration observation objects required",
    )
    ids = [sha(o.get("run_sha256")) for o in observations]
    need(
        len(set(ids)) == len(ids) and report["reference_sha256"] not in ids,
        "duplicate calibration observations",
    )
    bounds = calibration.get("bounds")
    need(
        isinstance(bounds, dict) and set(bounds) == set(report["metrics"]),
        "calibration region mismatch",
    )
    for region in bounds:
        need(set(bounds[region]) == set(ERROR_METRICS), "calibration metrics mismatch")
        for key in ERROR_METRICS:
            bound = number(bounds[region][key], "calibration bound", 0)
            values = [
                number(o["metrics"][region][key], "calibration observation", 0)
                for o in observations
            ]
            expected = max(values) + report["policy"]["floors"][key]
            need(
                bound == expected, "calibration envelope inconsistent with observations"
            )
    return bounds


def judge(capture, reference, candidate, calibration=None, policy=None):
    try:
        report = analyze(capture, reference, candidate, policy)
        missing = insufficient(report)
        failures = []
        bounds = None
        if calibration is None:
            missing.append("no A/A calibration")
        else:
            bounds = validate_calibration(calibration, report)
            for region, metrics in report["metrics"].items():
                for name, bound in bounds[region].items():
                    value = metrics[name]
                    if value is not None and value > bound:
                        failures.append(f"{region}.{name}: {value:.9g} > {bound:.9g}")
        report.update(
            verdict="FAIL" if failures else "INCONCLUSIVE" if missing else "PASS",
            reasons=failures + missing,
            bounds=bounds,
        )
        return report
    except (
        ValueError,
        KeyError,
        TypeError,
        IndexError,
        OverflowError,
        OSError,
    ) as error:
        return dict(
            schema="retoque-replay-result",
            version=1,
            verdict="INVALID",
            reasons=[str(error)],
            frames=[],
        )


def clear_artifacts(out):
    out = Path(out)
    out.mkdir(parents=True, exist_ok=True)
    for name in (
        "result.json",
        "frames.jsonl",
        "contact-sheet.png",
        "diff-heatmap.png",
    ):
        (out / name).unlink(missing_ok=True)
    return out


def write_report(report, out):
    out = clear_artifacts(out)
    frames = report.get("frames", [])
    (out / "frames.jsonl").write_text(
        "".join(json.dumps(r, allow_nan=False) + "\n" for r in frames)
    )
    visuals = report.get("_visuals", [])
    if visuals:
        w, h, pixels = contact(visuals)
        metadata = dict(
            columns=["input", "every-frame reference", "candidate"],
            mapping="fixed Reinhard then sRGB",
            rows=[{k: r[k] for k in ("id", "kind", "time_ms")} for r in visuals],
        )
        write_png(out / "contact-sheet.png", w, h, pixels, metadata)
        worst = report["_worst"]
        w, h, pixels = worst["heatmap"]
        write_png(
            out / "diff-heatmap.png",
            w,
            h,
            pixels,
            dict(
                frame=worst["id"],
                mapping="blue=0 red>=pixel_tolerance; max absolute RGB error",
                pixel_tolerance=report["policy"]["pixel_tolerance"],
            ),
        )
    public = {
        k: v for k, v in report.items() if not k.startswith("_") and k != "frames"
    }
    (out / "result.json").write_text(
        json.dumps(public, indent=2, allow_nan=False) + "\n"
    )

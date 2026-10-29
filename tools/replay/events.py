"""Bounded offline event analysis of already judged replay manifests.

This reads metadata, not pixel planes: fidelity remains the existing judge's
verdict. A retention receipt attests to the judged artifact bytes at its absolute
retained paths; it does not assign the current checkout's identity to an older
rendering run. Relocated copies can be analyzed without a historical receipt.
"""

import argparse
import hashlib
import json
import math
from pathlib import Path, PurePosixPath
import re
import sys

from .format import boolean, descriptor, geometry, integer, need, number, sha, vector
from .judge import ERROR_METRICS, METRICS_REVISION, insufficient, policy_values
from .metrics import mean, percentile

MAX_INPUT_BYTES = 32 * 1024 * 1024
MAX_FRAMES = 10_000
METRIC_FIELDS = (
    "golden_mae", "golden_max", "golden_bad_fraction", "ghost_mae",
    "disocclusion_mae", "detail_error", "candidate_gradient",
    "reference_gradient", "pulse", "temporal_mae", "coverage",
)
PAIRED_FIELDS = ("pulse_delta", "paired_abs_delta", "amplitude_excess")
KINDS = ("every", "model", "carried", "skipped")


def _object(value, name):
    need(isinstance(value, dict), name + ": object required")
    return value


def _text(value, name):
    need(isinstance(value, str) and value.strip(), name + ": nonempty string required")
    return value


def _name(value):
    need(isinstance(value, str) and re.fullmatch(r"[A-Za-z0-9_-]{1,64}", value), "invalid variant/region name")
    return value


def _schema(value, name, versions=(1,)):
    _object(value, name)
    need(value.get("schema") == name and type(value.get("version")) is int and value["version"] in versions,
         "unsupported " + name + " schema/version")


def _pairs(items):
    result = {}
    for key, value in items:
        need(key not in result, "duplicate JSON key: " + key)
        result[key] = value
    return result


def _finite_tree(value):
    if isinstance(value, float):
        need(math.isfinite(value), "nonfinite JSON number")
    elif isinstance(value, dict):
        for item in value.values():
            _finite_tree(item)
    elif isinstance(value, list):
        for item in value:
            _finite_tree(item)


def _decode(data):
    try:
        result = json.loads(data, object_pairs_hook=_pairs)
        _finite_tree(result)
    except (UnicodeError, RecursionError) as error:
        raise ValueError("invalid or excessively nested JSON") from error
    return _object(result, "JSON")


def _read(path, role, inputs, *, lines=False):
    path = Path(path).resolve()
    with path.open("rb") as stream:
        need(stream.seek(0, 2) <= MAX_INPUT_BYTES, "input exceeds 32 MiB limit: " + role)
        stream.seek(0)
        data = stream.read(MAX_INPUT_BYTES + 1)
    need(len(data) <= MAX_INPUT_BYTES, "input exceeds 32 MiB limit: " + role)
    entry = dict(role=role, path=str(path), sha256=hashlib.sha256(data).hexdigest(), bytes=len(data))
    inputs.append(entry)
    if not lines:
        return _decode(data), entry
    raw_rows = data.splitlines()
    need(len(raw_rows) <= MAX_FRAMES, "frame rows exceed 10000 limit")
    need(all(row.strip() for row in raw_rows), "empty frame JSONL row")
    return [_decode(row) for row in raw_rows], entry


def _capture(capture, path):
    """Validate the existing wire metadata without opening image files."""
    _schema(capture, "retoque-replay-capture", (1, 2))
    need(capture.get("complete") is True, "incomplete capture")
    v2 = capture["version"] == 2
    need(not v2 or capture.get("finalized") is True, "capture not finalized")
    need(capture.get("mode") == "nr-boundary" and capture.get("clock") == "recorded-evaluation", "capture mode/clock mismatch")
    need(capture.get("color_space") in ("linear-rec709", "srgb-rec709"), "unsupported color space")
    need(capture.get("color_jitter") in ("removed", "present"), "invalid color jitter")
    _text(capture.get("source"), "capture source")
    provenance = _object(capture.get("provenance"), "capture provenance")
    _text(provenance.get("producer"), "capture producer")
    sha(provenance.get("shader_sha256"))
    depth = _object(capture.get("depth"), "depth")
    motion = _object(capture.get("motion"), "motion")
    need(depth.get("kind") in ("device", "linear"), "invalid depth convention")
    boolean(depth.get("inverted"), "depth inverted")
    number(depth.get("threshold"), "depth threshold", 0)
    need(motion.get("direction") == "current-to-previous" and motion.get("units") in ("pixels", "uv"), "invalid motion convention")
    boolean(motion.get("includes_jitter"), "motion includes_jitter")
    if v2:
        need(motion.get("scale") == "per-frame" and motion["units"] == "pixels", "v2 motion scale mismatch")
    else:
        vector(motion.get("scale"), "motion scale")
    frames = capture.get("frames")
    need(isinstance(frames, list) and 1 <= len(frames) <= MAX_FRAMES, "invalid capture frame count")
    shape = None
    shapes = {}
    previous = None
    scoring = False
    for frame in frames:
        _object(frame, "capture frame")
        integer(frame.get("id"), "frame id", 1)
        integer(frame.get("present"), "frame present", 1)
        number(frame.get("time_ms"), "frame time", 0)
        boolean(frame.get("score"), "frame score")
        boolean(frame.get("reset"), "frame reset")
        if previous is None:
            need(frame["reset"] and (not v2 or frame["id"] == 1), "capture needs reset-prefixed history and valid initial ID")
        else:
            need(frame["id"] == previous["id"] + 1 and frame["present"] >= previous["present"] and frame["time_ms"] > previous["time_ms"], "nonconsecutive frame identities or clocks")
        need(not scoring or frame["score"], "warmup after scored frame")
        scoring = scoring or frame["score"]
        vector(frame.get("jitter"), "frame jitter")
        need(number(frame.get("pre_exposure"), "pre_exposure", 0) > 0, "zero pre_exposure")
        need(number(frame.get("exposure_value"), "exposure_value", 0) > 0, "zero exposure")
        _object(frame.get("params"), "frame params")
        need(frame.get("completion") == "fence-complete" and "exposure" in frame, "uncertified/incomplete capture frame")
        for key, channels in (("color", (3, 4)), ("depth", (1,)), ("motion", (2,)), ("motion_oracle", (2,))):
            if key == "motion_oracle" and key not in frame:
                continue
            desc = _object(frame.get(key), "frame " + key)
            _, w, h, c, _ = descriptor(path.parent, desc)
            if shape is None:
                shape = (w, h)
            if v2:
                need(desc.get("encoding") == "raw-dxgi", "v2 plane encoding mismatch")
                current_shape = (w, h, desc["dxgi_format"])
                need(key not in shapes or shapes[key] == current_shape, "v2 plane shape changed")
                shapes[key] = current_shape
                need(key != "motion_oracle" or (c == 2 and (w, h) == shape), "motion_oracle shape mismatch")
            else:
                need(desc.get("encoding") in ("raw", "pfm") and c in channels and (w, h) == shape, "v1 plane shape/encoding mismatch")
        if frame["exposure"] is not None:
            need(descriptor(path.parent, frame["exposure"])[3] == 1, "exposure must be scalar")
        if v2:
            geometry(frame, capture)
        number(frame.get("motion_tolerance_px", .05), "motion tolerance", 0)
        previous = frame
    need(scoring, "capture has no scored frames")
    regions = _object(capture.get("regions"), "capture regions")
    need("all" not in regions, "all is a reserved region")
    for name, rect in regions.items():
        _name(name)
        need(isinstance(rect, list) and len(rect) == 4, "region rectangle required")
        x, y, w, h = [integer(v, "region coordinate") for v in rect]
        need(w > 0 and h > 0 and x + w <= shape[0] and y + h <= shape[1], "region out of bounds")
    return shape


def _phases(capture):
    corpus = capture["provenance"].get("corpus")
    _schema(corpus, "retoque-replay-corpus-summary")
    profile = _text(corpus.get("profile"), "corpus profile")
    need(corpus.get("source") == capture["source"], "corpus source mismatch")
    for metadata in (capture, capture["provenance"]):
        need("profile" not in metadata or metadata["profile"] == profile, "corpus profile mismatch")
    frames = capture["frames"]
    need(integer(corpus.get("frames"), "corpus frames", 1) == len(frames), "corpus frame count mismatch")
    need(integer(corpus.get("scored_frames"), "corpus scored frames") == sum(f["score"] for f in frames), "corpus scored count mismatch")
    reset_ids = corpus.get("reset_frame_ids")
    need(isinstance(reset_ids, list) and all(type(i) is int for i in reset_ids) and reset_ids == [f["id"] for f in frames if f["reset"]], "corpus reset IDs mismatch")
    for key in ("nominal_duration_ms", "timestamp_span_ms", "scored_duration_ms"):
        number(corpus.get(key), "corpus " + key, 0)
    need(math.isclose(corpus["timestamp_span_ms"], frames[-1]["time_ms"] - frames[0]["time_ms"], rel_tol=1e-12, abs_tol=1e-9), "corpus timestamp span mismatch")
    need(corpus["scored_duration_ms"] <= corpus["nominal_duration_ms"], "corpus scored duration exceeds total")
    phases = corpus.get("phases")
    need(isinstance(phases, list) and 1 <= len(phases) <= len(frames), "corpus phase coverage missing")
    names, mapping = set(), []
    position = 0
    for phase in phases:
        _object(phase, "corpus phase")
        name = _text(phase.get("name"), "phase name")
        need(name not in names, "duplicate phase name")
        names.add(name)
        count = integer(phase.get("frames"), "phase frames", 1)
        first = integer(phase.get("first_frame_id"), "phase first frame", 1)
        last = integer(phase.get("last_frame_id"), "phase last frame", 1)
        need(position + count <= len(frames), "phase extends past capture")
        chunk = frames[position:position + count]
        need(first == chunk[0]["id"] and last == chunk[-1]["id"], "phase coverage/order mismatch")
        need(integer(phase.get("scored_frames"), "phase scored frames") == sum(f["score"] for f in chunk), "phase scored count mismatch")
        number(phase.get("nominal_duration_ms"), "phase duration", 0)
        mapping.extend([name] * count)
        position += count
    need(position == len(frames), "phase coverage incomplete")
    need(math.isclose(sum(p["nominal_duration_ms"] for p in phases), corpus["nominal_duration_ms"], rel_tol=1e-12, abs_tol=1e-8), "phase/corpus duration mismatch")
    return corpus, mapping


def _run(raw, capture, capture_hash, shape, path):
    _schema(raw, "retoque-replay-run", (capture["version"],))
    need(raw.get("complete") is True and raw.get("capture_sha256") == capture_hash, "incomplete/mismatched run capture identity")
    env = _object(raw.get("environment"), "run environment")
    sha(env.get("model_sha256"))
    for name in ("gpu", "driver", "runtime"):
        _text(env.get(name), "environment " + name)
    producer = _object(raw.get("producer"), "run producer")
    sha(producer.get("dll_sha256"))
    _text(producer.get("trial"), "producer trial")
    _text(producer.get("commit"), "producer commit")
    _object(raw.get("settings"), "run settings")
    frames = raw.get("frames")
    need(isinstance(frames, list) and len(frames) == len(capture["frames"]), "missing/extra raw frames")
    counts = dict(model=0, carried=0)
    for frame, source in zip(frames, capture["frames"]):
        _object(frame, "raw frame")
        need(type(frame.get("id")) is int and frame["id"] == source["id"], "raw frame identity mismatch")
        for key in ("present", "time_ms"):
            if key in frame:
                (integer if key == "present" else number)(frame[key], "raw frame " + key)
                need(frame[key] == source[key], "raw frame " + key + " mismatch")
        kind = frame.get("kind")
        need(kind in KINDS, "invalid raw frame kind")
        skipped = kind == "skipped"
        if skipped:
            _text(frame.get("skip"), "skip reason")
            need(not source["score"], "scored frame skipped")
        interval = integer(frame.get("interval"), "interval", 0 if skipped else 1)
        need((interval <= 4 if skipped else interval == 1 if kind == "every" else 2 <= interval <= 4), "raw kind/interval mismatch")
        requested = integer(frame.get("requested_passes"), "requested passes", 0 if skipped else 1)
        completed = integer(frame.get("completed_passes"), "completed passes")
        need(requested <= 3 and completed == (0 if kind in ("carried", "skipped") else requested), "partial/invalid model pass coverage")
        need(frame.get("completion") == "fence-complete", "uncertified raw frame")
        number(frame.get("gpu_ms"), "raw frame gpu_ms", 0)
        if "model_result" in frame:
            integer(frame["model_result"], "model result")
            need(kind not in ("every", "model") or frame["model_result"] == 1, "unsuccessful model result")
        if "recorded" in frame:
            boolean(frame["recorded"], "recorded")
            need(skipped or frame["recorded"], "model/reuse frame not recorded")
        desc = _object(frame.get("output"), "run output")
        _, w, h, channels, _ = descriptor(path.parent, desc)
        need((w, h) == shape, "run output shape mismatch")
        if capture["version"] == 2:
            need(desc.get("encoding") == "raw-dxgi" and desc.get("dxgi_format") == source["color"]["dxgi_format"], "v2 output format mismatch")
        else:
            need(desc.get("encoding") in ("raw", "pfm") and channels in (3, 4), "v1 output encoding mismatch")
        if source["score"]:
            counts["carried" if kind == "carried" else "model"] += 1
    requirements = _object(raw.get("requirements"), "run requirements")
    for key in counts:
        minimum = integer(requirements.get("min_" + key), "min_" + key)
        need(counts[key] >= minimum, "missing requested " + key + " coverage")
    return counts


def _same(actual, expected, label):
    """Check receipt aggregates; tolerate only floating recomputation roundoff."""
    if isinstance(expected, dict):
        need(isinstance(actual, dict) and set(actual) == set(expected), label + " fields mismatch")
        for key, value in expected.items():
            _same(actual[key], value, label + "." + str(key))
    elif isinstance(expected, list):
        need(isinstance(actual, list) and len(actual) == len(expected), label + " length mismatch")
        for index, value in enumerate(expected):
            _same(actual[index], value, label)
    elif type(expected) is float:
        number(actual, label)
        need(math.isclose(actual, expected, rel_tol=1e-12, abs_tol=1e-15), label + " mismatch")
    else:
        need(type(actual) is type(expected) and actual == expected, label + " mismatch")


def _row_metrics(metrics, paired, region):
    _object(metrics, "frame region " + region)
    need(set(metrics) == set(METRIC_FIELDS) | {"temporal_pair"}, "frame region metrics fields mismatch")
    boolean(metrics["temporal_pair"], "temporal_pair")
    need(metrics["temporal_pair"] == paired, "frame temporal pair mismatch")
    nullable = {"pulse", "temporal_mae", "disocclusion_mae", "coverage"}
    for key in METRIC_FIELDS:
        value = metrics[key]
        if value is None:
            need(key in nullable, "spatial frame metric unavailable: " + key)
            continue
        number(value, "frame metric " + key, None if key == "pulse" else 0)
        if key in ("coverage", "golden_bad_fraction"):
            need(value <= 1, "frame fraction exceeds 1")
        need(paired or key not in nullable, "unpaired/reset frame has temporal metric")
    if paired:
        need(metrics["coverage"] is not None and metrics["disocclusion_mae"] is not None, "paired frame missing coverage/disocclusion")
        need((metrics["temporal_mae"] is None) == (metrics["coverage"] == 0), "temporal MAE/coverage mismatch")


def _aggregates(rows, regions):
    metrics, pulse_groups = {}, {}
    for region in regions:
        values = [r["regions"][region] for r in rows]

        def vals(key):
            return [v[key] for v in values if v[key] is not None]

        metrics[region] = {key: mean(vals(key)) for key in ERROR_METRICS if key not in ("golden_max", "pulse_p95")}
        pulse = [abs(v) for v in vals("pulse")]
        metrics[region].update(golden_max=max(vals("golden_max"), default=None),
                               pulse_p95=percentile(pulse, .95), coverage=mean(vals("coverage")) or 0.0,
                               temporal_pairs=len(vals("temporal_mae")), pulse_pairs=len(pulse),
                               candidate_gradient=mean(vals("candidate_gradient")), reference_gradient=mean(vals("reference_gradient")))
        groups = {}
        for row in rows:
            value = row["regions"][region]["pulse"]
            if value is not None:
                for key in (row["kind"], row["transition"]):
                    groups.setdefault("null" if key is None else key, []).append(abs(value))
        pulse_groups[region] = {key: dict(count=len(v), p50=percentile(v, .5), p95=percentile(v, .95), maximum=max(v)) for key, v in groups.items()}
    timing = {}
    for kind in ("all", "every", "model", "carried"):
        values = [r["gpu_ms"] for r in rows if kind == "all" or r["kind"] == kind]
        timing[kind] = dict(count=len(values), p50=percentile(values, .5), p95=percentile(values, .95), maximum=max(values, default=None))
    return metrics, pulse_groups, timing


def _result(result, rows, capture, raw, reference, hashes, counts, regions):
    _schema(result, "retoque-replay-result")
    need(type(result.get("metrics_revision")) is int and result["metrics_revision"] == METRICS_REVISION, "metrics revision mismatch")
    for key in ("capture", "reference", "candidate"):
        need(result.get(key + "_sha256") == hashes[key], key + " result identity mismatch")
    for key, value in dict(environment=raw["environment"], reference_producer=reference["producer"], candidate_producer=raw["producer"], reference_settings=reference["settings"], candidate_settings=raw["settings"]).items():
        _same(result.get(key), value, "result " + key)
    policy = policy_values(result.get("policy"))
    _same(result.get("policy"), policy, "result policy")
    scored = [(i, frame) for i, frame in enumerate(capture["frames"]) if frame["score"]]
    need(len(rows) == len(scored), "missing/extra scored frame rows")
    for row, (index, frame) in zip(rows, scored):
        run_frame = raw["frames"][index]
        ref_frame = reference["frames"][index]
        for key in ("id", "present", "time_ms"):
            if key in ("id", "present"):
                integer(row.get(key), "frame " + key, 1)
            else:
                number(row.get(key), "frame time", 0)
            need(row[key] == frame[key], "scored frame " + key + " identity mismatch")
        for key in ("kind", "interval", "gpu_ms"):
            _same(row.get(key), run_frame[key], "frame " + key)
        _same(row.get("reference_gpu_ms"), ref_frame["gpu_ms"], "frame reference_gpu_ms")
        previous = index > 0 and raw["frames"][index-1]["kind"] != "skipped" and reference["frames"][index-1]["kind"] != "skipped"
        transition = raw["frames"][index-1]["kind"] + "->" + run_frame["kind"] if previous else None
        need(row.get("transition") == transition, "frame transition mismatch")
        paired = bool(previous and not frame["reset"] and frame["time_ms"] - capture["frames"][index-1]["time_ms"] < 250)
        need(isinstance(row.get("regions"), dict) and set(row["regions"]) == set(regions), "frame regions mismatch")
        for region in regions:
            _row_metrics(row["regions"][region], paired, region)
    metrics, pulse_groups, timing = _aggregates(rows, regions)
    _same(result.get("counts"), dict(scored=len(rows), **counts), "result counts")
    for key, value in (("metrics", metrics), ("pulse_groups", pulse_groups), ("timing", timing)):
        _same(result.get(key), value, "result aggregate " + key)
    bounds = _object(result.get("bounds"), "calibrated result bounds")
    need(set(bounds) == set(regions), "calibrated bound regions mismatch")
    failures = []
    for region in regions:
        need(isinstance(bounds[region], dict) and set(bounds[region]) == set(ERROR_METRICS), "calibrated bound metrics mismatch")
        for key in ERROR_METRICS:
            bound = number(bounds[region][key], "calibrated bound", 0)
            value = metrics[region][key]
            if value is not None and value > bound:
                failures.append((region, key))
    missing = insufficient(result)
    verdict = "FAIL" if failures else "INCONCLUSIVE" if missing else "PASS"
    need(result.get("verdict") == verdict, "fidelity verdict disagrees with recorded bounds/metrics")
    reasons = result.get("reasons")
    need(isinstance(reasons, list) and all(isinstance(r, str) and r.strip() for r in reasons), "invalid fidelity reasons")
    need(bool(reasons) == (verdict != "PASS"), "fidelity reasons/verdict mismatch")


def _retention(qualification, artifacts, inputs):
    """Bind historical proof to the consumed retained paths, never to source."""
    if qualification is None:
        return dict(validation="current-manifests-and-input-hashes", historically_retained=False,
                    retained_artifact_count=0, scope="Current capture/run hashes bind the judged metadata; historical retention is not claimed.")
    receipt, _ = _read(qualification, "qualification", inputs)
    _schema(receipt, "retoque-rendering-run")
    _text(receipt.get("run_id"), "qualification run_id")
    steps = receipt.get("steps")
    need(isinstance(steps, list) and all(isinstance(s, dict) for s in steps), "qualification steps missing")
    replay = [s for s in steps if s.get("name") == "nr-replay"]
    need(len(replay) == 1, "qualification nr-replay step missing/duplicate")
    step = replay[0]
    need(isinstance(step.get("artifact_retention"), dict) and step["artifact_retention"].get("status") == "PASS", "qualification artifact retention did not PASS")
    retained = step.get("retained_artifacts")
    need(isinstance(retained, list), "retained artifacts missing")
    by_relative = {}
    for record in retained:
        _object(record, "retained artifact")
        relative = _text(record.get("relative_path"), "retained relative_path")
        path = PurePosixPath(relative)
        need(not path.is_absolute() and ".." not in path.parts and "\\" not in relative, "invalid retained relative path")
        need(relative not in by_relative, "duplicate retained artifact")
        retained_path = Path(_text(record.get("path"), "retained path"))
        need(retained_path.is_absolute(), "retained artifact path must be absolute")
        _text(record.get("source"), "retained source")
        sha(record.get("sha256"))
        integer(record.get("bytes"), "retained bytes")
        by_relative[relative] = record
    for relative, consumed in artifacts.items():
        record = by_relative.get(relative)
        need(record is not None, "retained artifact receipt missing: " + relative)
        need(Path(record["path"]).resolve() == Path(consumed["path"]).resolve(), "retained artifact path mismatch: " + relative)
        need(record["sha256"] == consumed["sha256"] and record["bytes"] == consumed["bytes"], "retained artifact bytes/hash mismatch: " + relative)
    return dict(validation="retained-artifact-bytes-and-manifest-hashes", historically_retained=True,
                retained_artifact_count=len(artifacts), qualification_run_id=receipt["run_id"],
                qualification_status=receipt.get("status"),
                scope="Only the consumed candidate/control result.json and frames.jsonl bytes at the receipt's absolute retained paths are verified against historical retention. Relocated copies can be analyzed without a receipt as current inputs. Capture/run manifests are bound by those result hashes; images are not reread.")


def _timeline(capture, raw, phases):
    rows = []
    age, streak, epoch = None, 0, 0
    for index, (frame, work, phase) in enumerate(zip(capture["frames"], raw["frames"], phases)):
        if frame["reset"]:
            age, streak, epoch = None, 0, epoch + 1
        preceding = streak
        kind = work["kind"]
        if kind in ("every", "model"):
            age, streak = 0, 0
        elif kind == "carried":
            age = None if age is None else age + 1
            streak += 1
        else:
            age, streak = None, 0
        labels = []
        if frame["reset"]:
            labels.append("reset")
        if index == 0 or phase != phases[index-1]:
            labels.append("phase-change")
        previous = raw["frames"][index-1] if index else None
        if previous:
            if work["interval"] != previous["interval"]:
                labels.append("interval-change")
            if kind != previous["kind"]:
                labels.append("execution-transition")
            if (kind == "carried") != (previous["kind"] == "carried"):
                labels.append("reuse-transition")
        rows.append(dict(id=frame["id"], ordinal=index, present=frame["present"], time_ms=frame["time_ms"],
                         score=frame["score"], reset=frame["reset"], phase=phase,
                         kind=kind, interval=work["interval"], gpu_ms=work["gpu_ms"],
                         requested_passes=work["requested_passes"], completed_passes=work["completed_passes"],
                         history_age=age, preceding_carried_streak=preceding, history_epoch=epoch,
                         decision_transition=previous["kind"] + "->" + kind if previous else None,
                         events=labels))
    return rows


def _paired(candidate, control, frame):
    a, b = candidate["pulse"], control["pulse"]
    if frame["reset"]:
        reason = "reset"
    elif not candidate["temporal_pair"] or not control["temporal_pair"]:
        reason = "temporal-pair-unavailable"
    elif a is None and b is None:
        reason = "both-pulses-unavailable"
    elif a is None:
        reason = "candidate-pulse-unavailable"
    elif b is None:
        reason = "control-pulse-unavailable"
    else:
        reason = None
    return dict(pulse_delta=None if reason else a-b,
                paired_abs_delta=None if reason else abs(a-b),
                amplitude_excess=None if reason else abs(a)-abs(b),
                unavailable_reason=reason)


def _stats(rows, values):
    available = [(row["id"], value) for row, value in zip(rows, values) if value is not None]
    values = [value for _, value in available]
    low = min(available, key=lambda item: item[1]) if available else (None, None)
    high = max(available, key=lambda item: item[1]) if available else (None, None)
    return dict(count=len(values), missing=len(rows)-len(values), mean=mean(values),
                p50=percentile(values, .5), p95=percentile(values, .95), p99=percentile(values, .99),
                minimum=low[1], minimum_frame_id=low[0], maximum=high[1], maximum_frame_id=high[0],
                descriptive_only=True, small_sample=len(values) < 20)


def _summary(rows, regions):
    result = dict(frame_count=len(rows), kinds={k: sum(r["kind"] == k for r in rows) for k in KINDS},
                  gpu_ms={name: _stats(rows, [r[key] for r in rows]) for name, key in (("candidate", "gpu_ms"), ("control", "control_gpu_ms"), ("reference", "reference_gpu_ms"))},
                  regions={})
    for region in regions:
        result["regions"][region] = {
            side: {key: _stats(rows, [r["regions"][region][side][key] for r in rows]) for key in (PAIRED_FIELDS if side == "paired" else METRIC_FIELDS)}
            for side in ("candidate", "control", "paired")
        }
    return result


def analyze(capture_path, runs_dir, judge_dir, candidate, *, control="every-b", reference="every-a", qualification=None, window=4, top=8):
    """Reconcile complete metadata, pair pulses, and produce descriptive events."""
    integer(window, "event window")
    integer(top, "top frames", 1)
    need(window <= 32 and top <= 16, "event window/top exceeds limit")
    names = (candidate, control, reference)
    for name in names:
        _name(name)
    need(len(set(names)) == 3, "candidate/control/reference must be distinct runs")
    capture_path, runs_dir, judge_dir = (Path(p).resolve() for p in (capture_path, runs_dir, judge_dir))
    inputs = []
    capture, capture_input = _read(capture_path, "capture", inputs)
    shape = _capture(capture, capture_path)
    corpus, phases = _phases(capture)
    regions = ["all", *capture["regions"]]
    runs, run_inputs, counts = {}, {}, {}
    for name in names:
        path = runs_dir / name / "run.json"
        runs[name], run_inputs[name] = _read(path, "run:" + name, inputs)
        counts[name] = _run(runs[name], capture, capture_input["sha256"], shape, path)
    ref, base, cand = (runs[n] for n in (reference, control, candidate))
    need(all(r["environment"] == ref["environment"] for r in runs.values()), "run environment mismatch")
    trials = [r["producer"]["trial"] for r in runs.values()]
    need(len(set(trials)) == 3 and len({v["sha256"] for v in run_inputs.values()}) == 3, "runs need distinct fresh-process identities")
    for raw in (ref, base):
        need(all(f["kind"] in ("every", "skipped") for f in raw["frames"]), "reference/control must run every frame")
    need(ref["settings"] == base["settings"] and ref["producer"]["dll_sha256"] == base["producer"]["dll_sha256"], "A/A control settings/DLL mismatch")
    results, judged, artifacts = {}, {}, {}
    for name in (candidate, control):
        result_path = judge_dir / name / "result.json"
        rows_path = judge_dir / name / "frames.jsonl"
        results[name], result_input = _read(result_path, "result:" + name, inputs)
        judged[name], rows_input = _read(rows_path, "frames:" + name, inputs, lines=True)
        artifacts["judge/" + name + "/result.json"] = result_input
        artifacts["judge/" + name + "/frames.jsonl"] = rows_input
    # Verify receipts before parsing metric semantics so edited retained bytes have
    # an explicit historical-integrity failure, even when their JSON still parses.
    provenance = _retention(qualification, artifacts, inputs)
    for name in (candidate, control):
        _result(results[name], judged[name], capture, runs[name], ref,
                dict(capture=capture_input["sha256"], reference=run_inputs[reference]["sha256"], candidate=run_inputs[name]["sha256"]), counts[name], regions)
    for key in ("policy", "bounds", "environment", "metrics_revision"):
        _same(results[candidate][key], results[control][key], "paired result " + key)
    need(results[control]["verdict"] == "PASS", "every-frame control must have calibrated PASS")
    timeline = _timeline(capture, cand, phases)
    frames = []
    scored = [r for r in timeline if r["score"]]
    for frame, a, b in zip(scored, judged[candidate], judged[control]):
        row = dict(frame, transition=a["transition"], reference_gpu_ms=a["reference_gpu_ms"],
                   control_gpu_ms=b["gpu_ms"], control_kind=b["kind"], control_interval=b["interval"], regions={})
        for region in regions:
            ca, cb = a["regions"][region], b["regions"][region]
            row["regions"][region] = dict(candidate=ca, control=cb, paired=_paired(ca, cb, frame))
        frames.append(row)
    worst, selected = {}, {}
    for region in regions:
        ranked = sorted((r for r in frames if r["regions"][region]["paired"]["paired_abs_delta"] is not None), key=lambda r: (-r["regions"][region]["paired"]["paired_abs_delta"], r["id"]))[:top]
        worst[region] = [dict(id=r["id"], ordinal=r["ordinal"], phase=r["phase"], kind=r["kind"], transition=r["transition"], history_age=r["history_age"], preceding_carried_streak=r["preceding_carried_streak"], gpu_ms=r["gpu_ms"], control_gpu_ms=r["control_gpu_ms"], **r["regions"][region]["paired"]) for r in ranked]
        for row in ranked:
            row["events"].append("worst:" + region)
            selected.setdefault(row["ordinal"], []).append(region)
    windows = []
    for ordinal, selected_regions in sorted(selected.items()):
        context = timeline[max(0, ordinal-window):min(len(timeline), ordinal+window+1)]
        windows.append(dict(anchor_frame_id=timeline[ordinal]["id"], regions=selected_regions,
                            frame_ids=[r["id"] for r in context], scored_frame_ids=[r["id"] for r in context if r["score"]],
                            first_ordinal=context[0]["ordinal"], last_ordinal=context[-1]["ordinal"],
                            frames=context))
    groups = dict(by_phase={p["name"]: [] for p in corpus["phases"]}, by_kind={k: [] for k in KINDS}, by_history_age={}, by_event={})
    for row in frames:
        groups["by_phase"][row["phase"]].append(row)
        groups["by_kind"][row["kind"]].append(row)
        age = "unknown" if row["history_age"] is None else str(row["history_age"])
        groups["by_history_age"].setdefault(age, []).append(row)
        for label in row["events"]:
            groups["by_event"].setdefault(label, []).append(row)
    summaries = dict(all=_summary(frames, regions))
    summaries.update({key: {name: _summary(rows, regions) for name, rows in grouped.items()} for key, grouped in groups.items()})
    identities = {role: dict(name=name, sha256=run_inputs[name]["sha256"], producer=runs[name]["producer"], settings=runs[name]["settings"])
                  for role, name in (("candidate", candidate), ("control", control), ("reference", reference))}
    report = dict(schema="retoque-replay-events", version=1, metrics_revision=METRICS_REVISION,
                capture_sha256=capture_input["sha256"], corpus=corpus, capture_provenance=capture["provenance"],
                identities=identities, environment=cand["environment"], policy=results[candidate]["policy"], bounds=results[candidate]["bounds"],
                provenance=provenance, inputs=inputs, options=dict(window=window, top=top),
                validity=dict(functional="PASS", functional_scope="Complete recorded metadata and model-pass checks; no renderer or pixel validation was rerun.",
                              fidelity=dict(candidate=results[candidate]["verdict"], control=results[control]["verdict"]),
                              fidelity_reasons=dict(candidate=results[candidate]["reasons"], control=results[control]["reasons"]), perceptual="NOT_ASSESSED"),
                scope=dict(gpu="Replay harness GPU bracket; not game FPS, display pacing, latency, or isolated kernel busy time.",
                           statistics="Descriptive frame quantiles with linear interpolation; groups below 20 available observations are marked small_sample. No population confidence interval or independent-frame assumption.",
                           validation="Metadata only. No raw image was opened, regenerated, or rewritten; image correctness remains the input judge's responsibility.",
                           totals="Scored frames counted once; event groups overlap. Window contexts may include unscored warmup."),
                frames=frames, summaries=summaries, events=[dict(id=r["id"], ordinal=r["ordinal"], score=r["score"], labels=r["events"]) for r in timeline if r["events"]],
                worst_frames=worst, windows=windows,
                _input_roots=[str(capture_path.parent), str(runs_dir), str(judge_dir)])
    _finite_tree(report)
    return report


def _display(value):
    return "unavailable" if value is None else f"{value:.9g}" if isinstance(value, (float, int)) else str(value).replace("|", "\\|").replace("\n", " ")


def _markdown(report):
    identities, validity = report["identities"], report["validity"]
    lines = ["# Temporal replay events", "",
             f"Candidate **{_display(identities['candidate']['name'])}**, control **{_display(identities['control']['name'])}**, reference **{_display(identities['reference']['name'])}**.", "",
             f"Profile: **{_display(report['corpus']['profile'])}**. Functional metadata validation: **{validity['functional']}**. Existing numerical fidelity: candidate **{validity['fidelity']['candidate']}**, control **{validity['fidelity']['control']}**. Perceptual assessment: **{validity['perceptual']}**.", "",
             report["provenance"]["scope"], "", report["scope"]["gpu"], "", report["scope"]["statistics"], "", report["scope"]["validation"], "", report["scope"]["totals"], "",
             "## Phase quality and cost", "",
             "Pulse distance is `abs(candidate pulse - control pulse)`. Amplitude excess is signed `abs(candidate pulse) - abs(control pulse)`. A reset or missing pulse remains unavailable.", "",
             "| Phase | Region | Scored frames | Pulse pairs / missing | Distance p95 / p99 | Golden MAE mean | Candidate GPU mean / p95 / p99, ms | Control GPU mean, ms |", "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |"]
    for phase, group in report["summaries"]["by_phase"].items():
        for region, metrics in group["regions"].items():
            pair, gpu = metrics["paired"]["paired_abs_delta"], group["gpu_ms"]["candidate"]
            lines.append(f"| {_display(phase)} | {_display(region)} | {group['frame_count']} | {pair['count']} / {pair['missing']} | {_display(pair['p95'])} / {_display(pair['p99'])} | {_display(metrics['candidate']['golden_mae']['mean'])} | {_display(gpu['mean'])} / {_display(gpu['p95'])} / {_display(gpu['p99'])} | {_display(group['gpu_ms']['control']['mean'])} |")
    lines += ["", "## Worst paired deviations", "", "| Region | Frame | Phase | Kind / transition | Age / preceding carried streak | Signed delta | Distance | Amplitude excess | GPU candidate / control, ms |", "| --- | ---: | --- | --- | ---: | ---: | ---: | ---: | ---: |"]
    for region, rows in report["worst_frames"].items():
        for row in rows:
            lines.append(f"| {_display(region)} | {row['id']} | {_display(row['phase'])} | {row['kind']} / {_display(row['transition'])} | {_display(row['history_age'])} / {row['preceding_carried_streak']} | {_display(row['pulse_delta'])} | {_display(row['paired_abs_delta'])} | {_display(row['amplitude_excess'])} | {_display(row['gpu_ms'])} / {_display(row['control_gpu_ms'])} |")
    lines += ["", "## Selected context windows", ""]
    for context in report["windows"]:
        lines.append(f"- Frame {context['anchor_frame_id']} ({', '.join(context['regions'])}): IDs {', '.join(map(str, context['frame_ids']))}; ordinals {context['first_ordinal']}–{context['last_ordinal']}.")
    lines += ["", "## Bound source identities", "", "| Role | Name | Recorded source | Manifest SHA-256 |", "| --- | --- | --- | --- |"]
    for role, identity in identities.items():
        lines.append(f"| {role} | {_display(identity['name'])} | {_display(identity['producer']['commit'])} | `{identity['sha256']}` |")
    lines += ["", f"Capture SHA-256: `{report['capture_sha256']}`. Every consumed input's byte count and hash is in events.json. The recorded producer identities above are preserved; no current source checkout is substituted.", ""]
    return "\n".join(lines)


def write_report(report, output_dir):
    """Write three deterministic files in a directory isolated from all inputs."""
    out = Path(output_dir).resolve()
    for root in report["_input_roots"]:
        root = Path(root)
        need(not (out == root or out.is_relative_to(root) or root.is_relative_to(out)), "output must be separate from input directories")
    sources = {Path(item["path"]) for item in report["inputs"]}
    outputs = [out / name for name in ("events.json", "frames.jsonl", "events.md")]
    for path in outputs:
        need(path.resolve() not in sources and not path.is_symlink(), "output would overwrite input/symlink")
        need(not path.exists() or path.stat().st_nlink == 1, "output has linked input bytes")
    public = {key: value for key, value in report.items() if not key.startswith("_") and key != "frames"}
    contents = [json.dumps(public, indent=2, sort_keys=True, allow_nan=False) + "\n",
                "".join(json.dumps(row, sort_keys=True, allow_nan=False) + "\n" for row in report["frames"]), _markdown(report)]
    out.mkdir(parents=True, exist_ok=True)
    for path, content in zip(outputs, contents):
        path.write_text(content, encoding="utf-8")


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--capture", required=True, type=Path)
    parser.add_argument("--runs", required=True, type=Path)
    parser.add_argument("--judge", required=True, type=Path)
    parser.add_argument("--candidate", required=True)
    parser.add_argument("--control", default="every-b")
    parser.add_argument("--reference", default="every-a")
    parser.add_argument("--qualification", type=Path)
    parser.add_argument("--window", type=int, default=4)
    parser.add_argument("--top", type=int, default=8)
    parser.add_argument("--out", required=True, type=Path)
    args = parser.parse_args(argv)
    try:
        report = analyze(args.capture, args.runs, args.judge, args.candidate, control=args.control, reference=args.reference, qualification=args.qualification, window=args.window, top=args.top)
        write_report(report, args.out)
    except (ValueError, OSError, TypeError, KeyError, OverflowError) as error:
        print("events: " + str(error), file=sys.stderr)
        return 2
    print(f"Temporal metadata PASS; existing candidate fidelity {report['validity']['fidelity']['candidate']}; perceptual NOT_ASSESSED")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

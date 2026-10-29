"""Metadata-only, independently specified controls for temporal event analysis."""

import copy
import hashlib
import importlib
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT))

ERRORS = (
    "golden_mae", "golden_max", "golden_bad_fraction", "ghost_mae",
    "disocclusion_mae", "detail_error", "pulse_p95", "temporal_mae",
)


def save(path, value):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n")


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def quantile(values, fraction):
    if not values:
        return None
    ordered = sorted(values)
    position = (len(ordered) - 1) * fraction
    lower = int(position)
    upper = min(lower + 1, len(ordered) - 1)
    return ordered[lower] * (1 - (position - lower)) + ordered[upper] * (position - lower)


def aggregate(rows):
    """Build fixture receipts, without importing the analyzer or judge helpers."""
    regions = {}
    pulses = {}
    for region in ("all", "centre"):
        values = [row["regions"][region] for row in rows]
        metrics = {}
        for key in ERRORS:
            available = [v[key] for v in values if key in v and v[key] is not None]
            if key == "pulse_p95":
                metrics[key] = quantile([abs(v["pulse"]) for v in values if v["pulse"] is not None], .95)
            elif key == "golden_max":
                metrics[key] = max(available, default=None)
            else:
                metrics[key] = sum(available) / len(available) if available else None
        for key in ("coverage", "candidate_gradient", "reference_gradient"):
            available = [v[key] for v in values if v[key] is not None]
            metrics[key] = sum(available) / len(available) if available else (0.0 if key == "coverage" else None)
        metrics["temporal_pairs"] = sum(v["temporal_mae"] is not None for v in values)
        metrics["pulse_pairs"] = sum(v["pulse"] is not None for v in values)
        regions[region] = metrics
        groups = {}
        for row in rows:
            value = row["regions"][region]["pulse"]
            if value is not None:
                for key in (row["kind"], row["transition"]):
                    groups.setdefault(key, []).append(abs(value))
        pulses[region] = {
            key: dict(count=len(v), p50=quantile(v, .5), p95=quantile(v, .95), maximum=max(v))
            for key, v in groups.items()
        }
    timing = {}
    for kind in ("all", "every", "model", "carried"):
        values = [r["gpu_ms"] for r in rows if kind == "all" or r["kind"] == kind]
        timing[kind] = dict(count=len(values), p50=quantile(values, .5), p95=quantile(values, .95), maximum=max(values, default=None))
    return regions, pulses, timing


class Bundle:
    """Complete current manifests; deliberately no image files exist."""

    def __init__(self, root):
        self.root = root
        self.capture_path = root / "corpus/capture.json"
        self.runs_dir = root / "runs"
        self.judge_dir = root / "judge"
        desc = dict(path="pixels.raw", encoding="raw", dtype="f32", width=8, height=6, channels=3, row_pitch=96, sha256="0" * 64)
        self.capture = dict(
            schema="retoque-replay-capture", version=1, complete=True,
            mode="nr-boundary", source="analytic-temporal-test", clock="recorded-evaluation",
            color_space="linear-rec709", color_jitter="removed",
            depth=dict(kind="device", inverted=False, threshold=.02),
            motion=dict(direction="current-to-previous", units="pixels", scale=[1, 1], includes_jitter=False),
            regions={"centre": [2, 1, 4, 4]},
            provenance=dict(producer="test", shader_sha256="0" * 64, profile="analytic-v1"), frames=[],
        )
        for i in range(9):
            self.capture["frames"].append(dict(
                id=41 + i, present=(i + 1) * 7, time_ms=i * 16.0,
                reset=i in (0, 6), score=i >= 2, jitter=[0, 0],
                pre_exposure=1.0, exposure=None, exposure_value=1.0,
                completion="fence-complete", params=dict(frame_time_ms=16.0),
                color=copy.deepcopy(desc),
                depth=dict(desc, channels=1, row_pitch=32),
                motion=dict(desc, channels=2, row_pitch=64),
            ))
        phases = [("warmup", 0, 2), ("steady", 2, 5), ("fast-motion", 5, 6), ("cut", 6, 7), ("recovery", 7, 9)]
        self.capture["provenance"]["corpus"] = dict(
            schema="retoque-replay-corpus-summary", version=1, profile="analytic-v1",
            source="analytic-temporal-test", frames=9, scored_frames=7,
            nominal_duration_ms=144.0, timestamp_span_ms=128.0, scored_duration_ms=112.0,
            reset_frame_ids=[41, 47], phases=[
                dict(name=name, first_frame_id=41 + start, last_frame_id=40 + stop,
                     frames=stop-start, scored_frames=sum(i >= 2 for i in range(start, stop)),
                     nominal_duration_ms=(stop-start)*16.0)
                for name, start, stop in phases
            ],
        )
        self.runs = {}
        for name in ("every-a", "every-b", "candidate"):
            kinds = ["every"] * 9 if name != "candidate" else ["every", "carried", "carried", "model", "carried", "every", "every", "carried", "model"]
            self.runs[name] = dict(
                schema="retoque-replay-run", version=1, complete=True,
                environment=dict(model_sha256="1" * 64, driver="test", gpu="test", runtime="test"),
                producer=dict(dll_sha256="2" * 64, commit="qualified-source", trial=name),
                settings=dict(Cadence="2" if name == "candidate" else "1", Enabled=True),
                requirements=dict(min_model=1, min_carried=0), frames=[
                    dict(id=41 + i, kind=kind, interval=1 if kind == "every" else 2,
                         requested_passes=1, completed_passes=0 if kind == "carried" else 1,
                         completion="fence-complete", model_result=1, recorded=True,
                         gpu_ms=.1 if kind == "carried" else 2.0, output=copy.deepcopy(desc))
                    for i, kind in enumerate(kinds)
                ],
            )
        self.rows = {}
        for name, pulses in (("every-b", [2, 2, -3, 1, None, 3, 1]), ("candidate", [2, -2, -1, 3, None, None, 5])):
            rows = []
            for i, pulse in enumerate(pulses, 2):
                frame = self.capture["frames"][i]
                raw = self.runs[name]["frames"][i]
                pair = not frame["reset"]
                metric = dict(
                    golden_mae=.1 if name == "candidate" else 0.0,
                    golden_max=.2 if name == "candidate" else 0.0,
                    golden_bad_fraction=0.0, ghost_mae=0.0,
                    disocclusion_mae=0.0 if pair else None, detail_error=0.0,
                    candidate_gradient=.01, reference_gradient=.01, pulse=pulse,
                    temporal_mae=.02 if pair else None, coverage=1.0 if pair else None,
                    temporal_pair=pair,
                )
                rows.append(dict(id=frame["id"], present=frame["present"], time_ms=frame["time_ms"],
                                 kind=raw["kind"], interval=raw["interval"],
                                 transition=self.runs[name]["frames"][i-1]["kind"] + "->" + raw["kind"],
                                 gpu_ms=raw["gpu_ms"], reference_gpu_ms=2.0,
                                 regions={r: copy.deepcopy(metric) for r in ("all", "centre")}))
            self.rows[name] = rows
        self.results = {}
        self.flush()

    def run_path(self, name):
        return self.runs_dir / name / "run.json"

    def result_path(self, name):
        return self.judge_dir / name / "result.json"

    def rows_path(self, name):
        return self.judge_dir / name / "frames.jsonl"

    def flush(self):
        save(self.capture_path, self.capture)
        for name, raw in self.runs.items():
            raw["capture_sha256"] = sha(self.capture_path)
            save(self.run_path(name), raw)
        for name, rows in self.rows.items():
            metrics, pulse_groups, timing = aggregate(rows)
            raw = self.runs[name]
            self.results[name] = dict(
                schema="retoque-replay-result", version=1, backend="python", metrics_revision=2,
                capture_sha256=sha(self.capture_path), reference_sha256=sha(self.run_path("every-a")),
                candidate_sha256=sha(self.run_path(name)), environment=raw["environment"],
                reference_producer=self.runs["every-a"]["producer"], candidate_producer=raw["producer"],
                reference_settings=self.runs["every-a"]["settings"], candidate_settings=raw["settings"],
                policy=dict(version=1, min_coverage=.25, pixel_tolerance=.01, floors={k: 0 for k in ERRORS}),
                metrics=metrics, pulse_groups=pulse_groups, timing=timing,
                counts=dict(scored=len(rows), carried=sum(r["kind"] == "carried" for r in rows), model=sum(r["kind"] != "carried" for r in rows)),
                verdict="PASS" if name == "every-b" else "FAIL",
                reasons=[] if name == "every-b" else ["all.golden_mae: 0.1 > 0"],
                bounds={r: {k: (3.0 if k == "pulse_p95" else .02 if k == "temporal_mae" else 0.0) for k in ERRORS} for r in ("all", "centre")},
            )
            save(self.result_path(name), self.results[name])
            self.rows_path(name).write_text("".join(json.dumps(row, allow_nan=False) + "\n" for row in rows))

    def qualification(self):
        retained = []
        for name in ("candidate", "every-b"):
            for filename in ("result.json", "frames.jsonl"):
                path = self.judge_dir / name / filename
                retained.append(dict(source="/unavailable/original/judge/" + name + "/" + filename,
                                     path=str(path), relative_path="judge/" + name + "/" + filename,
                                     sha256=sha(path), bytes=path.stat().st_size))
        path = self.root / "qualification.json"
        save(path, dict(schema="retoque-rendering-run", version=1, status="PASS", run_id="retained-run",
                        steps=[dict(name="nr-replay", status="PASS", functional_status="PASS",
                                    artifact_retention=dict(status="PASS"), retained_artifacts=retained)]))
        return path


class EventsTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.bundle = Bundle(self.root)
        try:
            self.events = importlib.import_module("tools.replay.events")
        except ModuleNotFoundError:
            self.fail("offline temporal events analysis is not implemented")

    def analyze(self, **kwargs):
        b = self.bundle
        return self.events.analyze(b.capture_path, b.runs_dir, b.judge_dir, "candidate", **kwargs)

    def test_equal_nonzero_and_opposite_pulses_are_paired_before_absolute_value(self):
        rows = self.analyze()["frames"]
        expected = [(0, 0, 0), (-4, 4, 0), (2, 2, -2), (2, 2, 2)]
        for row, values in zip(rows, expected):
            paired = row["regions"]["all"]["paired"]
            self.assertEqual(tuple(paired[k] for k in ("pulse_delta", "paired_abs_delta", "amplitude_excess")), values)
        self.assertEqual(rows[0]["regions"]["all"]["control"]["pulse"], 2)

    def test_reset_transition_and_missing_pulse_remain_unavailable(self):
        report = self.analyze()
        reset, missing = report["frames"][4:6]
        self.assertEqual(reset["transition"], "every->every")
        for row, reason in ((reset, "reset"), (missing, "candidate-pulse-unavailable")):
            paired = row["regions"]["all"]["paired"]
            self.assertIsNone(paired["paired_abs_delta"])
            self.assertEqual(paired["unavailable_reason"], reason)
        cut = report["summaries"]["by_phase"]["cut"]["regions"]["all"]["paired"]["paired_abs_delta"]
        self.assertEqual((cut["count"], cut["missing"]), (0, 1))
        self.assertIsNone(cut["p95"])

    def test_history_uses_warmup_and_preserves_preceding_carried_streak(self):
        rows = self.analyze()["frames"]
        self.assertEqual([r["history_age"] for r in rows], [2, 0, 1, 0, 0, 1, 0])
        self.assertEqual(rows[1]["preceding_carried_streak"], 2)
        self.assertEqual(rows[3]["preceding_carried_streak"], 1)
        self.assertEqual(rows[4]["preceding_carried_streak"], 0)
        self.assertEqual([r["history_epoch"] for r in rows], [1, 1, 1, 1, 2, 2, 2])

    def test_skipped_warmup_invalidates_known_history(self):
        b = self.bundle
        b.runs["candidate"]["frames"][0].update(kind="skipped", interval=0, requested_passes=0, completed_passes=0, recorded=False, skip="settling")
        b.flush()
        report = self.analyze()
        self.assertIsNone(report["frames"][0]["history_age"])
        self.assertEqual(report["frames"][1]["history_age"], 0)
        self.assertEqual(report["frames"][1]["preceding_carried_streak"], 2)

    def test_quantiles_counts_extrema_and_phase_costs_are_descriptive(self):
        report = self.analyze(window=1, top=2)
        stats = report["summaries"]["all"]["regions"]["all"]["paired"]["paired_abs_delta"]
        self.assertEqual((stats["count"], stats["missing"]), (5, 2))
        self.assertEqual((stats["mean"], stats["p50"], stats["p95"], stats["p99"]), (2.4, 2, 4, 4))
        self.assertEqual((stats["minimum_frame_id"], stats["maximum_frame_id"]), (43, 44))
        self.assertTrue(stats["descriptive_only"])
        self.assertTrue(stats["small_sample"])
        phase = report["summaries"]["by_phase"]["fast-motion"]
        self.assertEqual(phase["frame_count"], 1)
        self.assertEqual(phase["gpu_ms"]["candidate"]["mean"], 2)
        self.assertEqual(report["summaries"]["by_phase"]["warmup"]["frame_count"], 0)
        steady = report["summaries"]["by_phase"]["steady"]["regions"]["all"]["paired"]["paired_abs_delta"]
        self.assertAlmostEqual(steady["p95"], 3.8)
        self.assertAlmostEqual(steady["p99"], 3.96)

    def test_event_overlap_never_duplicates_totals_and_windows_are_bounded(self):
        report = self.analyze(window=1, top=2)
        self.assertEqual(report["summaries"]["all"]["frame_count"], 7)
        self.assertEqual([r["id"] for r in report["worst_frames"]["all"]], [44, 49])
        self.assertEqual([w["frame_ids"] for w in report["windows"]], [[43, 44, 45], [48, 49]])
        self.assertIn("reset", report["frames"][4]["events"])
        self.assertIn("phase-change", report["frames"][4]["events"])

    def test_unretained_lineage_cannot_claim_historical_qualification(self):
        report = self.analyze()
        self.assertFalse(report["provenance"]["historically_retained"])
        self.assertEqual(report["provenance"]["validation"], "current-manifests-and-input-hashes")
        self.assertEqual(report["identities"]["candidate"]["producer"]["commit"], "qualified-source")
        self.assertEqual(len(report["inputs"]), 8)
        self.assertEqual(report["validity"]["fidelity"]["candidate"], "FAIL")
        self.assertEqual(report["validity"]["perceptual"], "NOT_ASSESSED")

    def test_retained_receipt_checks_actual_bytes_without_reading_stale_source(self):
        q = self.bundle.qualification()
        report = self.analyze(qualification=q)
        self.assertTrue(report["provenance"]["historically_retained"])
        self.assertEqual(report["provenance"]["retained_artifact_count"], 4)
        target = self.bundle.rows_path("candidate")
        target.write_text(target.read_text().replace('"id": 43', '"id":43', 1))
        with self.assertRaisesRegex(ValueError, "retained|receipt"):
            self.analyze(qualification=q)

    def test_retained_receipt_requires_consumed_absolute_retained_path(self):
        q = self.bundle.qualification()
        receipt = json.loads(q.read_text())
        consumed = self.bundle.result_path("candidate")
        copied = self.root / "copied-result.json"
        source = self.root / "original-source-result.json"
        copied.write_bytes(consumed.read_bytes())
        source.write_bytes(consumed.read_bytes())
        for label, path in (
            ("nonexistent", self.root / "missing-result.json"),
            ("different-copy", copied),
            ("source", source),
            ("relative", Path("judge/candidate/result.json")),
        ):
            modified = copy.deepcopy(receipt)
            artifact = modified["steps"][0]["retained_artifacts"][0]
            artifact["path"] = str(path)
            artifact["source"] = str(source)
            save(q, modified)
            with self.subTest(path=label), self.assertRaisesRegex(ValueError, "retained.*path"):
                self.analyze(qualification=q)
        save(q, receipt)
        self.assertTrue(self.analyze(qualification=q)["provenance"]["historically_retained"])

    def test_rejects_incomplete_or_duplicate_frame_rows(self):
        b = self.bundle
        original = b.rows_path("candidate").read_text()
        lines = original.splitlines()
        for changed in (lines[:-1], [lines[0], *lines[:-1]], list(reversed(lines))):
            with self.subTest(rows=len(changed)):
                b.rows_path("candidate").write_text("\n".join(changed) + "\n")
                with self.assertRaisesRegex(ValueError, "frame|identity|order"):
                    self.analyze()

    def test_rejects_scored_time_present_decision_and_gpu_mismatches(self):
        b = self.bundle
        original = copy.deepcopy(b.rows["candidate"][0])
        for key, value in (("id", True), ("time_ms", 99), ("present", 1), ("kind", "every"), ("interval", 4), ("gpu_ms", 9), ("reference_gpu_ms", 9), ("transition", "every->every")):
            with self.subTest(key=key):
                b.rows["candidate"][0] = dict(original, **{key: value})
                b.flush()
                with self.assertRaisesRegex(ValueError, "frame|identity|mismatch|transition"):
                    self.analyze()

    def test_rejects_provenance_policy_environment_and_metrics_revision_mismatch(self):
        b = self.bundle
        for key, value in (("candidate_sha256", "f" * 64), ("capture_sha256", "f" * 64), ("reference_sha256", "f" * 64), ("metrics_revision", True), ("environment", {}), ("candidate_producer", {}), ("policy", {})):
            with self.subTest(key=key):
                b.flush()
                value_dict = dict(b.results["candidate"], **{key: value})
                save(b.result_path("candidate"), value_dict)
                with self.assertRaises(ValueError):
                    self.analyze()

    def test_rejects_edited_metric_and_timing_aggregates(self):
        b = self.bundle
        for group, region, key in (("metrics", "all", "pulse_p95"), ("timing", "all", "count"), ("counts", None, "model")):
            b.flush()
            result = copy.deepcopy(b.results["candidate"])
            target = result[group] if region is None else result[group][region]
            target[key] += 1
            save(b.result_path("candidate"), result)
            with self.subTest(group=group), self.assertRaisesRegex(ValueError, "aggregate|count|timing|metrics"):
                self.analyze()

    def test_rejects_contradictory_reset_metrics_and_boolean_numbers(self):
        b = self.bundle
        baseline = copy.deepcopy(b.rows["candidate"])
        for index, key, value in ((4, "pulse", 0), (4, "temporal_pair", True), (0, "pulse", False), (0, "coverage", 2), (0, "golden_mae", -1)):
            b.rows["candidate"] = copy.deepcopy(baseline)
            b.rows["candidate"][index]["regions"]["all"][key] = value
            b.flush()
            with self.subTest(key=key, value=value), self.assertRaises(ValueError):
                self.analyze()

    def test_rejects_gaps_partial_passes_scored_skips_and_nonfresh_control(self):
        b = self.bundle
        original = copy.deepcopy(b.runs)
        changes = [
            lambda: b.runs["candidate"]["frames"][3].update(id=900),
            lambda: b.runs["candidate"]["frames"][3].update(completed_passes=0),
            lambda: b.runs["candidate"]["frames"][3].update(model_result=0),
            lambda: b.runs["candidate"]["frames"][3].update(kind="skipped", skip="failure", completed_passes=0, interval=0, requested_passes=0),
            lambda: b.runs["every-b"]["producer"].update(trial="every-a"),
            lambda: b.runs["every-b"]["frames"][1].update(kind="carried", interval=2, completed_passes=0),
        ]
        for mutate in changes:
            b.runs = copy.deepcopy(original)
            mutate()
            b.flush()
            with self.assertRaises(ValueError):
                self.analyze()

    def test_phase_coverage_names_counts_profile_and_reset_ids_are_bound(self):
        b = self.bundle
        original = copy.deepcopy(b.capture)
        for key, value in (("frames", True), ("scored_frames", 6), ("profile", "relabeled"), ("reset_frame_ids", [41]), ("phases", [])):
            b.capture = copy.deepcopy(original)
            b.capture["provenance"]["corpus"][key] = value
            b.flush()
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, "corpus|phase|profile|reset"):
                self.analyze()
        for key, value in (("first_frame_id", 42), ("last_frame_id", 49), ("frames", 5), ("scored_frames", 0), ("name", "warmup")):
            b.capture = copy.deepcopy(original)
            b.capture["provenance"]["corpus"]["phases"][1][key] = value
            b.flush()
            with self.subTest(key=key), self.assertRaisesRegex(ValueError, "corpus|phase"):
                self.analyze()

    def test_strict_json_limits_and_option_types_fail_before_output(self):
        b = self.bundle
        path = b.result_path("candidate")
        original = path.read_text()
        for text in ('{"duplicate": 1, "duplicate": 2}', '{"number": NaN}', '{"number": 1e999}'):
            path.write_text(text)
            with self.assertRaisesRegex(ValueError, "duplicate|finite"):
                self.analyze()
        path.write_text(original)
        with path.open("ab") as stream:
            stream.truncate(32 * 1024 * 1024 + 1)
        with self.assertRaisesRegex(ValueError, "limit|large"):
            self.analyze()
        path.write_text(original)
        for kwargs in (dict(window=True), dict(window=-1), dict(window=33), dict(top=False), dict(top=0), dict(top=17)):
            with self.subTest(kwargs=kwargs), self.assertRaises(ValueError):
                self.analyze(**kwargs)

    def test_frame_count_limit_and_capture_gap_are_rejected(self):
        b = self.bundle
        original = copy.deepcopy(b.capture)
        b.capture["frames"] = [original["frames"][0]] * 10_001
        b.flush()
        with self.assertRaisesRegex(ValueError, "frame count"):
            self.analyze()
        b.capture = copy.deepcopy(original)
        b.capture["frames"][1]["id"] += 1
        b.flush()
        with self.assertRaisesRegex(ValueError, "nonconsecutive"):
            self.analyze()

    def test_missing_control_pulse_is_not_zero_and_zero_window_has_one_frame(self):
        b = self.bundle
        for region in ("all", "centre"):
            b.rows["every-b"][0]["regions"][region]["pulse"] = None
        b.flush()
        report = self.analyze(window=0, top=1)
        self.assertIsNone(report["frames"][0]["regions"]["all"]["paired"]["pulse_delta"])
        self.assertEqual(report["frames"][0]["regions"]["all"]["paired"]["unavailable_reason"], "control-pulse-unavailable")
        self.assertEqual(report["windows"][0]["frame_ids"], [44])

    def test_real_judge_outputs_roundtrip_without_image_reads_in_analyzer(self):
        sys.path.insert(0, str(ROOT / "tests/_support"))
        from replay_fixture import corpus, run
        from tools.replay.judge import calibrate, judge, write_report

        base = self.root / "real"
        capture, pixels = corpus(base / "corpus", moving=True, n=9)
        data = json.loads(capture.read_text())
        for i, frame in enumerate(data["frames"]):
            frame.update(score=i >= 2, reset=i in (0, 6))
        metadata = copy.deepcopy(self.bundle.capture["provenance"]["corpus"])
        metadata.update(source=data["source"], reset_frame_ids=[1, 7])
        for phase in metadata["phases"]:
            phase["first_frame_id"] -= 40
            phase["last_frame_id"] -= 40
        data["provenance"]["corpus"] = metadata
        save(capture, data)
        paths = {name: run(base / "runs" / name, capture, pixels, suffix="run", salt=i + 1)
                 for i, name in enumerate(("every-a", "every-b", "candidate"))}
        repeats = [run(base / ("repeat" + str(i)), capture, pixels, salt=i + 10) for i in range(5)]
        calibration = calibrate(capture, paths["every-a"], repeats)
        for name in ("every-b", "candidate"):
            result = judge(capture, paths["every-a"], paths[name], calibration)
            self.assertEqual(result["verdict"], "PASS", result.get("reasons"))
            write_report(result, base / "judge" / name)
        for path in base.rglob("*.raw"):
            path.unlink()
        for path in (base / "corpus").iterdir():
            if path != capture:
                path.unlink()
        report = self.events.analyze(capture, base / "runs", base / "judge", "candidate")
        self.assertEqual(report["summaries"]["all"]["frame_count"], 7)
        self.assertEqual(report["summaries"]["all"]["regions"]["all"]["paired"]["paired_abs_delta"]["maximum"], 0)

    def test_large_finite_metrics_cannot_generate_nonfinite_report(self):
        b = self.bundle
        for name in ("candidate", "every-b"):
            for row in b.rows[name]:
                for region in ("all", "centre"):
                    if row["regions"][region]["pulse"] is not None:
                        row["regions"][region]["pulse"] = 1e308
        b.flush()
        for name in ("candidate", "every-b"):
            for region in ("all", "centre"):
                b.results[name]["bounds"][region]["pulse_p95"] = 1e308
            save(b.result_path(name), b.results[name])
        with self.assertRaisesRegex(ValueError, "nonfinite"):
            self.analyze()

    def test_write_report_and_cli_are_deterministic_and_do_not_overwrite_inputs(self):
        report = self.analyze(window=1, top=2)
        out = self.root / "analysis"
        self.events.write_report(report, out)
        first = {p.name: p.read_bytes() for p in out.iterdir()}
        self.events.write_report(self.analyze(window=1, top=2), out)
        self.assertEqual(first, {p.name: p.read_bytes() for p in out.iterdir()})
        self.assertEqual(set(first), {"events.json", "frames.jsonl", "events.md"})
        self.assertEqual(len(first["frames.jsonl"].splitlines()), 7)
        markdown = first["events.md"].decode()
        for text in ("fast-motion", "44", "49", "NOT_ASSESSED", "harness", "p95"):
            self.assertIn(text, markdown)
        b = self.bundle
        original = b.rows_path("candidate").read_bytes()
        for output in (b.judge_dir / "candidate", b.judge_dir / "new-output", b.runs_dir / "new-output", b.capture_path.parent):
            with self.assertRaisesRegex(ValueError, "output|input"):
                self.events.write_report(report, output)
        self.assertEqual(b.rows_path("candidate").read_bytes(), original)
        cli_out = self.root / "cli-output"
        args = [sys.executable, "-m", "tools.replay.events", "--capture", str(b.capture_path), "--runs", str(b.runs_dir), "--judge", str(b.judge_dir), "--candidate", "candidate", "--window", "1", "--top", "2", "--out", str(cli_out)]
        result = subprocess.run(args, cwd=ROOT, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((cli_out / "events.json").read_bytes(), first["events.json"])
        bad = subprocess.run(args[:-1] + [str(b.judge_dir / "candidate")], cwd=ROOT, capture_output=True, text=True)
        self.assertNotEqual(bad.returncode, 0)
        self.assertEqual(b.rows_path("candidate").read_bytes(), original)


if __name__ == "__main__":
    unittest.main()

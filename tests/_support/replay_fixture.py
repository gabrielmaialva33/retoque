"""Small analytic corpus, independent of the judge's readers/metrics."""

import hashlib
import json
import struct


def save_json(path, value):
    path.write_text(json.dumps(value, indent=2, allow_nan=False) + "\n")
    return path


def plane(root, name, values, w=8, h=6, channels=3, dtype="f32"):
    code = {"f32": "f", "f16": "e", "u8": "B"}[dtype]
    data = struct.pack("<" + str(len(values)) + code, *values)
    (root / name).write_bytes(data)
    return dict(
        path=name,
        encoding="raw",
        dtype=dtype,
        width=w,
        height=h,
        channels=channels,
        row_pitch=w * channels * struct.calcsize(code),
        sha256=hashlib.sha256(data).hexdigest(),
    )


def corpus(root, moving=False, n=8):
    root.mkdir(parents=True, exist_ok=True)
    capture = dict(
        schema="retoque-replay-capture",
        version=1,
        complete=True,
        mode="nr-boundary",
        source="synthetic-test",
        clock="recorded-evaluation",
        color_space="linear-rec709",
        color_jitter="removed",
        depth=dict(kind="device", inverted=False, threshold=0.02),
        motion=dict(
            direction="current-to-previous",
            units="pixels",
            scale=[1, 1],
            includes_jitter=False,
        ),
        regions={"left": [0, 0, 4, 6]},
        provenance={"producer": "test", "shader_sha256": "0" * 64},
        frames=[],
    )
    pixels = []
    for i in range(n):
        rgb = [
            v
            for y in range(6)
            for x in range(8)
            for v in ([0.2 + 0.05 * ((x - i if moving else x) % 8)] * 3)
        ]
        pixels.append(rgb)
        mv = [v for _ in range(48) for v in ([-1, 0] if moving else [0, 0])]
        capture["frames"].append(
            dict(
                id=i + 1,
                present=(i + 1) * 4,
                time_ms=i * 16.0,
                reset=i == 0,
                score=i > 0,
                jitter=[0, 0],
                pre_exposure=1.0,
                exposure=None,
                exposure_value=1.0,
                completion="fence-complete",
                params={},
                color=plane(root, f"{i}.color", rgb),
                depth=plane(root, f"{i}.depth", [0.5] * 48, channels=1),
                motion=plane(root, f"{i}.mv", mv, channels=2),
            )
        )
    return save_json(root / "capture.json", capture), pixels


def run(root, capture_path, pixels, suffix="ref", transform=None, kinds=None, salt=0):
    root.mkdir(parents=True, exist_ok=True)
    frames = []
    for i, rgb in enumerate(pixels):
        output = [v * 1.1 for v in rgb]
        if transform:
            output = transform(i, output)
        kind = kinds[i] if kinds else "every"
        frames.append(
            dict(
                id=i + 1,
                kind=kind,
                interval=1 if kind == "every" else 2,
                requested_passes=1,
                completed_passes=0 if kind == "carried" else 1,
                completion="fence-complete",
                gpu_ms=0.1 if kind == "carried" else 2.0,
                output=plane(root, f"{suffix}{i}.raw", output),
            )
        )
    result = dict(
        schema="retoque-replay-run",
        version=1,
        complete=True,
        capture_sha256=hashlib.sha256(capture_path.read_bytes()).hexdigest(),
        environment=dict(
            model_sha256="1" * 64, driver="test", gpu="test", runtime="test"
        ),
        producer=dict(dll_sha256="2" * 64, commit="test", trial=f"trial-{salt}"),
        settings=dict(Cadence=1),
        requirements=dict(min_model=1, min_carried=0),
        frames=frames,
    )
    return save_json(root / (suffix + ".json"), result)


def change(path, update):
    data = json.loads(path.read_text())
    update(data)
    save_json(path, data)

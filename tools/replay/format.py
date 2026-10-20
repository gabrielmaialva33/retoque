"""Strict v1 wire reader. See docs/dlssnr/design/offline-replay.md."""

from array import array
from dataclasses import dataclass
import hashlib
import json
import math
from pathlib import Path, PurePosixPath
import re
import struct

MAX_PIXELS = 16_777_216
MAX_BYTES = 536_870_912


@dataclass
class Image:
    width: int
    height: int
    channels: int
    data: object


def need(condition, reason):
    if not condition:
        raise ValueError(reason)


def number(value, name, minimum=None):
    need(
        type(value) in (int, float) and math.isfinite(value),
        f"{name}: finite number required",
    )
    need(minimum is None or value >= minimum, f"{name}: below minimum")
    return value


def integer(value, name, minimum=0):
    need(
        type(value) is int and value >= minimum,
        f"{name}: integer >= {minimum} required",
    )
    return value


def boolean(value, name):
    need(type(value) is bool, f"{name}: boolean required")
    return value


def sha(value):
    need(
        isinstance(value, str) and re.fullmatch("[0-9a-f]{64}", value),
        "invalid SHA-256",
    )
    return value


def digest(path):
    with Path(path).open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def json_file(path):
    path = Path(path)
    need(path.stat().st_size <= 64 * 1024 * 1024, "manifest too large")

    def pairs(items):
        result = {}
        for key, value in items:
            need(key not in result, f"duplicate JSON key: {key}")
            result[key] = value
        return result

    def finite_tree(value):
        if isinstance(value, float):
            need(math.isfinite(value), "nonfinite JSON number")
        elif isinstance(value, dict):
            for item in value.values():
                finite_tree(item)
        elif isinstance(value, list):
            for item in value:
                finite_tree(item)

    result = json.loads(path.read_text(encoding="utf-8"), object_pairs_hook=pairs)
    need(isinstance(result, dict), "JSON object required")
    finite_tree(result)
    return result


def child(root, value):
    need(
        isinstance(value, str) and value and "\\" not in value and ":" not in value,
        "invalid plane path",
    )
    path = PurePosixPath(value)
    need(
        not path.is_absolute() and ".." not in path.parts and value not in (".", ""),
        "unsafe plane path",
    )
    root = Path(root).resolve()
    result = (root / value).resolve()
    need(result.is_relative_to(root) and result != root, "plane path escapes root")
    return result


def descriptor(root, desc):
    need(isinstance(desc, dict), "plane descriptor required")
    w = integer(desc.get("width"), "width", 1)
    h = integer(desc.get("height"), "height", 1)
    c = integer(desc.get("channels"), "channels", 1)
    need(w * h <= MAX_PIXELS and c <= 4, "plane extent/channels exceeds limit")
    need(desc.get("dtype") in ("f16", "f32", "u8"), "unsupported plane dtype")
    size = {"f16": 2, "f32": 4, "u8": 1}[desc["dtype"]]
    pitch = integer(desc.get("row_pitch"), "row_pitch", w * c * size)
    need(pitch * h <= MAX_BYTES, "plane bytes exceeds limit")
    need(desc.get("encoding") in ("raw", "pfm"), "unsupported plane encoding")
    if desc["encoding"] == "pfm":
        need(
            desc["dtype"] == "f32" and c in (1, 3) and pitch == w * c * 4,
            "PFM descriptor mismatch",
        )
    sha(desc.get("sha256"))
    return child(root, desc.get("path")), w, h, c, pitch


def read_plane(root, desc):
    path, w, h, c, pitch = descriptor(root, desc)
    need(path.stat().st_size <= MAX_BYTES + 256, "plane file too large")
    need(digest(path) == desc["sha256"], "plane checksum mismatch")
    data = array("f")
    with path.open("rb") as stream:
        if desc["encoding"] == "raw":
            need(path.stat().st_size == pitch * h, "raw byte count mismatch")
            code = {"f16": "e", "f32": "f", "u8": "B"}[desc["dtype"]]
            count = w * c
            size = struct.calcsize(code)
            for _ in range(h):
                row = stream.read(pitch)
                values = struct.unpack("<" + str(count) + code, row[: count * size])
                data.extend(v / 255 for v in values) if code == "B" else data.extend(
                    values
                )
        else:
            magic = stream.readline(16).strip()
            need(magic == (b"PF" if c == 3 else b"Pf"), "PFM channel header mismatch")
            need(
                stream.readline(64).split() == [str(w).encode(), str(h).encode()],
                "PFM extent mismatch",
            )
            scale = float(stream.readline(64))
            number(scale, "PFM scale")
            need(scale != 0, "PFM zero scale")
            raw = stream.read(pitch * h + 1)
            need(len(raw) == pitch * h, "PFM byte count mismatch")
            for y in range(h - 1, -1, -1):
                values = struct.unpack_from(
                    ("<" if scale < 0 else ">") + str(w * c) + "f", raw, y * pitch
                )
                data.extend(v * abs(scale) for v in values)
    need(all(math.isfinite(v) for v in data), "nonfinite plane pixels")
    return Image(w, h, c, data)


def vector(value, name):
    need(isinstance(value, list) and len(value) == 2, f"{name}: pair required")
    return [number(v, name) for v in value]


def load_capture(path):
    path = Path(path)
    c = json_file(path)
    need(
        c.get("schema") == "retoque-replay-capture"
        and type(c.get("version")) is int
        and c["version"] == 1,
        "unsupported capture schema/version",
    )
    need(c.get("complete") is True, "incomplete capture")
    need(
        c.get("mode") == "nr-boundary" and c.get("clock") == "recorded-evaluation",
        "unsupported replay mode/clock",
    )
    need(
        c.get("color_space") in ("linear-rec709", "srgb-rec709"),
        "unsupported color space",
    )
    need(isinstance(c.get("source"), str) and c["source"], "capture source required")
    need(isinstance(c.get("provenance"), dict), "capture provenance required")
    need(isinstance(c["provenance"].get("producer"), str), "producer required")
    sha(c["provenance"].get("shader_sha256"))
    depth, motion = c.get("depth", {}), c.get("motion", {})
    need(
        isinstance(depth, dict) and isinstance(motion, dict),
        "depth/motion objects required",
    )
    need(depth.get("kind") in ("device", "linear"), "unsupported depth convention")
    boolean(depth.get("inverted"), "depth inverted")
    number(depth.get("threshold"), "depth threshold", 0)
    need(
        motion.get("direction") == "current-to-previous", "unsupported motion direction"
    )
    need(motion.get("units") in ("pixels", "uv"), "unsupported motion units")
    vector(motion.get("scale"), "motion scale")
    boolean(motion.get("includes_jitter"), "motion jitter flag")
    frames = c.get("frames")
    need(isinstance(frames, list) and 1 <= len(frames) <= 10000, "invalid frame count")
    root = path.resolve().parent
    prev = None
    scoring = False
    shape = None
    for f in frames:
        need(isinstance(f, dict), "frame object required")
        integer(f.get("id"), "frame id", 1)
        integer(f.get("present"), "present id", 1)
        number(f.get("time_ms"), "frame time", 0)
        boolean(f.get("reset"), "reset")
        boolean(f.get("score"), "score")
        vector(f.get("jitter"), "jitter")
        need(number(f.get("pre_exposure"), "pre_exposure", 0) > 0, "zero pre_exposure")
        need(number(f.get("exposure_value"), "exposure_value", 0) > 0, "zero exposure")
        need(
            "exposure" in f and isinstance(f.get("params"), dict),
            "exposure/params missing",
        )
        need(f.get("completion") == "fence-complete", "uncertified capture")
        if prev:
            need(
                f["id"] == prev["id"] + 1
                and f["present"] > prev["present"]
                and f["time_ms"] > prev["time_ms"],
                "nonconsecutive frames or nonmonotonic clocks",
            )
        else:
            need(f["reset"], "capture needs reset-prefixed history")
        need(not scoring or f["score"], "warmup after scoring")
        scoring = scoring or f["score"]
        for name, channels in (
            ("color", (3, 4)),
            ("depth", (1,)),
            ("motion", (2,)),
            ("motion_oracle", (2,)),
        ):
            if name == "motion_oracle" and name not in f:
                continue
            _, w, h, ch, _ = descriptor(root, f.get(name))
            need(ch in channels, f"{name}: invalid channels")
            if shape is None:
                shape = (w, h)
            need((w, h) == shape, "v1 requires aligned constant-sized planes")
        if f["exposure"] is not None:
            need(
                descriptor(root, f["exposure"])[3] == 1, "exposure must be scalar plane"
            )
        number(f.get("motion_tolerance_px", 0.05), "motion tolerance", 0)
        prev = f
    regions = c.get("regions")
    need(
        isinstance(regions, dict) and "all" not in regions,
        "invalid regions (all is reserved)",
    )
    for name, rect in regions.items():
        need(
            isinstance(name, str) and re.fullmatch("[A-Za-z0-9_-]{1,64}", name),
            "invalid region name",
        )
        need(isinstance(rect, list) and len(rect) == 4, "region rectangle required")
        x, y, w, h = [integer(v, "region coordinate") for v in rect]
        need(
            w > 0 and h > 0 and x + w <= shape[0] and y + h <= shape[1],
            "region out of bounds",
        )
    c.update(_root=root, _sha256=digest(path), _shape=shape)
    return c


def load_run(path, capture):
    path = Path(path)
    r = json_file(path)
    need(
        r.get("schema") == "retoque-replay-run"
        and type(r.get("version")) is int
        and r["version"] == 1,
        "unsupported run schema/version",
    )
    need(
        r.get("complete") is True and r.get("capture_sha256") == capture["_sha256"],
        "incomplete/mismatched run",
    )
    env = r.get("environment", {})
    need(
        isinstance(env, dict) and isinstance(r.get("producer"), dict),
        "environment/producer objects required",
    )
    sha(env.get("model_sha256"))
    for name in ("driver", "gpu", "runtime"):
        need(
            isinstance(env.get(name), str) and env[name], "missing environment " + name
        )
    sha(r.get("producer", {}).get("dll_sha256"))
    need(
        isinstance(r["producer"].get("commit"), str)
        and isinstance(r.get("settings"), dict),
        "missing run provenance",
    )
    frames = r.get("frames")
    need(
        isinstance(frames, list) and len(frames) == len(capture["frames"]),
        "missing/extra output frames",
    )
    counts = dict(model=0, carried=0)
    for f, source in zip(frames, capture["frames"]):
        need(isinstance(f, dict), "output frame object required")
        need(
            type(f.get("id")) is int and f["id"] == source["id"],
            "output frame identity mismatch",
        )
        kind = f.get("kind")
        need(kind in ("every", "model", "carried"), "invalid frame kind")
        interval = integer(f.get("interval"), "interval", 1)
        need(
            interval == 1 if kind == "every" else 2 <= interval <= 4,
            "kind/interval mismatch",
        )
        requested = integer(f.get("requested_passes"), "requested_passes", 1)
        completed = integer(f.get("completed_passes"), "completed_passes")
        need(
            requested <= 3 and completed == (0 if kind == "carried" else requested),
            "partial/invalid pass coverage",
        )
        need(f.get("completion") == "fence-complete", "uncertified output")
        number(f.get("gpu_ms"), "gpu_ms", 0)
        _, w, h, ch, _ = descriptor(path.parent, f.get("output"))
        need((w, h) == capture["_shape"] and ch in (3, 4), "output shape mismatch")
        if source["score"]:
            counts["carried" if kind == "carried" else "model"] += 1
    requirements = r.get("requirements", {})
    need(isinstance(requirements, dict), "requirements object required")
    for name in counts:
        minimum = integer(requirements.get("min_" + name), "min_" + name)
        need(counts[name] >= minimum, "missing requested " + name + " coverage")
    r.update(_root=path.resolve().parent, _sha256=digest(path), _counts=counts)
    return r

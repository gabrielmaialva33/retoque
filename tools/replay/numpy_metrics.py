"""Vectorized equivalent of metrics.py; float32 storage, float64 arithmetic.

No process-wide NumPy settings or dependency installation. Keep the scalar module
independent so tests and benchmarks can force either implementation.
"""

from array import array
import math
import numpy as np
from .format import Image, need


def pixels(image):
    return np.asarray(image.data, dtype=np.float64).reshape(
        image.height, image.width, image.channels
    )


def stored(values):
    """Match each array('f') rounding boundary in the reference implementation."""
    return np.asarray(values, dtype=np.float32).astype(np.float64)


def rgb(image, space, pre_exposure=1.0):
    space = "srgb-rec709" if image.srgb else space
    source = pixels(image)
    values = np.zeros((image.height, image.width, 3), dtype=np.float64)
    values[..., : min(3, image.channels)] = source[..., :3]
    if space == "srgb-rec709":
        low = values <= 0.04045
        values[low] /= 12.92
        values[~low] = ((values[~low] + 0.055) / 1.055) ** 2.4
    else:
        values /= pre_exposure
    return Image(
        image.width,
        image.height,
        3,
        array("f", np.asarray(values, dtype=np.float32).tobytes()),
    )


def luma(image):
    p = pixels(image)
    return stored(0.2126 * p[..., 0] + 0.7152 * p[..., 1] + 0.0722 * p[..., 2])


def retain(frame, source, depth, output):
    return dict(
        frame=frame,
        source=source,
        depth=depth,
        output=output,
        source_y=luma(source),
        out_y=luma(output),
    )


def coordinates(capture, frame, previous_frame, motion):
    m = capture["motion"]
    sx, sy = m["scale"]
    if m["units"] == "uv":
        sx *= motion.width
        sy *= motion.height
    mv = pixels(motion)
    dx, dy = mv[..., 0] * sx, mv[..., 1] * sy
    if capture["color_jitter"] == "present" and not m["includes_jitter"]:
        dx += previous_frame["jitter"][0] - frame["jitter"][0]
        dy += previous_frame["jitter"][1] - frame["jitter"][1]
    if m.get("_remove_jitter"):
        dx -= previous_frame["jitter"][0] - frame["jitter"][0]
        dy -= previous_frame["jitter"][1] - frame["jitter"][1]
    return dx, dy


def validate_guides(capture, frame, motion, depth, previous_frame, oracle=None):
    d = pixels(depth)[..., 0]
    need(
        bool(
            np.all((d >= 0) & (d <= 1))
            if capture["depth"]["kind"] == "device"
            else np.all(d > 0)
        ),
        "depth outside recorded convention",
    )
    if oracle is None or previous_frame is None or frame["reset"]:
        return
    dx, dy = coordinates(capture, frame, previous_frame, motion)
    # Keep the reference's addition/subtraction order, including its rounding.
    x, y = np.arange(motion.width)[None, :], np.arange(motion.height)[:, None]
    expected = pixels(oracle)
    tolerance = frame.get("motion_tolerance_px", 0.05)
    need(
        bool(
            np.all(np.abs((x + dx) - x - expected[..., 0]) <= tolerance)
            and np.all(np.abs((y + dy) - y - expected[..., 1]) <= tolerance)
        ),
        "motion/jitter disagrees with analytic oracle",
    )


def gradient(values):
    count = values.shape[0] * max(0, values.shape[1] - 1)
    count += values.shape[1] * max(0, values.shape[0] - 1)
    return (
        float(
            (
                np.abs(np.diff(values, axis=1)).sum()
                + np.abs(np.diff(values, axis=0)).sum()
            )
            / count
        )
        if count
        else 0.0
    )


def light(source, output):
    a, b = float(source.sum()), float(output.sum())
    return math.log2(b / a) if a > 0 and b > 0 else None


def frame_metrics(capture, frame, source, depth, motion, output, reference, previous):
    w, h = source.width, source.height
    retained = retain(frame, source, depth, output)
    source_y, out_y, ref_y = retained["source_y"], retained["out_y"], luma(reference)
    out, src = pixels(output), pixels(source)
    errors = stored(np.abs(out - pixels(reference)))
    temporal = np.zeros((h, w), dtype=np.float64)
    valid = np.zeros((h, w), dtype=bool)
    paired = (
        previous is not None
        and not frame["reset"]
        and frame["time_ms"] - previous["frame"]["time_ms"] < 250
    )
    if paired:
        dx, dy = coordinates(capture, frame, previous["frame"], motion)
        qx, qy = np.arange(w)[None, :] + dx, np.arange(h)[:, None] + dy
        valid = (qx >= 0) & (qx <= w - 1) & (qy >= 0) & (qy <= h - 1)
        # Clamp only for safe gather. The original coordinates determine validity.
        qx, qy = np.clip(qx, 0, w - 1), np.clip(qy, 0, h - 1)
        x0, y0 = np.floor(qx).astype(np.intp), np.floor(qy).astype(np.intp)
        x1, y1 = np.minimum(x0 + 1, w - 1), np.minimum(y0 + 1, h - 1)
        fx, fy = (qx - x0)[..., None], (qy - y0)[..., None]

        def sample(image):
            p = pixels(image)
            return (p[y0, x0] * (1 - fx) + p[y0, x1] * fx) * (1 - fy) + (
                p[y1, x0] * (1 - fx) + p[y1, x1] * fx
            ) * fy

        old_depth = sample(previous["depth"])[..., 0]
        current_depth = pixels(depth)[..., 0]
        tolerance = capture["depth"]["threshold"]
        if capture["depth"]["kind"] == "linear":
            tolerance = tolerance * np.maximum(
                np.maximum(np.abs(current_depth), np.abs(old_depth)), 1e-6
            )
        valid &= np.abs(current_depth - old_depth) <= tolerance
        temporal = stored(
            np.abs(
                (out - src) - (sample(previous["output"]) - sample(previous["source"]))
            ).sum(axis=2)
            / 3
        )
    result = {}
    for name, (x, y, rw, rh) in {"all": [0, 0, w, h], **capture["regions"]}.items():
        rect = np.s_[y : y + rh, x : x + rw]
        error = errors[rect]
        pixel_error = error.sum(axis=2) / 3
        mask = valid[rect]
        out_gradient, ref_gradient = gradient(out_y[rect]), gradient(ref_y[rect])
        pulse = None
        if paired:
            old_light = light(previous["source_y"][rect], previous["out_y"][rect])
            new_light = light(source_y[rect], out_y[rect])
            if old_light is not None and new_light is not None:
                pulse = new_light - old_light
        samples = temporal[rect][mask]
        rejected = pixel_error[~mask]
        result[name] = dict(
            golden_mae=float(error.mean()),
            golden_max=float(error.max()),
            golden_bad_fraction=float(
                (error.max(axis=2) > capture["_pixel_tolerance"]).mean()
            ),
            ghost_mae=float(pixel_error.mean()),
            disocclusion_mae=(
                (float(rejected.mean()) if rejected.size else 0.0) if paired else None
            ),
            detail_error=abs(out_gradient - ref_gradient) / max(ref_gradient, 1e-6),
            candidate_gradient=out_gradient,
            reference_gradient=ref_gradient,
            pulse=pulse,
            temporal_mae=float(samples.mean()) if samples.size else None,
            coverage=float(mask.mean()) if paired else None,
            temporal_pair=paired,
        )
    return result, retained

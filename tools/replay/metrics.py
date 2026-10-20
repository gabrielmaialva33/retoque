"""Explicit linear-light metrics and backward-flow correspondence; no hidden defaults."""

from array import array
import math
from .format import Image, need


def percentile(values, fraction):
    if not values:
        return None
    values = sorted(values)
    index = (len(values) - 1) * fraction
    lo, hi = math.floor(index), math.ceil(index)
    return values[lo] + (values[hi] - values[lo]) * (index - lo)


def mean(values):
    return sum(values) / len(values) if values else None


def rgb(image, space, pre_exposure=1.0):
    data = array("f")
    for i in range(0, len(image.data), image.channels):
        for v in image.data[i : i + 3]:
            if space == "srgb-rec709":
                v = v / 12.92 if v <= 0.04045 else ((v + 0.055) / 1.055) ** 2.4
            else:
                v /= pre_exposure
            data.append(v)
    return Image(image.width, image.height, 3, data)


def luma(image):
    return array(
        "f",
        (
            0.2126 * image.data[i]
            + 0.7152 * image.data[i + 1]
            + 0.0722 * image.data[i + 2]
            for i in range(0, len(image.data), 3)
        ),
    )


def coordinates(
    x, y, mv, convention, width, height, current_jitter, previous_jitter, color_jitter
):
    sx, sy = convention["scale"]
    if convention["units"] == "uv":
        sx *= width
        sy *= height
    dx, dy = mv[0] * sx, mv[1] * sy
    if color_jitter == "present" and not convention["includes_jitter"]:
        dx += previous_jitter[0] - current_jitter[0]
        dy += previous_jitter[1] - current_jitter[1]
    return x + dx, y + dy


def sample(image, x, y):
    if x < 0 or y < 0 or x > image.width - 1 or y > image.height - 1:
        return None
    x0, y0 = math.floor(x), math.floor(y)
    x1, y1 = min(x0 + 1, image.width - 1), min(y0 + 1, image.height - 1)
    fx, fy = x - x0, y - y0
    out = []
    for c in range(image.channels):

        def at(px, py):
            return image.data[(py * image.width + px) * image.channels + c]

        out.append(
            (at(x0, y0) * (1 - fx) + at(x1, y0) * fx) * (1 - fy)
            + (at(x0, y1) * (1 - fx) + at(x1, y1) * fx) * fy
        )
    return out


def corresponds(current, previous, depth):
    tolerance = depth["threshold"]
    if depth["kind"] == "linear":
        tolerance *= max(abs(current), abs(previous), 1e-6)
    return abs(current - previous) <= tolerance


def edit_light(source, output):
    a, b = sum(source), sum(output)
    return math.log2(b / a) if a > 0 and b > 0 else None


def light_step(previous_source, previous_output, source, output):
    a, b = edit_light(previous_source, previous_output), edit_light(source, output)
    return None if a is None or b is None else b - a


def gradient(values, width, rect):
    x0, y0, w, h = rect
    total, count = 0.0, 0
    for y in range(y0, y0 + h):
        for x in range(x0, x0 + w):
            p = y * width + x
            if x + 1 < x0 + w:
                total += abs(values[p + 1] - values[p])
                count += 1
            if y + 1 < y0 + h:
                total += abs(values[p + width] - values[p])
                count += 1
    return total / count if count else 0.0


def retain(frame, source, depth, output):
    return dict(
        frame=frame,
        source=source,
        depth=depth,
        output=output,
        source_y=luma(source),
        out_y=luma(output),
    )


def frame_metrics(capture, frame, source, depth, motion, output, reference, previous):
    width, height = source.width, source.height
    regions = {"all": [0, 0, width, height], **capture["regions"]}
    source_y, out_y, ref_y = luma(source), luma(output), luma(reference)
    errors = array("f", (abs(a - b) for a, b in zip(output.data, reference.data)))
    temporal = array("f", [0.0]) * (width * height)
    valid = bytearray(width * height)
    paired = (
        previous is not None
        and not frame["reset"]
        and frame["time_ms"] - previous["frame"]["time_ms"] < 250
    )
    if paired:
        old = previous
        for y in range(height):
            for x in range(width):
                p = y * width + x
                qx, qy = coordinates(
                    x,
                    y,
                    motion.data[2 * p : 2 * p + 2],
                    capture["motion"],
                    width,
                    height,
                    frame["jitter"],
                    old["frame"]["jitter"],
                    capture["color_jitter"],
                )
                old_depth = sample(old["depth"], qx, qy)
                if old_depth is None or not corresponds(
                    depth.data[p], old_depth[0], capture["depth"]
                ):
                    continue
                a, b = sample(old["output"], qx, qy), sample(old["source"], qx, qy)
                temporal[p] = (
                    sum(
                        abs(
                            (output.data[3 * p + c] - source.data[3 * p + c])
                            - (a[c] - b[c])
                        )
                        for c in range(3)
                    )
                    / 3
                )
                valid[p] = 1
    result = {}
    for name, rect in regions.items():
        x0, y0, w, h = rect
        indices = [y * width + x for y in range(y0, y0 + h) for x in range(x0, x0 + w)]
        pixel_error = [sum(errors[3 * p : 3 * p + 3]) / 3 for p in indices]
        all_error = [errors[3 * p + c] for p in indices for c in range(3)]
        out_gradient, ref_gradient = (
            gradient(out_y, width, rect),
            gradient(ref_y, width, rect),
        )
        pulse = None
        if paired:
            pulse = light_step(
                [previous["source_y"][p] for p in indices],
                [previous["out_y"][p] for p in indices],
                [source_y[p] for p in indices],
                [out_y[p] for p in indices],
            )
        samples = [temporal[p] for p in indices if valid[p]]
        disoccluded = [
            pixel_error[i] for i, p in enumerate(indices) if paired and not valid[p]
        ]
        result[name] = dict(
            golden_mae=mean(all_error),
            golden_max=max(all_error),
            golden_bad_fraction=sum(
                max(errors[3 * p : 3 * p + 3]) > capture["_pixel_tolerance"]
                for p in indices
            )
            / len(indices),
            ghost_mae=mean(pixel_error),
            disocclusion_mae=(mean(disoccluded) if disoccluded else 0.0)
            if paired
            else None,
            detail_error=abs(out_gradient - ref_gradient) / max(ref_gradient, 1e-6),
            candidate_gradient=out_gradient,
            reference_gradient=ref_gradient,
            pulse=pulse,
            temporal_mae=mean(samples),
            coverage=len(samples) / len(indices) if paired else None,
            temporal_pair=paired,
        )
    retained = dict(
        frame=frame,
        source=source,
        depth=depth,
        output=output,
        source_y=source_y,
        out_y=out_y,
    )
    return result, retained


def validate_guides(capture, frame, motion, depth, previous_frame, oracle=None):
    need(
        all(0 <= d <= 1 for d in depth.data)
        if capture["depth"]["kind"] == "device"
        else all(d > 0 for d in depth.data),
        "depth outside recorded convention",
    )
    if oracle is None or previous_frame is None or frame["reset"]:
        return
    w, h = motion.width, motion.height
    tolerance = frame.get("motion_tolerance_px", 0.05)
    for y in range(h):
        for x in range(w):
            p = y * w + x
            qx, qy = coordinates(
                x,
                y,
                motion.data[2 * p : 2 * p + 2],
                capture["motion"],
                w,
                h,
                frame["jitter"],
                previous_frame["jitter"],
                capture["color_jitter"],
            )
            need(
                abs(qx - x - oracle.data[2 * p]) <= tolerance
                and abs(qy - y - oracle.data[2 * p + 1]) <= tolerance,
                "motion/jitter disagrees with analytic oracle",
            )

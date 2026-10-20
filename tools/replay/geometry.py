"""Output-grid guide snapshots using the renderer's integer PointSource mapping."""

from array import array
from .format import Image, geometry
from .dxgi import use_numpy


def aligned(image, width, height, base, extent):
    if (
        base == [0, 0]
        and extent == [width, height]
        and (image.width, image.height) == (width, height)
    ):
        return image
    xs = [
        base[0] + min(((2 * x + 1) * extent[0]) // (2 * width), extent[0] - 1)
        for x in range(width)
    ]
    ys = [
        base[1] + min(((2 * y + 1) * extent[1]) // (2 * height), extent[1] - 1)
        for y in range(height)
    ]
    if use_numpy():
        import numpy as np

        pixels = np.asarray(image.data).reshape(
            image.height, image.width, image.channels
        )
        data = array(
            "f",
            pixels[np.asarray(ys)[:, None], np.asarray(xs)[None, :]]
            .astype(np.float32)
            .tobytes(),
        )
    else:
        data = array(
            "f",
            (
                image.data[(y * image.width + x) * image.channels + c]
                for y in ys
                for x in xs
                for c in range(image.channels)
            ),
        )
    return Image(width, height, image.channels, data)


def prepare(capture, frame, depth, motion):
    if capture["version"] == 1:
        return capture, frame, depth, motion
    depth = components(depth, 1)
    motion = components(motion, 2)
    p = frame["params"]
    w, h = frame["color"]["width"], frame["color"]["height"]
    regions = geometry(frame, capture)
    depth_base, render = regions["depth"]
    motion_base, motion_extent = regions["motion"]
    low = bool(p["create_flags"] & 2)
    depth = aligned(depth, w, h, depth_base, render)
    motion = aligned(motion, w, h, motion_base, motion_extent)
    scale = [
        p["mv_scale"][0] * (w / render[0] if low else 1),
        p["mv_scale"][1] * (h / render[1] if low else 1),
    ]
    convention = dict(
        capture["motion"],
        scale=scale,
        _remove_jitter=capture["color_jitter"] == "removed"
        and capture["motion"]["includes_jitter"],
    )
    # NGX jitter is in render pixels, irrespective of motion-vector resolution.
    frame = dict(
        frame,
        jitter=[frame["jitter"][0] * w / render[0], frame["jitter"][1] * h / render[1]],
    )
    return dict(capture, motion=convention), frame, depth, motion


def components(image, count):
    """D3D typed-load components: absent RGB channels read zero; depth uses .r, motion .xy."""
    if image.channels == count:
        return image
    if use_numpy():
        import numpy as np

        p = np.asarray(image.data).reshape(-1, image.channels)
        values = np.zeros((len(p), count), dtype=np.float32)
        n = min(count, image.channels)
        values[:, :n] = p[:, :n]
        data = array("f", values.tobytes())
    else:
        data = array(
            "f",
            (
                image.data[i + c] if c < image.channels else 0
                for i in range(0, len(image.data), image.channels)
                for c in range(count)
            ),
        )
    return Image(image.width, image.height, count, data)

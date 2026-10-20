"""Native little-endian DXGI decoding. Values stay encoded; rgb() linearizes colour once."""

from array import array
import math
import os
import struct

# format: (bytes, channels, scalar code). Packed formats have no scalar code.
FORMATS = {
    2: (16, 4, "f"),
    10: (8, 4, "e"),
    16: (8, 2, "f"),
    26: (4, 3, None),
    24: (4, 4, None),
    28: (4, 4, "B"),
    29: (4, 4, "B"),
    87: (4, 4, "B"),
    91: (4, 4, "B"),
    41: (4, 1, "f"),
    40: (4, 1, "f"),
    34: (4, 2, "e"),
}


def unsigned_float(bits, mantissa):
    fraction = bits & ((1 << mantissa) - 1)
    exponent = bits >> mantissa
    if exponent == 31:
        return math.nan if fraction else math.inf
    return (
        math.ldexp(fraction, 1 - 15 - mantissa)
        if not exponent
        else math.ldexp(1 + fraction / (1 << mantissa), exponent - 15)
    )


def decode(raw, fmt, numpy=False):
    size, channels, code = FORMATS[fmt]
    if numpy:
        import numpy as np

        if code:
            values = (
                np.frombuffer(raw, dtype={"f": "<f4", "e": "<f2", "B": "u1"}[code])
                .astype(np.float32)
                .reshape(-1, channels)
            )
            if code == "B":
                values = (values.astype(np.float64) / 255).astype(np.float32)
                if fmt in (87, 91):
                    values = values[:, [2, 1, 0, 3]]
        else:
            words = np.frombuffer(raw, dtype="<u4")
            if fmt == 24:
                values = np.stack(
                    [
                        (words >> shift & mask).astype(np.float64) / mask
                        for shift, mask in ((0, 1023), (10, 1023), (20, 1023), (30, 3))
                    ],
                    axis=1,
                )
            else:
                parts = []
                for shift, m in ((0, 6), (11, 6), (22, 5)):
                    bits = (words >> shift) & ((1 << (m + 5)) - 1)
                    exponent = bits >> m
                    fraction = bits & ((1 << m) - 1)
                    v = np.ldexp(
                        1 + fraction.astype(np.float64) / (1 << m),
                        exponent.astype(np.int32) - 15,
                    )
                    v = np.where(
                        exponent == 0,
                        np.ldexp(fraction.astype(np.float64), 1 - 15 - m),
                        v,
                    )
                    v = np.where(
                        exponent == 31, np.where(fraction == 0, np.inf, np.nan), v
                    )
                    parts.append(v)
                values = np.stack(parts, axis=1)
        return array("f", np.asarray(values, dtype=np.float32).tobytes())
    out = array("f")
    if code:
        for pixel in struct.iter_unpack("<" + str(channels) + code, raw):
            if code == "B":
                if fmt in (87, 91):
                    pixel = (pixel[2], pixel[1], pixel[0], pixel[3])
                out.extend(v / 255 for v in pixel)
            else:
                out.extend(pixel)
    else:
        for (word,) in struct.iter_unpack("<I", raw):
            if fmt == 24:
                out.extend(
                    ((word >> s) & mask) / mask
                    for s, mask in ((0, 1023), (10, 1023), (20, 1023), (30, 3))
                )
            else:
                out.extend(
                    unsigned_float((word >> s) & ((1 << (m + 5)) - 1), m)
                    for s, m in ((0, 6), (11, 6), (22, 5))
                )
    return out


def use_numpy():
    if os.environ.get("RETOQUE_REPLAY_BACKEND") == "python":
        return False
    try:
        import numpy  # noqa: F401
    except ImportError:
        return False
    return True

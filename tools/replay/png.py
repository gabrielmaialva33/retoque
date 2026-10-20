"""Bounded RGB previews and standards-compliant PNG using only the standard library."""

import json
import struct
import zlib


def write_png(path, width, height, pixels, metadata):
    if len(pixels) != width * height * 3 or width < 1 or height < 1:
        raise ValueError("PNG dimensions do not match RGB bytes")

    def chunk(kind, data):
        return (
            struct.pack(">I", len(data))
            + kind
            + data
            + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)
        )

    raw = b"".join(
        b"\0" + pixels[y * width * 3 : (y + 1) * width * 3] for y in range(height)
    )
    data = b"\x89PNG\r\n\x1a\n"
    data += chunk(b"IHDR", struct.pack(">2I5B", width, height, 8, 2, 0, 0, 0))
    data += chunk(
        b"tEXt",
        b"Description\0" + json.dumps(metadata, ensure_ascii=True).encode("ascii"),
    )
    data += chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b"")
    path.write_bytes(data)


def preview(image, max_width=192):
    width = min(image.width, max_width)
    height = max(1, round(image.height * width / image.width))
    pixels = bytearray()
    for y in range(height):
        for x in range(width):
            p = (
                min(image.height - 1, y * image.height // height) * image.width
                + x * image.width // width
            ) * 3
            for v in image.data[p : p + 3]:
                # Fixed Reinhard + sRGB, including HDR: same mapping for every panel.
                v = max(0, v) / (1 + max(0, v))
                v = 12.92 * v if v <= 0.0031308 else 1.055 * v ** (1 / 2.4) - 0.055
                pixels.append(round(255 * min(1, v)))
    return width, height, bytes(pixels)


def heatmap(output, reference, tolerance):
    w, h, _ = preview(output)
    pixels = bytearray()
    for y in range(h):
        for x in range(w):
            p = (y * output.height // h * output.width + x * output.width // w) * 3
            error = max(
                abs(output.data[p + c] - reference.data[p + c]) for c in range(3)
            )
            level = min(1, error / max(tolerance, 1e-12))
            pixels.extend((round(255 * level), 0, round(255 * (1 - level))))
    return w, h, bytes(pixels)


def contact(rows):
    width, height, _ = rows[0]["panels"][0]
    out = bytearray()
    for row in rows:
        for y in range(height):
            for pw, ph, pixels in row["panels"]:
                if (pw, ph) != (width, height):
                    raise ValueError("contact dimensions differ")
                out.extend(pixels[y * width * 3 : (y + 1) * width * 3])
    return width * 3, height * len(rows), bytes(out)

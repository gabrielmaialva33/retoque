"""Codepoints of the menu's embedded Hack font (OptiScaler/menu/font/Hack_Compressed.h).

The header is ImGui's binary_to_compressed_c output: base85 text of an stb_compress stream of the TTF. This decodes
it the way imgui_draw.cpp does (Decode85, stb_decompress) and reads the font's Unicode cmap (formats 4 and 12).
"""
import re
import struct


def embedded_font(header_text):
    body = header_text[header_text.index("=") + 1:header_text.rindex(";")]
    data = "".join(re.findall(r'"((?:\\.|[^"\\])*)"', body))
    data = re.sub(r"\\(.)", lambda m: m.group(1), data)  # the encoder escapes only '?' (trigraphs), '"' and '\'
    raw = bytearray()
    digit = lambda c: ord(c) - 36 if c >= "\\" else ord(c) - 35
    for k in range(0, len(data) - len(data) % 5, 5):
        raw += struct.pack("<I", sum(digit(data[k + j]) * 85 ** j for j in range(5)) & 0xFFFFFFFF)
    return stb_decompress(bytes(raw))


def stb_decompress(i):
    be = lambda p, n: int.from_bytes(i[p:p + n], "big")
    assert be(0, 4) == 0x57BC0000 and be(4, 4) == 0, "not an stb_compress stream"
    olen = be(8, 4)
    out = bytearray()
    p = 16

    def match(dist, length):
        start = len(out) - dist
        assert start >= 0
        for k in range(length):
            out.append(out[start + k])

    while True:
        c = i[p]
        if c >= 0x80:
            match(i[p + 1] + 1, c - 0x80 + 1); p += 2
        elif c >= 0x40:
            match(be(p, 2) - 0x4000 + 1, i[p + 2] + 1); p += 3
        elif c >= 0x20:
            n = c - 0x20 + 1; out += i[p + 1:p + 1 + n]; p += 1 + n
        elif c >= 0x18:
            match(be(p, 3) - 0x180000 + 1, i[p + 3] + 1); p += 4
        elif c >= 0x10:
            match(be(p, 3) - 0x100000 + 1, be(p + 3, 2) + 1); p += 5
        elif c >= 0x08:
            n = be(p, 2) - 0x0800 + 1; out += i[p + 2:p + 2 + n]; p += 2 + n
        elif c == 0x07:
            n = be(p + 1, 2) + 1; out += i[p + 3:p + 3 + n]; p += 3 + n
        elif c == 0x06:
            match(be(p + 1, 3) + 1, i[p + 4] + 1); p += 5
        elif c == 0x04:
            match(be(p + 1, 3) + 1, be(p + 4, 2) + 1); p += 6
        elif c == 0x05 and i[p + 1] == 0xFA:
            break
        else:
            raise ValueError("bad stb_compress token")
    assert len(out) == olen, "stb_decompress length mismatch"
    return bytes(out)


def cmap(ttf):
    u16 = lambda p: struct.unpack_from(">H", ttf, p)[0]
    u32 = lambda p: struct.unpack_from(">I", ttf, p)[0]
    tables = {ttf[12 + 16 * k:16 + 16 * k].decode("latin-1"): u32(20 + 16 * k) for k in range(u16(4))}
    base = tables["cmap"]
    points = set()
    for k in range(u16(base + 2)):
        sub = base + u32(base + 4 + 8 * k + 4)
        fmt = u16(sub)
        if fmt == 4:
            segs = u16(sub + 6) // 2
            ends = sub + 14
            starts = ends + 2 * segs + 2
            deltas = starts + 2 * segs
            offsets = deltas + 2 * segs
            for s in range(segs):
                end, start, delta, offset = (u16(ends + 2 * s), u16(starts + 2 * s), u16(deltas + 2 * s),
                                             u16(offsets + 2 * s))
                for c in range(start, end + 1):
                    if c == 0xFFFF:
                        continue
                    glyph = (c + delta) & 0xFFFF if offset == 0 else u16(offsets + 2 * s + offset + 2 * (c - start))
                    if offset and glyph:
                        glyph = (glyph + delta) & 0xFFFF
                    if glyph:
                        points.add(c)
        elif fmt == 12:
            for g in range(u32(sub + 12)):
                points.update(range(u32(sub + 16 + 12 * g), u32(sub + 20 + 12 * g) + 1))
    return points


def hack_codepoints(root):
    header = (root / "OptiScaler/menu/font/Hack_Compressed.h").read_text(encoding="utf-8-sig")
    ttf = embedded_font(header)
    assert len(ttf) == int(re.search(r"\((\d+) bytes\)", header).group(1)), "decoded Hack size differs from its header"
    return cmap(ttf)

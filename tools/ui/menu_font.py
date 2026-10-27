# /// script
# requires-python = ">=3.12"
# dependencies = ["pillow==12.2.0", "numpy==2.5.4", "scipy==1.18.1"]
# ///
"""Convert the menu's pixel font from BDF into the glyph table packed for the firmware.

Reads raws/ui/common/fonts/fusion-pixel-12px-proportional-zh_hans.bdf.gz
(Fusion Pixel Font 12px, proportional, Simplified Chinese glyph forms, OFL-1.1)
and writes assets/ui/common/menu_font.bin. The font covers Latin, CJK,
kana and Hangul, so file names in those scripts show as written.

Layout, little endian:
  header   magic "GBXFONT\\0", u16 version, u8 line height, u8 ascent,
           u32 range count, u32 glyph count
  ranges   per run of consecutive code points: u32 first, u32 count,
           u32 index of its first glyph
  offsets  u32 per glyph, from the start of the glyph data
  glyphs   u8 advance, i8 left, i8 top (rows below the top of the line),
           u8 width, u8 height, then width * height bits row by row, most
           significant bit first, padded to a whole byte
"""

import gzip
import struct
from pathlib import Path

from uikit import ASSETS, RAWS

SOURCE = RAWS / "common" / "fonts" / "fusion-pixel-12px-proportional-zh_hans.bdf.gz"
OUTPUT = ASSETS / "common" / "menu_font.bin"
MAGIC = b"GBXFONT\0"
VERSION = 1
HEADER = struct.Struct("<8sHBBII")
RANGE = struct.Struct("<III")
GLYPH = struct.Struct("<BbbBB")


def read_bdf(path):
    """(line height, ascent, {code point: (advance, left, top, width, height, rows)})."""
    with gzip.open(path, "rt", encoding="utf-8") as f:
        lines = f.read().splitlines()
    ascent = descent = None
    glyphs = {}
    i = 0
    while i < len(lines):
        key = lines[i].split(" ", 1)[0]
        if key == "FONT_ASCENT":
            ascent = int(lines[i].split()[1])
        elif key == "FONT_DESCENT":
            descent = int(lines[i].split()[1])
        elif key == "STARTCHAR":
            code = advance = box = None
            i += 1
            while lines[i] != "BITMAP":
                field = lines[i].split()
                if field[0] == "ENCODING":
                    code = int(field[1])
                elif field[0] == "DWIDTH":
                    advance = int(field[1])
                elif field[0] == "BBX":
                    box = tuple(int(v) for v in field[1:5])
                i += 1
            i += 1
            rows = []
            while lines[i] != "ENDCHAR":
                rows.append(int(lines[i], 16))
                i += 1
            if code is not None and code >= 0:
                glyphs[code] = (advance, box, rows)
        i += 1
    if ascent is None or descent is None:
        raise SystemExit(f"{path} has no FONT_ASCENT or FONT_DESCENT")
    out = {}
    for code, (advance, (w, h, x, y), rows) in glyphs.items():
        nbytes = (w + 7) // 8
        bits = [[(r >> (nbytes * 8 - 1 - c)) & 1 for c in range(w)] for r in rows]
        # BDF places the box's bottom y px above the baseline.
        out[code] = (advance, x, ascent - y - h, w, h, bits)
    return ascent + descent, ascent, out


def pack(line_height, ascent, glyphs):
    codes = sorted(glyphs)
    ranges = []
    for index, code in enumerate(codes):
        if ranges and ranges[-1][0] + ranges[-1][1] == code:
            ranges[-1][1] += 1
        else:
            ranges.append([code, 1, index])
    offsets, data = [], bytearray()
    for code in codes:
        advance, left, top, w, h, bits = glyphs[code]
        offsets.append(len(data))
        data += GLYPH.pack(advance, left, top, w, h)
        flat = [b for row in bits for b in row]
        flat += [0] * (-len(flat) % 8)
        data += bytes(sum(bit << (7 - j) for j, bit in enumerate(flat[k:k + 8])) for k in range(0, len(flat), 8))
    return (HEADER.pack(MAGIC, VERSION, line_height, ascent, len(ranges), len(codes))
            + b"".join(RANGE.pack(*r) for r in ranges)
            + struct.pack(f"<{len(offsets)}I", *offsets) + bytes(data))


class MenuFont:
    """The packed font as the firmware reads it: lookup, measuring and glyph bits."""

    def __init__(self, path=OUTPUT):
        raw = Path(path).read_bytes()
        magic, version, self.line_height, self.ascent, nranges, nglyphs = HEADER.unpack_from(raw)
        if magic != MAGIC or version != VERSION:
            raise SystemExit(f"{path} is not a version {VERSION} menu font")
        at = HEADER.size
        self.ranges = [RANGE.unpack_from(raw, at + i * RANGE.size) for i in range(nranges)]
        at += nranges * RANGE.size
        self.offsets = struct.unpack_from(f"<{nglyphs}I", raw, at)
        self.data = raw[at + 4 * nglyphs:]
        self.cache = {}

    def glyph(self, code):
        """(advance, left, top, width, height, rows of bits), or None when the font lacks it."""
        if code in self.cache:
            return self.cache[code]
        found = None
        lo, hi = 0, len(self.ranges)
        while lo < hi:
            mid = (lo + hi) // 2
            first, count, index = self.ranges[mid]
            if code < first:
                hi = mid
            elif code >= first + count:
                lo = mid + 1
            else:
                at = self.offsets[index + code - first]
                advance, left, top, w, h = GLYPH.unpack_from(self.data, at)
                bits = self.data[at + GLYPH.size:at + GLYPH.size + (w * h + 7) // 8]
                flat = [(bits[k // 8] >> (7 - k % 8)) & 1 for k in range(w * h)]
                found = (advance, left, top, w, h, [flat[r * w:(r + 1) * w] for r in range(h)])
                break
        self.cache[code] = found
        return found


def main():
    line_height, ascent, glyphs = read_bdf(SOURCE)
    data = pack(line_height, ascent, glyphs)
    OUTPUT.write_bytes(data)
    print(f"{OUTPUT.name}: {len(glyphs)} glyphs, {len(data)} bytes")


if __name__ == "__main__":
    main()

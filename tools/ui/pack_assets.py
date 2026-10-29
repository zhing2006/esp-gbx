# /// script
# requires-python = ">=3.12"
# dependencies = ["pillow==12.2.0", "numpy==2.5.4", "scipy==1.18.1"]
# ///
"""Pack one board's UI assets into the binary image flashed to the assets partition.

The firmware build runs this; the format is read by ui/pack.cpp, and the record
layouts here must match ui/include/gbx/ui/pack_format.h. Sprites are rendered at
their final board position, as render_previews.py draws them, so the firmware
only copies and blends whole pixels. Colors are converted to little-endian
RGB565 with 4x4 ordered dithering in screen coordinates; sprites and font
atlases carry a separate 8-bit alpha plane after the color plane. The menus'
colors, palettes and icons from menu.json and the menu font from
menu_font.bin go in as they are, since the firmware draws the menus itself,
and so do the samples of the menus' sound cues in sounds/*.wav.
"""

import argparse
import json
import math
import struct
import sys
import wave
import zlib
from pathlib import Path

import numpy as np
from PIL import Image

from build_sprites import SHAPES
from uikit import ASSETS, resample

VERSION = 4
MAGIC = b"GBXUIPK\0"
ALIGN = 64
HIT_MARGIN = 24  # master px around each control's shape that still counts as a press
MIN_HIT_MM = 10  # thin controls get a margin that makes their hit area at least this wide

# Nominal panel diagonals in inches, from docs/boards; Sila's is the "4IN"
# label on its schematic. They turn MIN_HIT_MM into pixels. m5stack_tab5_x3
# is a layout of the Tab5's panel with a 720x480 game window.
PANEL_INCHES = {"sila_p4": 4.0, "sila_p4x": 6.2, "jc1060p470": 7.0, "m5stack_tab5": 5.0, "m5stack_tab5_x3": 5.0,
                "osptek_p4c5": 4.3, "wks_s31_cb": 4.3, "esp32_s31_korvo_1": 4.3, "waveshare_c5_lcd28": 2.8}

HEADER = struct.Struct("<8sIIIII HH32s")
ENTRY = struct.Struct("<32sIII4x")
IMAGE = struct.Struct("<HHHHhhI")
LAYOUT = struct.Struct("<4h4H64s")
CONTROL = struct.Struct("<16sBB2x7f")
LAMP = struct.Struct("<8s8s8s")
FONT = struct.Struct("<BBHHHH2x")
CELL = struct.Struct("<8s4f4f3f48s8sBBBB")
LINE = struct.Struct("<2f96sBB2x")
MAX_CELL_LINES = 8
MENU = struct.Struct("<HHBBBB")
PALETTE = struct.Struct("<8s12s")
ICON = struct.Struct("<12s16H")
SOUND = struct.Struct("<II")

ENTRY_IMAGE, ENTRY_LAYOUT, ENTRY_MENU, ENTRY_FONT, ENTRY_SOUND = 1, 2, 3, 4, 5
# Menu colors in the order of pack_format::MenuColor.
MENU_COLORS = ["paper", "card", "ink", "muted", "disabled", "rule", "header", "header_ink", "header_muted",
               "accent", "accent_ink", "accent_dim", "alert", "gb", "gbc", "gba", "badge_ink", "clean", "dirty",
               "error", "scrim", "shadow"]
FORMAT_RGB565, FORMAT_RGB565A8 = 1, 2
KIND_BUTTON, KIND_DPAD, KIND_SWITCH = 0, 1, 2
SHAPE_CIRCLE, SHAPE_CAPSULE, SHAPE_RECT = 0, 1, 2
UNIT_NONE, UNIT_RIGHT, UNIT_BELOW = 0, 1, 2

BAYER4 = (np.array([[0, 8, 2, 10], [12, 4, 14, 6], [3, 11, 1, 9], [15, 7, 13, 5]], dtype=np.float32) + 0.5) / 16


def to_rgb565(rgb, x0=0, y0=0, dither=True):
    """uint16 RGB565 of an (h, w, 3) uint8 array whose top-left pixel sits at (x0, y0) on screen."""
    h, w, _ = rgb.shape
    if dither:
        ys = (np.arange(h) + y0) % 4
        xs = (np.arange(w) + x0) % 4
        t = BAYER4[ys[:, None], xs[None, :]][..., None]
    else:
        t = np.full((h, w, 1), 0.5, dtype=np.float32)
    levels = np.array([31, 63, 31], dtype=np.float32)
    q = np.minimum(np.floor(rgb.astype(np.float32) / 255 * levels + t), levels).astype(np.uint16)
    return (q[..., 0] << 11) | (q[..., 1] << 5) | q[..., 2]


def cstr(text, size):
    raw = text.encode("ascii")
    if len(raw) >= size:
        raise SystemExit(f"'{text}' does not fit in {size - 1} bytes")
    return raw


class Pack:
    def __init__(self):
        self.entries = []  # (name, type, payload)

    def add(self, name, kind, payload):
        if any(n == name for n, _, _ in self.entries):
            raise SystemExit(f"duplicate pack entry {name}")
        self.entries.append((cstr(name, 32), kind, payload))

    def add_image(self, name, im, x=0, y=0, dither=True):
        """Store an RGB or RGBA image whose top-left corner is (x, y) on screen."""
        a = np.asarray(im)
        rgb565 = to_rgb565(a[..., :3], x, y, dither)
        h, w = rgb565.shape
        data = rgb565.astype("<u2").tobytes()
        fmt = FORMAT_RGB565
        if im.mode == "RGBA":
            data += a[..., 3].astype(np.uint8).tobytes()
            fmt = FORMAT_RGB565A8
        self.add(name, ENTRY_IMAGE, (fmt, w, h, w * 2, x, y, data))

    def build(self, board, size):
        def align(n):
            return (n + ALIGN - 1) // ALIGN * ALIGN

        offset = align(HEADER.size + ENTRY.size * len(self.entries))
        table, blobs = [], []
        for name, kind, payload in self.entries:
            if kind == ENTRY_IMAGE:
                fmt, w, h, stride, x, y, data = payload
                data_offset = align(offset + IMAGE.size)
                blob = IMAGE.pack(fmt, w, h, stride, x, y, data_offset)
                blob += b"\0" * (data_offset - offset - IMAGE.size) + data
            else:
                blob = payload
            table.append(ENTRY.pack(name, kind, offset, len(blob)))
            blobs.append((offset, blob))
            offset = align(offset + len(blob))
        body = bytearray(offset - HEADER.size)
        entries = b"".join(table)
        body[:len(entries)] = entries
        for start, blob in blobs:
            body[start - HEADER.size:start - HEADER.size + len(blob)] = blob
        header = HEADER.pack(MAGIC, VERSION, offset, zlib.crc32(body), len(table), HEADER.size,
                             size[0], size[1], cstr(board, 32))
        return header + bytes(body)


def crop_visible(im, x, y, size):
    """Crop a sprite at (x, y) to its visible pixels on a screen of `size`, keeping its position."""
    im = im.crop((max(0, -x), max(0, -y), min(im.width, size[0] - x), min(im.height, size[1] - y)))
    x, y = max(0, x), max(0, y)
    box = im.getchannel("A").getbbox()
    if box is None:
        raise SystemExit("sprite has no visible pixels")
    return im.crop(box), x + box[0], y + box[1]


def hit_shape(sprite, center, k):
    """(shape, a, b, angle, dead) of a control in board px, from the fitted geometry."""
    s = SHAPES[sprite]
    kind = s["kind"]
    if kind == "circle":
        return SHAPE_CIRCLE, s["radius"] * k, 0.0, 0.0, 0.0
    if kind == "cross":
        return SHAPE_CIRCLE, s["size"] / 2 * k, 0.0, 0.0, s["arm"] / 4 * k
    if kind == "capsule":
        return SHAPE_CAPSULE, (s["length"] - s["thick"]) / 2 * k, s["thick"] / 2 * k, math.radians(s["angle"]), 0.0
    if kind == "bar":
        x0, y0, x1, y1 = s["box"]
        return SHAPE_RECT, (x1 - x0) / 2 * k, (y1 - y0) / 2 * k, 0.0, 0.0
    raise SystemExit(f"no hit shape for {sprite}")


def menu_entry(window):
    """The "menu" entry: grid, the scale that fills the game window `window`
    with it, colors in MENU_COLORS order, palettes and 16x16 icons."""
    theme = json.load(open(ASSETS / "common" / "menu.json"))
    colors = theme["colors"]
    if sorted(colors) != sorted(MENU_COLORS):
        raise SystemExit(f"menu.json colors must be exactly {MENU_COLORS}")
    gw, gh = theme["grid"]
    scale = window[2] // gw
    if scale < 1 or window[2:] != [gw * scale, gh * scale]:
        raise SystemExit(f"the {window[2]}x{window[3]} game window is not the {gw}x{gh} menu grid at a whole scale")
    out = MENU.pack(gw, gh, scale, len(MENU_COLORS), len(theme["palettes"]), len(theme["icons"]))
    out += b"".join(bytes(colors[name]) + b"\0" for name in MENU_COLORS)
    for name, shades in theme["palettes"].items():
        if len(shades) != 4:
            raise SystemExit(f"palette {name} needs 4 shades")
        out += PALETTE.pack(cstr(name, 8), b"".join(bytes(s) for s in shades))
    for name, rows in theme["icons"].items():
        if len(rows) != 16 or any(len(r) != 16 for r in rows):
            raise SystemExit(f"icon {name} must be 16x16")
        out += ICON.pack(cstr(name, 12), *(int(r.replace("#", "1").replace(".", "0"), 2) for r in rows))
    return out


def build(board):
    m = json.load(open(ASSETS / board / "board.json"))
    sp = json.load(open(ASSETS / "common" / "sprites.json"))
    font = json.load(open(ASSETS / "common" / "vfd_font.json"))
    k = m["sprite_scale"]
    px_per_mm = math.hypot(*m["size"]) / (PANEL_INCHES[board] * 25.4)

    def margin(half_width):
        """Margin for a shape whose narrowest half extent is `half_width` board px."""
        return max(HIT_MARGIN * k, MIN_HIT_MM / 2 * px_per_mm - half_width)

    pack = Pack()
    cache = {}

    def sprite(file, center, anchor):
        """A common sprite scaled and positioned on this board: (image, x, y)."""
        if file not in cache:
            cache[file] = Image.open(ASSETS / "common" / file).convert("RGBA")
        return crop_visible(*resample(cache[file], center, anchor, k), m["size"])

    pack.add_image("bg", Image.open(ASSETS / board / m["background"]).convert("RGB"))

    controls = []
    for cid, c in m["controls"].items():
        meta = sp["sprites"][c["sprite"]]
        for state, file in meta["states"].items():
            im, x, y = sprite(file, c["center"], meta["anchor"])
            pack.add_image(f"ctl.{cid}.{state}", im, x, y)
        shape, a, b, angle, dead = hit_shape(c["sprite"], c["center"], k)
        kind = KIND_DPAD if c["sprite"] == "dpad" else KIND_BUTTON
        half_width = a if shape == SHAPE_CIRCLE else b if shape == SHAPE_CAPSULE else min(a, b)
        controls.append(CONTROL.pack(cstr(cid, 16), kind, shape, *c["center"], a, b, angle, margin(half_width), dead))

    knob = sp["sprites"]["knob"]
    for pos in ("1x", "2x"):
        im, x, y = sprite(knob["states"]["normal"], m["speed_switch"][pos], knob["anchor"])
        pack.add_image(f"ctl.speed.{pos}", im, x, y)
    sx, sy, sw, sh = m["speed_switch"]["slot"]
    controls.append(CONTROL.pack(cstr("speed", 16), KIND_SWITCH, SHAPE_RECT, sx + sw / 2, sy + sh / 2,
                                 sw / 2, sh / 2, 0.0, margin(min(sw, sh) / 2), 0.0))

    lamps = []
    for lid, colors in sp["lamps"]["use"].items():
        for color in colors:
            im, x, y = sprite(sp["lamps"]["colors"][color], m["lamps"][lid]["center"], sp["lamps"]["anchor"])
            pack.add_image(f"lamp.{lid}.{color}", im, x, y)
        names = [cstr(c, 8) for c in colors] + [b""] * (2 - len(colors))
        lamps.append(LAMP.pack(cstr(lid, 8), *names))

    # A board without a VFD gets no VFD font either.
    vfd = m.get("vfd", {})
    fonts = []
    for p, a in sorted(font["atlases"].items(), key=lambda kv: int(kv[0])) if vfd else []:
        # Glyphs land on varying positions, so the atlas is not dithered.
        pack.add_image(f"font.{p}", Image.open(ASSETS / "common" / a["file"]).convert("RGBA"), dither=False)
        fonts.append(FONT.pack(a["pitch"], a["margin"], a["cell"][0], a["cell"][1], a["columns"], a["advance"]))

    cells = []
    positions = {None: UNIT_NONE, "right": UNIT_RIGHT, "below": UNIT_BELOW}
    aligns = {"left": 0, "right": 1}
    for cid, c in vfd.items():
        v, lines = c["value"], c.get("lines", [])
        if len(lines) > MAX_CELL_LINES:
            raise SystemExit(f"VFD cell {cid} has {len(lines)} lines, at most {MAX_CELL_LINES} fit")
        unit = v.get("unit", "")
        packed = [LINE.pack(ln["x"], ln["top"], cstr(ln["text"], 96), ln["pitch"], aligns[ln["align"]])
                  for ln in lines]
        packed += [bytes(LINE.size)] * (MAX_CELL_LINES - len(lines))
        cells.append(CELL.pack(
            cstr(cid, 8), *c["rect"], *c["label"], v["right"], v["top"], v["left_limit"],
            cstr(v["binding"], 48), cstr(unit, 8),
            v["pitch"], v.get("unit_pitch", v["pitch"]), positions[v.get("unit_position") if unit else None],
            len(lines)) + b"".join(packed))

    layout = LAYOUT.pack(*m["display"], len(controls), len(lamps), len(fonts), len(cells), cstr(font["chars"], 64))
    pack.add("layout", ENTRY_LAYOUT, layout + b"".join(controls + lamps + fonts + cells))
    pack.add("menu", ENTRY_MENU, menu_entry(m["display"]))
    pack.add("menu_font", ENTRY_FONT, (ASSETS / "common" / "menu_font.bin").read_bytes())
    for path in sorted((ASSETS / "common" / "sounds").glob("*.wav")):
        with wave.open(str(path), "rb") as w:
            if w.getnchannels() != 1 or w.getsampwidth() != 2:
                raise SystemExit(f"{path} is not 16-bit mono")
            frames = w.getnframes()
            pack.add(f"sound.{path.stem}", ENTRY_SOUND, SOUND.pack(w.getframerate(), frames) + w.readframes(frames))
    return pack.build(board, m["size"])


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--board", required=True)
    p.add_argument("--output", required=True)
    p.add_argument("--limit", type=lambda s: int(s, 0), help="partition size; fail if the pack is larger")
    args = p.parse_args()
    data = build(args.board)
    if args.limit and len(data) > args.limit:
        print(f"asset pack is {len(data)} bytes, partition holds {args.limit}", file=sys.stderr)
        sys.exit(1)
    out = Path(args.output)
    out.parent.mkdir(parents=True, exist_ok=True)
    out.write_bytes(data)
    print(f"{args.board}: {len(data)} bytes")


if __name__ == "__main__":
    main()

# /// script
# requires-python = ">=3.12"
# dependencies = ["pillow==12.2.0", "numpy==2.5.4", "scipy==1.18.1"]
# ///
"""5x7 dot-matrix VFD font: glyph table, glowing atlases per dot pitch, and text layout.

Run to write assets/ui/common/vfd_font.json and vfd_p<pitch>.png. Each atlas
cell holds one glyph with its glow; the dot grid starts `margin` px inside the
cell, and the pen advances 6 dots per character.
"""

import json
import math

import numpy as np
from PIL import Image
from scipy import ndimage as ndi

from uikit import ASSETS

PITCHES = (2, 3, 4)
COLUMNS = 16
CORE = np.array((150, 255, 252), dtype=np.float32) / 255
EDGE = np.array((88, 230, 228), dtype=np.float32) / 255
GLOW = np.array((70, 225, 215), dtype=np.float32) / 255

GLYPHS = {
    " ": [".....", ".....", ".....", ".....", ".....", ".....", "....."],
    "0": [".###.", "#...#", "#..##", "#.#.#", "##..#", "#...#", ".###."],
    "1": ["..#..", ".##..", "..#..", "..#..", "..#..", "..#..", ".###."],
    "2": [".###.", "#...#", "....#", "...#.", "..#..", ".#...", "#####"],
    "3": ["#####", "...#.", "..#..", "...#.", "....#", "#...#", ".###."],
    "4": ["...#.", "..##.", ".#.#.", "#..#.", "#####", "...#.", "...#."],
    "5": ["#####", "#....", "####.", "....#", "....#", "#...#", ".###."],
    "6": ["..##.", ".#...", "#....", "####.", "#...#", "#...#", ".###."],
    "7": ["#####", "....#", "...#.", "..#..", ".#...", ".#...", ".#..."],
    "8": [".###.", "#...#", "#...#", ".###.", "#...#", "#...#", ".###."],
    "9": [".###.", "#...#", "#...#", ".####", "....#", "...#.", ".##.."],
    "A": [".###.", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"],
    "B": ["####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."],
    "C": [".###.", "#...#", "#....", "#....", "#....", "#...#", ".###."],
    "D": ["###..", "#..#.", "#...#", "#...#", "#...#", "#..#.", "###.."],
    "E": ["#####", "#....", "#....", "####.", "#....", "#....", "#####"],
    "F": ["#####", "#....", "#....", "####.", "#....", "#....", "#...."],
    "G": [".###.", "#...#", "#....", "#.###", "#...#", "#...#", ".####"],
    "H": ["#...#", "#...#", "#...#", "#####", "#...#", "#...#", "#...#"],
    "I": [".###.", "..#..", "..#..", "..#..", "..#..", "..#..", ".###."],
    "J": ["..###", "...#.", "...#.", "...#.", "...#.", "#..#.", ".##.."],
    "K": ["#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#"],
    "L": ["#....", "#....", "#....", "#....", "#....", "#....", "#####"],
    "M": ["#...#", "##.##", "#.#.#", "#.#.#", "#...#", "#...#", "#...#"],
    "N": ["#...#", "#...#", "##..#", "#.#.#", "#..##", "#...#", "#...#"],
    "O": [".###.", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."],
    "P": ["####.", "#...#", "#...#", "####.", "#....", "#....", "#...."],
    "Q": [".###.", "#...#", "#...#", "#...#", "#.#.#", "#..#.", ".##.#"],
    "R": ["####.", "#...#", "#...#", "####.", "#.#..", "#..#.", "#...#"],
    "S": [".####", "#....", "#....", ".###.", "....#", "....#", "####."],
    "T": ["#####", "..#..", "..#..", "..#..", "..#..", "..#..", "..#.."],
    "U": ["#...#", "#...#", "#...#", "#...#", "#...#", "#...#", ".###."],
    "V": ["#...#", "#...#", "#...#", "#...#", "#...#", ".#.#.", "..#.."],
    "W": ["#...#", "#...#", "#...#", "#.#.#", "#.#.#", "#.#.#", ".#.#."],
    "X": ["#...#", "#...#", ".#.#.", "..#..", ".#.#.", "#...#", "#...#"],
    "Y": ["#...#", "#...#", ".#.#.", "..#..", "..#..", "..#..", "..#.."],
    "Z": ["#####", "....#", "...#.", "..#..", ".#...", "#....", "#####"],
    "i": ["..#..", ".....", ".##..", "..#..", "..#..", "..#..", ".###."],
    "m": [".....", ".....", "##.#.", "#.#.#", "#.#.#", "#...#", "#...#"],
    "s": [".....", ".....", ".####", "#....", ".###.", "....#", "####."],
    ".": [".....", ".....", ".....", ".....", ".....", ".##..", ".##.."],
    "-": [".....", ".....", ".....", "#####", ".....", ".....", "....."],
    "+": [".....", "..#..", "..#..", "#####", "..#..", "..#..", "....."],
    "%": ["##...", "##..#", "...#.", "..#..", ".#...", "#..##", "...##"],
    "/": [".....", "....#", "...#.", "..#..", ".#...", "#....", "....."],
    "(": ["...#.", "..#..", ".#...", ".#...", ".#...", "..#..", "...#."],
    ")": [".#...", "..#..", "...#.", "...#.", "...#.", "..#..", ".#..."],
    ":": [".....", ".##..", ".##..", ".....", ".##..", ".##..", "....."],
}
CHARS = "".join(GLYPHS)


def margin(pitch):
    return math.ceil(2.6 * pitch)


def render_glyph(rows, pitch, ss=4):
    """RGBA image of one glyph: soft round dots with a glow around them."""
    m = margin(pitch)
    w, h = 5 * pitch + 2 * m, 7 * pitch + 2 * m
    W, H = w * ss, h * ss
    yy, xx = np.mgrid[0:H, 0:W].astype(np.float32) / ss
    dots = np.zeros((H, W), dtype=np.float32)
    shade = np.zeros((H, W), dtype=np.float32)
    r = 0.42 * pitch
    for j, row in enumerate(rows):
        for i, c in enumerate(row):
            if c != "#":
                continue
            cx, cy = m + (i + 0.5) * pitch, m + (j + 0.5) * pitch
            d = np.hypot(xx - cx, yy - cy)
            dots = np.maximum(dots, np.clip((r - d) * ss / 2 + 0.5, 0, 1))
            shade = np.maximum(shade, np.clip(1 - d / r, 0, 1))
    dots = np.asarray(Image.fromarray((dots * 255).astype(np.uint8)).resize((w, h), Image.LANCZOS),
                      dtype=np.float32) / 255
    shade = np.asarray(Image.fromarray((shade * 255).astype(np.uint8)).resize((w, h), Image.LANCZOS),
                       dtype=np.float32) / 255
    glow = np.clip(ndi.gaussian_filter(dots, 0.8 * pitch) * 1.4, 0, 1) * 0.6
    core = EDGE + (CORE - EDGE) * shade[..., None]
    alpha = dots + glow * (1 - dots)
    rgb = (core * dots[..., None] + GLOW * (glow * (1 - dots))[..., None]) / np.maximum(alpha, 1e-4)[..., None]
    return Image.fromarray((np.dstack([np.clip(rgb, 0, 1), alpha]) * 255).round().astype(np.uint8), "RGBA")


def build_atlas(pitch):
    m = margin(pitch)
    cw, ch = 5 * pitch + 2 * m, 7 * pitch + 2 * m
    rows = math.ceil(len(CHARS) / COLUMNS)
    atlas = Image.new("RGBA", (cw * COLUMNS, ch * rows), (0, 0, 0, 0))
    for k, c in enumerate(CHARS):
        atlas.paste(render_glyph(GLYPHS[c], pitch), ((k % COLUMNS) * cw, (k // COLUMNS) * ch))
    return atlas, {"file": f"vfd_p{pitch}.png", "pitch": pitch, "margin": m, "cell": [cw, ch],
                   "columns": COLUMNS, "advance": 6 * pitch, "height": 7 * pitch}


def text_width(text, pitch):
    return max(0, len(text) * 6 * pitch - pitch)


class VfdFont:
    """Draws text from the exported atlases; (x, y) is the top-left of the first dot grid."""

    def __init__(self, common_dir=ASSETS / "common"):
        self.meta = json.load(open(common_dir / "vfd_font.json"))
        self.atlases = {int(p): (Image.open(common_dir / a["file"]).convert("RGBA"), a)
                        for p, a in self.meta["atlases"].items()}

    def pitches(self):
        return sorted(self.atlases)

    def draw(self, base, text, x, y, pitch):
        atlas, a = self.atlases[pitch]
        cw, ch, m = a["cell"][0], a["cell"][1], a["margin"]
        for n, c in enumerate(text):
            k = self.meta["chars"].index(c)
            cell = atlas.crop(((k % a["columns"]) * cw, (k // a["columns"]) * ch,
                               (k % a["columns"] + 1) * cw, (k // a["columns"] + 1) * ch))
            px, py = round(x + n * a["advance"] - m), round(y - m)
            cx0, cy0 = max(0, -px), max(0, -py)
            cell = cell.crop((cx0, cy0, min(cw, base.width - px), min(ch, base.height - py)))
            base.alpha_composite(cell, (px + cx0, py + cy0))


PLACEHOLDER = "--"


def placeholder(binding):
    """A binding such as "EMU {emu_fps:.1f}" with every field shown as uncollected."""
    out, depth = "", 0
    for ch in binding:
        if ch == "{":
            depth += 1
            if depth == 1:
                out += PLACEHOLDER
        elif ch == "}":
            depth -= 1
        elif depth == 0:
            out += ch
    return out


def fit_pitch(width_at, pitch, available):
    """Largest pitch, from `pitch` down to 2, whose layout width fits `available`."""
    while pitch > 2 and width_at(pitch) > available:
        pitch -= 1
    return pitch


def draw_cell(base, font, cell, value, unit_text=None, lines=()):
    """Draw one VFD cell laid out by export_boards.py; texts are already formatted.

    `lines` holds the text of the cell's lines, in order.
    """
    v = cell["value"]
    x0, _, w, _ = cell["rect"]
    unit_text = v.get("unit", "") if unit_text is None else unit_text
    up = v.get("unit_pitch", v["pitch"])
    beside = bool(unit_text) and v.get("unit_position") == "right"

    def width(p):
        uw = text_width(unit_text, min(up, p)) if unit_text else 0
        return text_width(value, p) + (3 * min(up, p) + uw if beside else 0)

    p = fit_pitch(width, v["pitch"], v["right"] - v.get("left_limit", x0 + 4))
    u = min(up, p)
    bottom = v["top"] + 7 * v["pitch"]
    if beside:
        ux = v["right"] - text_width(unit_text, u)
        font.draw(base, unit_text, ux, bottom - 7 * u, u)
        font.draw(base, value, ux - 3 * u - text_width(value, p), bottom - 7 * p, p)
    else:
        font.draw(base, value, v["right"] - text_width(value, p), bottom - 7 * p, p)
        if unit_text:
            font.draw(base, unit_text, v["right"] - text_width(unit_text, u), bottom + 2 * u, u)
    for line, text in zip(cell.get("lines", []), lines):
        if not text:
            continue
        right = line["align"] == "right"
        available = line["x"] - (x0 + 4) if right else x0 + w - 4 - line["x"]
        lp = fit_pitch(lambda q: text_width(text, q), line["pitch"], available)
        x = line["x"] - text_width(text, lp) if right else line["x"]
        font.draw(base, text, x, line["top"] + 7 * (line["pitch"] - lp), lp)


def main():
    out = ASSETS / "common"
    out.mkdir(parents=True, exist_ok=True)
    meta = {"version": 1, "dots": [5, 7], "advance_dots": 6, "chars": CHARS, "glyphs": GLYPHS, "atlases": {}}
    for p in PITCHES:
        atlas, info = build_atlas(p)
        atlas.save(out / info["file"], optimize=True)
        meta["atlases"][str(p)] = info
    json.dump(meta, open(out / "vfd_font.json", "w"), indent=1)


if __name__ == "__main__":
    main()

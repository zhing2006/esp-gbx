"""Shared helpers for the UI asset tools: paths, shape masks, plates and compositing."""

import json
import math
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter
from scipy import ndimage as ndi

ROOT = Path(__file__).resolve().parents[2]
RAWS = ROOT / "raws" / "ui"
ASSETS = ROOT / "assets" / "ui"
DOCS = ROOT / "docs" / "ui-layouts"

# Raws and plates are painted at twice the Tab5 logical resolution.
MASTER_SCALE = 2
SS = 4  # supersampling for anti-aliased masks


def boards():
    """Board names, one per raws/ui/<board>/spec.json."""
    return sorted(p.parent.name for p in RAWS.glob("*/spec.json"))


def load_spec(board):
    return json.load(open(RAWS / board / "spec.json"))


def rounded_rect_mask(size, rect, radii):
    """Anti-aliased mask of `rect` (x, y, w, h) with per-corner radii (tl, tr, br, bl)."""
    w, h = size
    m = Image.new("L", (w * SS, h * SS), 0)
    d = ImageDraw.Draw(m)
    x, y, rw, rh = [v * SS for v in rect]
    d.rectangle((x, y, x + rw - 1, y + rh - 1), fill=255)
    corners = ((radii[0], (x, y), 180, (0, 0)), (radii[1], (x + rw, y), 270, (-1, 0)),
               (radii[2], (x + rw, y + rh), 0, (-1, -1)), (radii[3], (x, y + rh), 90, (0, -1)))
    for r, (px, py), start, (sx, sy) in corners:
        r *= SS
        if r <= 0:
            continue
        qx, qy = px + sx * r, py + sy * r
        d.rectangle((qx, qy, qx + r - 1, qy + r - 1), fill=0)
        cx, cy = qx + (r if sx == 0 else 0), qy + (r if sy == 0 else 0)
        d.pieslice((cx - r, cy - r, cx + r, cy + r), start, start + 90, fill=255)
    return m.resize(size, Image.LANCZOS)


def draw_capsule(draw, center, length, thick, angle, fill):
    """Rotated capsule; `length` includes the round ends."""
    cx, cy = center
    t = thick / 2
    hl = length / 2 - t
    a = math.radians(angle)
    p0 = (cx - math.cos(a) * hl, cy - math.sin(a) * hl)
    p1 = (cx + math.cos(a) * hl, cy + math.sin(a) * hl)
    draw.line((p0, p1), fill=fill, width=max(1, round(2 * t)))
    for px, py in (p0, p1):
        draw.ellipse((px - t, py - t, px + t, py + t), fill=fill)


def region_mask(size, regions):
    """Union of feathered regions (rect, ellipse or capsule), as float 0..1."""
    total = np.zeros((size[1], size[0]), dtype=np.float32)
    for r in regions:
        m = Image.new("L", size, 0)
        d = ImageDraw.Draw(m)
        f = r.get("feather", 8)
        if "rect" in r:
            x0, y0, x1, y1 = r["rect"]
            d.rectangle((x0 + f, y0 + f, x1 - f, y1 - f), fill=255)
        elif "ellipse" in r:
            cx, cy, rx, ry = r["ellipse"]
            d.ellipse((cx - rx + f, cy - ry + f, cx + rx - f, cy + ry - f), fill=255)
        elif "capsule" in r:
            cx, cy, length, thick, ang = r["capsule"]
            draw_capsule(d, (cx, cy), length - 2 * f, thick - 2 * f, ang, 255)
        if f > 0:
            m = m.filter(ImageFilter.GaussianBlur(f / 2))
        total = np.maximum(total, np.asarray(m, dtype=np.float32) / 255)
    return total


def blend_regions(base, edit, regions):
    """Take only `regions` of an AI edit and feather them into `base`."""
    a = np.asarray(base.convert("RGB"), dtype=np.float32)
    b = np.asarray(edit.convert("RGB"), dtype=np.float32)
    m = region_mask(base.size, regions)[..., None]
    return Image.fromarray(np.round(a * (1 - m) + b * m).astype(np.uint8))


def soften_ghosts(region, keep=0.35, coarse=8.0, fine=1.2):
    """Fade mid-frequency detail such as unlit VFD segment ghosts, keeping the fine grain."""
    blur = lambda a, s: ndi.gaussian_filter(a, (s, s, 0))
    base = blur(region, coarse)
    grain = region - blur(region, fine)
    return base + keep * (region - base - grain) + grain


def build_plate(board, spec=None):
    """The board's background at 2x, built from its raws by the recipe in spec["plate"]."""
    spec = spec or load_spec(board)
    d = RAWS / board
    recipe = spec["plate"]
    im = Image.open(d / recipe["base"]).convert("RGB")
    for step in recipe["blend"]:
        im = blend_regions(im, Image.open(d / step["edit"]), json.load(open(d / step["regions"])))
    a = np.asarray(im, dtype=np.float32).copy()
    S = MASTER_SCALE
    if recipe.get("soften_vfd"):
        label_h = 26  # logical px kept as painted at the top of each cell, where the label is
        for cx, cy, cw, ch in spec["vfd"]["cells"].values():
            x0, y0 = round((cx + 4) * S), round((cy + label_h) * S)
            x1, y1 = round((cx + cw - 4) * S), round((cy + ch - 4) * S)
            region = a[y0:y1, x0:x1]
            m = np.zeros(region.shape[:2], dtype=np.float32)
            m[6:-6, 6:-6] = 1
            m = ndi.gaussian_filter(m, 3)[..., None]
            a[y0:y1, x0:x1] = region * (1 - m) + soften_ghosts(region) * m
    x, y, w, h = spec["display"]
    cut_screen_opening(a, (x * S, y * S, (x + w) * S, (y + h) * S), recipe.get("display_border", 0) * S)
    return Image.fromarray(np.clip(a, 0, 255).round().astype(np.uint8))


PANEL = (24, 25, 28)        # unlit LCD border between the bezel and the game window
BEZEL_LIGHT = (150, 155, 170)
LIGHT_DIR = (-0.7071, -0.7071)  # towards the light, from the top-left


def cut_screen_opening(a, display, border):
    """Open the bezel `border` master px beyond the display, showing the LCD's unlit border.

    The bezel walls facing the light catch a highlight and the others fall in
    shadow, and the bezel shades the panel along its top and left edges, so the
    panel reads as sitting behind the lens. The display itself stays black.
    """
    x0, y0, x1, y1 = display
    if border > 0:
        ox0, oy0, ox1, oy1 = x0 - border, y0 - border, x1 + border, y1 + border
        lip = 2
        ys, xs = np.mgrid[oy0 - lip:oy1 + lip, ox0 - lip:ox1 + lip]
        # Chebyshev distance outside the opening, and the outward normal of the nearest side.
        out = np.stack([ox0 - xs, xs - (ox1 - 1), oy0 - ys, ys - (oy1 - 1)])
        d = out.max(0)
        side = out.argmax(0)
        normal = np.array([(-1, 0), (1, 0), (0, -1), (0, 1)], dtype=np.float32)[side]
        # The wall on each side faces back into the opening.
        lit = -(normal[..., 0] * LIGHT_DIR[0] + normal[..., 1] * LIGHT_DIR[1])
        ring = a[oy0 - lip:oy1 + lip, ox0 - lip:ox1 + lip]
        for dist, gain, shade in ((1, 0.45, 0.35), (2, 0.2, 0.15)):
            m = d == dist
            hi = (m & (lit > 0))[..., None] * (gain * lit[..., None])
            lo = (m & (lit < 0))[..., None] * (shade * -lit[..., None])
            ring[:] = ring + hi * (np.array(BEZEL_LIGHT) - ring) - lo * ring
        panel = np.ones((oy1 - oy0, ox1 - ox0, 1), dtype=np.float32) * np.array(PANEL, dtype=np.float32)
        ty = np.arange(oy1 - oy0, dtype=np.float32)[:, None]
        tx = np.arange(ox1 - ox0, dtype=np.float32)[None, :]
        shadow = 0.4 * np.maximum(np.exp(-ty / 1.5), np.exp(-tx / 1.5))
        a[oy0:oy1, ox0:ox1] = panel * (1 - shadow[..., None])
    a[y0:y1, x0:x1] = 0


def resample(sprite, center, anchor, scale):
    """Scale `sprite` (master px) so its anchor lands on `center` (board px).

    Returns the image and its whole-pixel top-left corner. The sprite is
    resampled at the fractional position instead of being moved to a whole
    pixel, so a center such as 31.0 at scale 0.4 does not shift by a third of
    a pixel.
    """
    fx, fy = center[0] - anchor[0] * scale, center[1] - anchor[1] * scale
    x, y = math.floor(fx), math.floor(fy)
    w = math.ceil(fx + sprite.width * scale) - x
    h = math.ceil(fy + sprite.height * scale) - y
    p = 3 * math.ceil(1 / scale)  # transparent border wider than the Lanczos support
    src = Image.new("RGBa", (sprite.width + 2 * p, sprite.height + 2 * p))
    src.paste(sprite.convert("RGBa"), (p, p))
    box = (p + (x - fx) / scale, p + (y - fy) / scale, p + (x + w - fx) / scale, p + (y + h - fy) / scale)
    return src.resize((w, h), Image.LANCZOS, box=box).convert("RGBA"), x, y


def place(base, sprite, center, anchor, scale):
    """Alpha-composite `sprite` (master px) so its anchor lands on `center` (board px)."""
    sprite, x, y = resample(sprite, center, anchor, scale)
    cx0, cy0 = max(0, -x), max(0, -y)
    sprite = sprite.crop((cx0, cy0, min(sprite.width, base.width - x), min(sprite.height, base.height - y)))
    base.alpha_composite(sprite, (x + cx0, y + cy0))

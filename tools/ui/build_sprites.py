# /// script
# requires-python = ">=3.12"
# dependencies = ["pillow==12.2.0", "numpy==2.5.4", "scipy==1.18.1"]
# ///
"""Build the shared control sprites and their states from raws/ui/common/concept.png.

Shapes are geometry fitted to the concept's controls, colors are cut from the
concept, and shadows, pressed states and lit lamps are drawn here, so every
board gets the same controls. Sizes are master px, twice the Tab5 logical size.
Writes assets/ui/common/*.png and sprites.json.
"""

import json
import math

import numpy as np
from PIL import Image, ImageDraw
from scipy import ndimage as ndi

from uikit import ASSETS, RAWS, SS

MARGIN = 28  # room for shadows around the footprint

# Fitted geometry in concept pixels.
SHAPES = {
    "dpad": {"kind": "cross", "center": (355.0, 2059.5), "size": 440, "arm": 150, "radius": 16},
    "button_round": {"kind": "circle", "center": (1180.0, 1982.0), "radius": 114},
    "button_pill": {"kind": "capsule", "center": (601.0, 2411.0), "length": 183, "thick": 50, "angle": -25.6},
    "shoulder_l": {"kind": "bar", "box": (0, 1595, 464, 1698), "round": "right"},
    "shoulder_r": {"kind": "bar", "box": (976, 1595, 1440, 1698), "round": "left"},
    "menu": {"kind": "capsule", "center": (1275.0, 68.0), "length": 204, "thick": 88, "angle": 0.0},
    "knob": {"kind": "rrect", "center": (142.0, 71.5), "w": 121, "h": 45, "radius": 18},
}

DPAD_DIRECTIONS = {"up": (0, -1), "down": (0, 1), "left": (-1, 0), "right": (1, 0),
                   "up_left": (-1, -1), "up_right": (1, -1), "down_left": (-1, 1), "down_right": (1, 1)}

LAMP_COLORS = {
    "green": (90, 240, 120),
    "lime": (170, 235, 70),
    "berry": (255, 70, 120),
    "violet": (160, 130, 255),
    "amber": (255, 175, 50),
    "red": (255, 60, 45),
}

# Color of each indicator when lit. SAVE is amber while modifications are
# pending or being written and red after a failed write.
LAMP_USE = {"gb": ["lime"], "gbc": ["berry"], "gba": ["violet"], "int": ["green"], "jit": ["green"],
            "save": ["amber", "red"], "err": ["red"]}


def shape_mask(spec, origin, size):
    """Anti-aliased mask of a fitted shape on a canvas at `origin` with `size`."""
    w, h = size
    m = Image.new("L", (w * SS, h * SS), 0)
    d = ImageDraw.Draw(m)
    ox, oy = origin

    def P(x, y):
        return ((x - ox) * SS, (y - oy) * SS)

    k = spec["kind"]
    if k == "circle":
        cx, cy = spec["center"]
        r = spec["radius"]
        d.ellipse((*P(cx - r, cy - r), *P(cx + r, cy + r)), fill=255)
    elif k == "cross":
        cx, cy = spec["center"]
        s, a, r = spec["size"] / 2, spec["arm"] / 2, spec["radius"] * SS
        d.rounded_rectangle((*P(cx - s, cy - a), *P(cx + s, cy + a)), r, fill=255)
        d.rounded_rectangle((*P(cx - a, cy - s), *P(cx + a, cy + s)), r, fill=255)
    elif k == "capsule":
        cx, cy = spec["center"]
        L, t, ang = spec["length"], spec["thick"], math.radians(spec["angle"])
        ux, uy = math.cos(ang), math.sin(ang)
        hl = (L - t) / 2
        p0, p1 = P(cx - ux * hl, cy - uy * hl), P(cx + ux * hl, cy + uy * hl)
        d.line((p0, p1), fill=255, width=round(t * SS))
        for px, py in (p0, p1):
            d.ellipse((px - t * SS / 2, py - t * SS / 2, px + t * SS / 2, py + t * SS / 2), fill=255)
    elif k == "rrect":
        cx, cy = spec["center"]
        d.rounded_rectangle((*P(cx - spec["w"] / 2, cy - spec["h"] / 2),
                             *P(cx + spec["w"] / 2, cy + spec["h"] / 2)), spec["radius"] * SS, fill=255)
    elif k == "bar":
        # The outer end continues past the screen edge, so only the inner end is round.
        x0, y0, x1, y1 = spec["box"]
        r = (y1 - y0) / 2 * SS
        if spec["round"] == "right":
            d.rounded_rectangle((*P(x0 - 200, y0), *P(x1, y1)), r, fill=255)
        else:
            d.rounded_rectangle((*P(x0, y0), *P(x1 + 200, y1)), r, fill=255)
    return np.asarray(m.resize((w, h), Image.LANCZOS), dtype=np.float32) / 255


def footprint_box(spec):
    k = spec["kind"]
    if k == "bar":
        return spec["box"]
    cx, cy = spec["center"]
    if k == "circle":
        e = spec["radius"]
        return (cx - e, cy - e, cx + e, cy + e)
    if k == "cross":
        e = spec["size"] / 2
        return (cx - e, cy - e, cx + e, cy + e)
    if k == "capsule":
        ang = math.radians(spec["angle"])
        ex = abs(math.cos(ang)) * (spec["length"] - spec["thick"]) / 2 + spec["thick"] / 2
        ey = abs(math.sin(ang)) * (spec["length"] - spec["thick"]) / 2 + spec["thick"] / 2
        return (cx - ex, cy - ey, cx + ex, cy + ey)
    return (cx - spec["w"] / 2, cy - spec["h"] / 2, cx + spec["w"] / 2, cy + spec["h"] / 2)


def shift(a, dx, dy):
    return ndi.shift(a, (dy, dx) + ((0,) if a.ndim == 3 else ()), order=1, mode="nearest")


def blur(a, sigma):
    return ndi.gaussian_filter(a, sigma)


def shadow_alpha(mask, pressed):
    """Drop shadow toward the lower right, away from the key light, plus a tight contact shadow."""
    if pressed:
        drop = shift(blur(mask, 3.5), 2, 3) * 0.42
    else:
        drop = shift(blur(mask, 6.0), 4, 7) * 0.55
    contact = blur(ndi.grey_dilation(mask, size=(3, 3)), 1.6) * 0.55
    return np.clip(np.maximum(drop, contact), 0, 1)


def compose(rgb, mask, sh):
    """Footprint over a black shadow, as straight-alpha RGBA."""
    a = mask + sh * (1 - mask)
    col = np.where(a[..., None] > 1e-4, rgb * mask[..., None] / np.maximum(a, 1e-4)[..., None], 0)
    rgba = np.dstack([np.clip(col, 0, 1), np.clip(a, 0, 1)]) * 255
    return Image.fromarray(rgba.round().astype(np.uint8), "RGBA")


def pressed_surface(rgb, mask, light=(5, 8), dark=0.70, flatten=0.55):
    """Sunken look: content moves down, darker, flatter highlights, inner shadow on the lit side."""
    out = shift(rgb, 0, 3)
    lum = out.mean(-1, keepdims=True)
    out = out - flatten * np.clip(lum - 0.45, 0, None)
    out = out * dark
    inner = blur(np.clip(mask - shift(mask, *light), 0, 1), 3.5)
    return np.clip(out * (1 - 0.75 * inner[..., None]), 0, 1)


def dpad_tilt(rgb, mask, center, half, direction):
    """Pressed d-pad: the pressed side sinks and darkens, the opposite side catches more light."""
    h, w = mask.shape
    yy, xx = np.mgrid[0:h, 0:w].astype(np.float32)
    dx, dy = direction
    n = math.hypot(dx, dy)
    t = ((xx - center[0]) * dx / n + (yy - center[1]) * dy / n) / half
    sink = np.clip(t, 0, 1)
    rise = np.clip(-t, 0, 1)
    lum = rgb.mean(-1, keepdims=True)
    out = rgb * (1 - 0.55 * sink[..., None]) - 0.6 * np.clip(lum - 0.12, 0, None) * sink[..., None]
    edge = blur(np.clip(mask - ndi.grey_erosion(mask, size=(9, 9)), 0, 1), 2.0)
    out = out + (0.10 * rise + 0.22 * edge * rise)[..., None] * np.array([0.92, 0.94, 1.0])
    out = out * (1 - 0.65 * (edge * np.clip(t * 1.4, 0, 1))[..., None])
    return np.clip(out, 0, 1)


def lamp_sprite(color, dome_r=14, size=160):
    """Lit lamp: an opaque glowing dome plus a soft glow that spills onto the panel."""
    c = (size - 1) / 2
    yy, xx = np.mgrid[0:size, 0:size].astype(np.float32)
    r = np.hypot(xx - c, yy - c)
    col = np.array(color, dtype=np.float32) / 255
    glow = 0.80 * np.exp(-((r / (dome_r * 1.25)) ** 2)) + 0.32 * np.exp(-((r / (dome_r * 3.4)) ** 2))
    window = np.clip(1 - (r / (c * 0.98)) ** 2, 0, 1) ** 2  # reach zero inside the canvas
    glow = np.clip(glow * window, 0, 1)
    dome = np.clip(dome_r + 0.5 - r, 0, 1)
    k = np.clip(r / dome_r, 0, 1)[..., None]
    hot = col * 0.55 + 0.45
    dome_col = hot * (1 - k ** 2) + col * k ** 2
    hx, hy = c - dome_r * 0.35, c - dome_r * 0.40
    spec = np.clip(1 - np.hypot(xx - hx, yy - hy) / (dome_r * 0.32), 0, 1) ** 1.5
    dome_col = dome_col + (1 - dome_col) * 0.85 * spec[..., None]
    alpha = dome + glow * (1 - dome)
    rgb = (dome_col * dome[..., None] + col * (glow * (1 - dome))[..., None]) / np.maximum(alpha, 1e-4)[..., None]
    return Image.fromarray((np.dstack([np.clip(rgb, 0, 1), alpha]) * 255).round().astype(np.uint8), "RGBA")


def main():
    out = ASSETS / "common"
    out.mkdir(parents=True, exist_ok=True)
    img = np.asarray(Image.open(RAWS / "common" / "concept.png").convert("RGB"), dtype=np.float32) / 255
    H, W = img.shape[:2]
    sprites = {}
    for name, spec in SHAPES.items():
        x0, y0, x1, y1 = footprint_box(spec)
        ox, oy = math.floor(x0) - MARGIN, math.floor(y0) - MARGIN
        cw, ch = math.ceil(x1) + MARGIN - ox, math.ceil(y1) + MARGIN - oy
        mask = shape_mask(spec, (ox, oy), (cw, ch))
        sx0, sy0 = max(ox, 0), max(oy, 0)
        sx1, sy1 = min(ox + cw, W), min(oy + ch, H)
        rgb = np.zeros((ch, cw, 3), dtype=np.float32)
        rgb[sy0 - oy:sy1 - oy, sx0 - ox:sx1 - ox] = img[sy0:sy1, sx0:sx1]
        valid = np.zeros((ch, cw), dtype=np.float32)
        valid[sy0 - oy:sy1 - oy, sx0 - ox:sx1 - ox] = 1
        # Shoulder bars wrap the screen edge; the canvas extends past it with zero alpha.
        mask = mask * valid

        states = {"normal": f"{name}.png"}
        compose(rgb, mask, shadow_alpha(mask, False)).save(out / states["normal"])
        if name == "dpad":
            c = (spec["center"][0] - ox, spec["center"][1] - oy)
            for dn, dv in DPAD_DIRECTIONS.items():
                states[dn] = f"{name}_{dn}.png"
                compose(dpad_tilt(rgb, mask, c, spec["size"] / 2, dv), mask,
                        shadow_alpha(mask, False)).save(out / states[dn])
        elif name != "knob":
            # Light grey parts read as much darker than the maroon buttons for the same change.
            dark, flatten = (0.70, 0.55) if name == "button_round" else (0.82, 0.30)
            states["pressed"] = f"{name}_pressed.png"
            compose(pressed_surface(rgb, mask, dark=dark, flatten=flatten), mask,
                    shadow_alpha(mask, True)).save(out / states["pressed"])
        sprites[name] = {"size": [cw, ch],
                         "anchor": [round((x0 + x1) / 2 - ox, 2), round((y0 + y1) / 2 - oy, 2)],
                         "states": states}

    for cname, col in LAMP_COLORS.items():
        lamp_sprite(col).save(out / f"lamp_{cname}_on.png")
    manifest = {
        "version": 1,
        "master_scale": "2x of the M5Stack Tab5 logical size",
        "sprites": sprites,
        "lamps": {"size": [160, 160], "anchor": [79.5, 79.5], "dome_radius": 14,
                  "colors": {k: f"lamp_{k}_on.png" for k in LAMP_COLORS}, "use": LAMP_USE},
    }
    json.dump(manifest, open(out / "sprites.json", "w"), indent=2)


if __name__ == "__main__":
    main()

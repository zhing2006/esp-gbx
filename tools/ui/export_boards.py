# /// script
# requires-python = ">=3.12"
# dependencies = ["pillow==12.2.0", "numpy==2.5.4", "scipy==1.18.1"]
# ///
"""Build each board's background and board.json from raws/ui/<board>/spec.json.

The background is the 2x plate from the spec's recipe, reduced to the board's
size. Lamp domes, the speed switch slot and the printed VFD labels are measured
on the plate, so sprites and text land on what was painted.
Run build_sprites.py and vfd_font.py first; board.json refers to their output.
"""

import argparse
import json

import numpy as np
from PIL import Image
from scipy import ndimage as ndi

from uikit import ASSETS, MASTER_SCALE, boards, build_plate, load_spec

S = MASTER_SCALE
SPRITE_OF = {"dpad": "dpad", "a": "button_round", "b": "button_round", "select": "button_pill",
             "start": "button_pill", "l": "shoulder_l", "r": "shoulder_r", "menu": "menu"}
KNOB_INSET = 10      # master px between the knob and the slot ends
SPRITE_MARGIN = 28   # shadow margin around sprite footprints, see build_sprites.py


def lamp_centers(plate, approx, search=14):
    """Refine approximate lamp centers to the centroid of the tinted dome."""
    a = np.asarray(plate, dtype=np.int16)
    sat = a.max(-1) - a.min(-1)
    out = {}
    for name, (x, y) in approx.items():
        cx, cy, r = round(x * S), round(y * S), search * S
        win = ndi.binary_opening(sat[cy - r:cy + r, cx - r:cx + r] > 20, iterations=1)
        lab, n = ndi.label(win)
        best = None
        for i in range(1, n + 1):
            ys, xs = np.nonzero(lab == i)
            d = np.hypot(xs.mean() - r, ys.mean() - r)
            if len(ys) > 20 and (best is None or d < best[0]):
                best = (d, xs.mean(), ys.mean())
        if best is None:
            raise SystemExit(f"lamp {name}: no dome near {x}, {y}")
        out[name] = [round((cx - r + best[1]) / S, 2), round((cy - r + best[2]) / S, 2)]
    return out


def slot_extent(plate, approx, scale):
    """Measure the slot opening around the approximate rect.

    Horizontally it is the dark run through the slot centre, because the
    printed text beside the slot is dark too. Vertically it lies between the
    steepest light-to-dark and dark-to-light edges, because the harmonized
    plates have a lighter sheen in the lower half of the slot. The search
    reaches 10 px around the rect, less where the controls are scaled below
    0.6, as the groove and the lens below the slot come closer.
    """
    a = np.asarray(plate.convert("L"), dtype=np.int16)
    x, y, w, h = [round(v * S) for v in approx]
    pad = round(10 * S * min(1, scale / 0.6))
    left = max(0, x - pad)
    row = a[y + h // 2, left:x + w + pad] < 75
    lab, _ = ndi.label(ndi.binary_closing(row, iterations=2))
    run = np.nonzero(lab == lab[x + w // 2 - left])[0]
    top = max(0, y - pad)
    profile = a[top:y + h + pad, x + w // 4:x + 3 * w // 4].mean(1)
    step = np.diff(profile)
    mid = (y + h // 2) - top
    y0 = top + int(np.argmin(step[:mid])) + 1
    y1 = top + mid + int(np.argmax(step[mid:])) + 1
    x0, x1 = (left + run.min()) / S, (left + run.max() + 1) / S
    return [round(x0, 2), round(y0 / S, 2), round(x1 - x0, 2), round((y1 - y0) / S, 2)]


def label_box(plate, rect):
    """Bounding box of the glowing cyan label in the top part of a VFD cell."""
    a = np.asarray(plate, dtype=np.int16)
    x, y, w, h = [round(v * S) for v in rect]
    top = a[y:y + h // 2, x:x + w]
    m = (top[..., 1] > 150) & (top[..., 2] > 150) & (top[..., 1] - top[..., 0] > 40)
    m = ndi.binary_opening(m, iterations=1)
    ys, xs = np.nonzero(m)
    if len(ys) == 0:
        raise SystemExit(f"no VFD label in cell {rect}")
    return [round((x + xs.min()) / S, 1), round((y + ys.min()) / S, 1),
            round((xs.max() - xs.min() + 1) / S, 1), round((ys.max() - ys.min() + 1) / S, 1)]


def place_line(line, v, rect, label, value, pad, s):
    """Position of a line of small text: on the label's row, or on a numbered row.

    Rows count down from under the unit when the unit sits below the value,
    otherwise up from the bottom of the cell. Left-aligned text starts at the
    label's left edge, right-aligned text ends where the value does.
    """
    x, y, w, h = rect
    lx, ly, lw, lh = label
    lp = v["line_pitch"]
    row = line.get("row", 0)
    if row == "label":
        top = ly + lh / 2 - 7 * lp / 2
    elif v["unit_position"] == "below" and v["value_row"] != "label":
        unit_bottom = value["top"] + 7 * v["value_pitch"] + 2 * v["unit_pitch"] + 7 * v["unit_pitch"]
        top = unit_bottom + 5 * lp + row * 9 * lp
    else:
        top = y + h - round(6 * s + 2) - 7 * lp - row * 9 * lp
    align = line.get("align", "left")
    return {"text": line["text"], "pitch": lp, "align": align,
            "x": value["right"] if align == "right" else lx, "top": round(top, 1)}


def vfd_layout(plate, spec):
    v = spec["vfd"]
    s = spec["control_scale"]
    pad = round(10 * s + 2)
    out = {}
    for cid, rect in v["cells"].items():
        x, y, w, h = rect
        lx, ly, lw, lh = label_box(plate, rect)
        fmt = v["formats"][cid]
        P = v["value_pitch"]
        value = {"binding": fmt["value"], "pitch": P, "right": x + w - pad}
        if fmt.get("unit"):
            value.update(unit=fmt["unit"], unit_pitch=v["unit_pitch"], unit_position=v["unit_position"])
        if v["value_row"] == "label":
            value.update(top=round(ly + lh / 2 - 7 * P / 2, 1), left_limit=round(lx + lw + 3 * P, 1))
        else:
            value.update(top=round(ly + lh + 2 * P, 1), left_limit=x + 4)
        cell = {"rect": rect, "label": [lx, ly, lw, lh], "value": value}
        lines = fmt.get("lines", [])
        if lines:
            cell["lines"] = [place_line(line, v, rect, (lx, ly, lw, lh), value, pad, s) for line in lines]
        out[cid] = cell
    return out


def export(board):
    spec = load_spec(board)
    plate = build_plate(board, spec)
    W, H = spec["size"]
    d = ASSETS / board
    d.mkdir(parents=True, exist_ok=True)
    plate.resize((W, H), Image.LANCZOS).save(d / "background.png", optimize=True)

    sprites = json.load(open(ASSETS / "common" / "sprites.json"))
    scale = 0.5 * spec["control_scale"]
    knob_w = (sprites["sprites"]["knob"]["size"][0] - 2 * SPRITE_MARGIN) * scale
    sx, sy, sw, sh = slot_extent(plate, spec["slot"], spec["control_scale"])
    inset = KNOB_INSET * scale
    cy = round(sy + sh / 2, 2)
    manifest = {
        "version": 1,
        "board": board,
        "size": [W, H],
        "background": "background.png",
        "sprites": "../common/sprites.json",
        "font": "../common/vfd_font.json",
        "sprite_scale": round(scale, 4),
        "display": spec["display"],
        "controls": {cid: {"sprite": SPRITE_OF[cid], "center": c} for cid, c in spec["controls"].items()},
        "speed_switch": {"slot": [sx, sy, sw, sh],
                         "1x": [round(sx + inset + knob_w / 2, 2), cy],
                         "2x": [round(sx + sw - inset - knob_w / 2, 2), cy]},
        "lamps": {k: {"center": c} for k, c in lamp_centers(plate, spec["lamps"]).items()},
    }
    # A board without a VFD leaves "vfd" out of its spec.
    if "vfd" in spec:
        manifest["vfd"] = vfd_layout(plate, spec)
    json.dump(manifest, open(d / "board.json", "w"), indent=2)


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--board", choices=boards(), help="export one board only")
    args = p.parse_args()
    for board in [args.board] if args.board else boards():
        export(board)
        print(f"{board}: exported")


if __name__ == "__main__":
    main()

# /// script
# requires-python = ">=3.12"
# dependencies = ["pillow==12.2.0", "numpy==2.5.4", "scipy==1.18.1"]
# ///
"""Paste-up for a new board: flat shell colors plus finished parts cut from the Tab5 plate.

Writes raws/ui/<board>/pasteup.png at 2x. It is the only input image of one AI
pass that blends it into a finished plate (see raws/ui/common/harmonize_prompt.txt);
every element already sits at its final position and size, which keeps the AI
from moving them. The layout comes from the "pasteup" section of the board's
spec.json, the serial-number sticker, if the layout has one, from
raws/ui/<board>/sticker.png.
"""

import argparse
import math
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

from uikit import MASTER_SCALE, RAWS, boards, build_plate, draw_capsule, load_spec, rounded_rect_mask

S = MASTER_SCALE

# Finished parts on the 2x Tab5 plate.
LAMP_SRC = {"gb": (161, 379), "gbc": (161, 501), "gba": (161, 618), "int": (1280, 364),
            "jit": (1280, 483), "save": (1280, 601), "err": (1280, 720)}
VFD_LABELS = {"speed": (160, 1204, 310, 1258), "frame": (772, 1204, 916, 1258),
              "audio": (160, 1374, 300, 1428), "memory": (772, 1374, 872, 1428)}
PARTS = {
    "logo": (360, 1020, 1080, 1132),
    "print_text": (510, 178, 930, 214),
    "slot_group": (56, 2, 500, 118),
    "label_a": (1262, 2104, 1312, 2166),
    "label_b": (1056, 2270, 1110, 2334),
    "label_select": (536, 2466, 672, 2512),
    "label_start": (796, 2466, 910, 2512),
}
TAB5_SLOT_CENTER = (220, 72)
DISH_SRC = ((352, 2060), 262)
CAPSULE_SRC = ((1058, 2084), 640, 330, -40)
# Tab5 offsets, in logical px, from a button centre to its printed label.
LABEL_OFFSETS = {"a": (54.0, 77.0), "b": (74.0, 57.0), "select": (1.5, 39.5), "start": (1.5, 39.5)}


def sample(P, box):
    return tuple(int(v) for v in np.asarray(P.crop(box)).reshape(-1, 3).mean(0))


def feather_rect(size, f):
    m = Image.new("L", size, 0)
    ImageDraw.Draw(m).rectangle((f, f, size[0] - 1 - f, size[1] - 1 - f), fill=255)
    return m.filter(ImageFilter.GaussianBlur(f / 2)) if f else m


def paste_part(canvas, part, center, scale, feather=6, mask=None):
    nw, nh = max(1, round(part.width * scale)), max(1, round(part.height * scale))
    part = part.resize((nw, nh), Image.LANCZOS)
    m = mask.resize((nw, nh), Image.LANCZOS) if mask is not None else feather_rect((nw, nh), feather)
    canvas.paste(part, (round(center[0] - nw / 2), round(center[1] - nh / 2)), m)


def fill(canvas, rect, radii, color):
    mask = rounded_rect_mask(canvas.size, [round(v * S) for v in rect], [round(v * S) for v in radii])
    canvas.paste(Image.new("RGB", canvas.size, color), (0, 0), mask)


def build(board):
    spec = load_spec(board)
    pu = spec["pasteup"]
    P = build_plate("m5stack_tab5")
    W, H = spec["size"]
    s = spec["control_scale"]
    grey = sample(P, (800, 20, 1100, 100))
    indigo = sample(P, (1150, 1640, 1400, 1700))
    slate = sample(P, (96, 760, 140, 900))
    vfd = sample(P, (560, 1290, 700, 1340))
    im = Image.new("RGB", (W * S, H * S), "#1E1E20")
    fill(im, (0, 0, W, H), pu["shell_radii"], indigo)
    fill(im, (0, 0, W, pu["seam_y"]), (pu["shell_radii"][0], pu["shell_radii"][1], 0, 0), grey)
    d = ImageDraw.Draw(im)
    d.line([v * S for v in (10, pu["groove_y"], W - 10, pu["groove_y"])], fill="#9C968A", width=2 * S)

    # Speed switch group, scaled around the slot centre.
    sx, sy, sw, sh = spec["slot"]
    src = P.crop(PARTS["slot_group"])
    ox = TAB5_SLOT_CENTER[0] - PARTS["slot_group"][0] - src.width / 2
    oy = TAB5_SLOT_CENTER[1] - PARTS["slot_group"][1] - src.height / 2
    paste_part(im, src, ((sx + sw / 2) * S - ox * s, (sy + sh / 2) * S - oy * s), s)

    # Lens, print lines and text, display, lamps.
    fill(im, pu["lens"], pu["lens_radii"], slate)
    lx0, ly, lx1, g0, g1 = pu["print_lines"]
    for i, c in enumerate(((128, 160, 70), (176, 52, 104), (110, 98, 200))):
        yy = (ly + i * 3 * min(1, s * 1.25)) * S
        d.line((lx0 * S, yy, g0 * S, yy), fill=c, width=S)
        d.line((g1 * S, yy, lx1 * S, yy), fill=c, width=S)
    paste_part(im, P.crop(PARTS["print_text"]), ((g0 + g1) / 2 * S, (ly + 3) * S), s)
    x, y, w, h = spec["display"]
    d.rectangle((x * S, y * S, (x + w) * S - 1, (y + h) * S - 1), fill="#000000")
    for name, (cx, cy) in spec["lamps"].items():
        lx, lyy = LAMP_SRC[name]
        part = P.crop((lx - 40, lyy - 30, lx + 40, lyy + 74))
        paste_part(im, part, (cx * S, cy * S + 22 * s), s, feather=4)

    lc = pu["logo"]
    paste_part(im, P.crop(PARTS["logo"]), (lc["center"][0] * S, lc["center"][1] * S), lc["scale"], feather=10)

    # VFD windows, with each label at its cell's top-left.
    for win in pu["windows"]:
        fill(im, win["rect"], (8, 8, 8, 8), vfd)
        for line in win.get("dividers", []):
            d.line([v * S for v in line], fill=(34, 70, 62), width=S)
    for cid, (cx, cy, cw, ch) in spec["vfd"]["cells"].items():
        part = P.crop(VFD_LABELS[cid])
        paste_part(im, part, ((cx + 6) * S + part.width * s / 2, (cy + 6) * S + part.height * s / 2), s, feather=4)

    # Recesses from Tab5, scaled.
    (dcx, dcy), dr = DISH_SRC
    dish = P.crop((dcx - dr, dcy - dr, dcx + dr, dcy + dr))
    dm = Image.new("L", dish.size, 0)
    ImageDraw.Draw(dm).ellipse((10, 10, dish.width - 10, dish.height - 10), fill=255)
    paste_part(im, dish, [v * S for v in pu["dpad"]["center"]], s, mask=dm.filter(ImageFilter.GaussianBlur(6)))
    (ccx, ccy), cl, ct, ca = CAPSULE_SRC
    cap = P.crop((ccx - 340, ccy - 340, ccx + 340, ccy + 340))
    cm = Image.new("L", cap.size, 0)
    draw_capsule(ImageDraw.Draw(cm), (cap.width / 2, cap.height / 2), cl - 24, ct - 24, ca, 255)
    paste_part(im, cap, [v * S for v in pu["ab_capsule"]["center"]], s, mask=cm.filter(ImageFilter.GaussianBlur(6)))

    # Printed labels next to A/B and under SELECT/START.
    c = spec["controls"]
    for cid, part in (("a", "label_a"), ("b", "label_b"), ("select", "label_select"), ("start", "label_start")):
        ox, oy = LABEL_OFFSETS[cid]
        paste_part(im, P.crop(PARTS[part]), ((c[cid][0] + ox * s) * S, (c[cid][1] + oy * s) * S), s, feather=4)

    for spx, spy, ln, th, ang in pu["speaker"]:
        draw_capsule(d, (spx * S, spy * S), ln * S, th * S, ang, (36, 30, 62))

    if "sticker" in pu:
        st = pu["sticker"]
        sticker = Image.open(RAWS / board / "sticker.png").convert("RGB")
        paste_part(im, sticker, ((st[0] + st[2] / 2) * S, (st[1] + st[3] / 2) * S), 1.0, feather=3)
    return im


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("board", choices=[b for b in boards() if "pasteup" in load_spec(b)])
    p.add_argument("--output", help="default raws/ui/<board>/pasteup.png")
    p.add_argument("--force", action="store_true", help="replace an existing paste-up")
    args = p.parse_args()
    out = args.output or RAWS / args.board / "pasteup.png"
    # An existing pasteup.png is the record of what the AI pass was given.
    if not args.force and Path(out).exists():
        raise SystemExit(f"{out} exists; pass --force or --output")
    build(args.board).save(out)


if __name__ == "__main__":
    main()

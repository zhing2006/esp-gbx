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
from scipy import ndimage as ndi

from uikit import MASTER_SCALE, RAWS, boards, build_plate, draw_capsule, load_spec, rounded_rect_mask

S = MASTER_SCALE

# Finished parts on the 2x Tab5 plate.
LAMP_SRC = {"gb": (161, 379), "gbc": (161, 501), "gba": (161, 618), "int": (1280, 364),
            "jit": (1280, 483), "save": (1280, 601), "err": (1280, 720)}
VFD_LABELS = {"speed": (160, 1204, 310, 1258), "frame": (772, 1204, 916, 1258),
              "audio": (160, 1374, 300, 1428), "memory": (772, 1374, 872, 1428)}
# The labels' glow beyond those boxes: left, top, right, bottom. The left stays
# clear of the window's lit bevel, the top of the divider between the rows.
VFD_GLOW = (4, 4, 10, 10)
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
# The slot with its bevel, a capsule in the slot_group crop: x0, y0, x1, y1.
SLOT_BEVEL = (6, 31, 322, 111)
PART_MARGIN = 16  # flat margin around a cleaned part, wider than the paste's feather
DISH_SRC = ((352, 2060), 262)
CAPSULE_SRC = ((1058, 2084), 640, 330, -40)
# Tab5 offsets, in logical px, from a button centre to its printed label.
LABEL_OFFSETS = {"a": (54.0, 77.0), "b": (74.0, 57.0), "select": (1.5, 39.5), "start": (1.5, 39.5)}


def sample(P, box):
    return tuple(int(v) for v in np.asarray(P.crop(box)).reshape(-1, 3).mean(0))


def feather_rect(size, f):
    # A part scaled down below twice the feather keeps a solid middle line.
    f = min(f, (min(size) - 1) // 2)
    m = Image.new("L", size, 0)
    ImageDraw.Draw(m).rectangle((f, f, size[0] - 1 - f, size[1] - 1 - f), fill=255)
    return m.filter(ImageFilter.GaussianBlur(f / 2)) if f else m


def paste_part(canvas, part, center, scale, feather=6, mask=None):
    nw, nh = max(1, round(part.width * scale)), max(1, round(part.height * scale))
    part = part.resize((nw, nh), Image.LANCZOS)
    m = mask.resize((nw, nh), Image.LANCZOS) if mask is not None else feather_rect((nw, nh), feather)
    canvas.paste(part, (round(center[0] - nw / 2), round(center[1] - nh / 2)), m)


def clean_lamp(P, lx, ly, color):
    """The Tab5 lamp at (lx, ly), its dome and printed name on a flat `color`.

    The crop's own lens, with the plate's reflections and scratches, would
    show as a box on the paste-up's flat lens. The dome keeps its dark rim and
    the light lip around it, shifted from the plate's lens to `color`; the
    name is a matte of the light print against the lens around it.
    """
    a = np.asarray(P.crop((lx - 40, ly - 30, lx + 40, ly + 74)), dtype=np.float32)
    ys, xs = np.mgrid[:a.shape[0], :a.shape[1]]
    r = np.hypot(xs - 40, ys - 30)
    target = np.array(color, dtype=np.float32)
    out = np.empty_like(a)
    out[:] = target
    shift = target - np.median(a[(r >= 19) & (r < 26)], 0)
    dome = a + shift * np.clip((r - 14) / 3, 0, 1)[..., None]
    m = np.clip((20 - r) / 2.5, 0, 1)[..., None]
    out = out * (1 - m) + dome * m
    name = a[56:92]
    L = name.mean(-1)
    lens = ndi.median_filter(L, size=17)
    strong = L > lens + 60
    ink = name[strong].mean(0)
    near = ndi.binary_dilation(strong, iterations=2)
    alpha = (np.clip((L - lens - 8) / (ink.mean() - lens - 8), 0, 1) * near)[..., None]
    out[56:92] = out[56:92] * (1 - alpha) + ink * alpha
    return Image.fromarray(np.clip(out, 0, 255).round().astype(np.uint8))


def clean_slot_group(P, color):
    """The Tab5 speed switch, its slot and the printed 1X, 2X and SPEED, on a flat `color`.

    As with the lamps, the plate's plastic around them would show as a box.
    The slot keeps its bevel, shifted from the plate's plastic to `color` at
    its rim; the print is a matte of the dark ink against the plastic.
    """
    a = np.asarray(P.crop(PARTS["slot_group"]), dtype=np.float32)
    target = np.array(color, dtype=np.float32)
    x0, y0, x1, y1 = SLOT_BEVEL
    r = (y1 - y0) / 2
    ys, xs = np.mgrid[:a.shape[0], :a.shape[1]]
    # Distance outside the capsule, negative inside.
    d = np.hypot(xs - np.clip(xs, x0 + r, x1 - r), ys - (y0 + r)) - r
    L = a.mean(-1)
    plastic = (d > 4) & (np.abs(L - np.median(L)) < 18)
    shift = target - np.median(a[plastic], 0)
    slot = a + shift * np.clip((d + 6) / 4, 0, 1)[..., None]
    m = np.clip((1.5 - d) / 3, 0, 1)[..., None]
    out = np.empty_like(a)
    out[:] = target
    out = out * (1 - m) + slot * m
    # The print reaches the crop's top edge, where a local estimate of the
    # plastic would take in the ink; the plastic is even enough for one level.
    level = np.median(L[plastic])
    strong = (L < level - 60) & (d > 3)
    ink = a[strong].mean(0)
    near = ndi.binary_dilation(strong, iterations=2) & (d > 1.5)
    alpha = (np.clip((level - L - 8) / (level - ink.mean() - 8), 0, 1) * near)[..., None]
    out = out * (1 - alpha) + ink * alpha
    return with_margin(out, target)


def vfd_glyphs(a):
    """The lit cyan of a VFD label, as export_boards.py finds it."""
    return (a[..., 1] > 150) & (a[..., 2] > 150) & (a[..., 1] - a[..., 0] > 40)


def clean_print(part, color, reach=3, glyphs=None):
    """`part` with only its print kept, on a flat `color`.

    Pixels within `reach` master px of the print are shifted from the part's
    own background, the median of its outer edge, to `color`; the rest
    becomes `color`, so the plate's grain and reflections leave no box. The
    print is what stands out from the background, or what `glyphs` picks
    where scratches and reflections stand out as much.
    """
    a = np.asarray(part, dtype=np.float32)
    edge = np.concatenate([a[:4].reshape(-1, 3), a[-4:].reshape(-1, 3), a[:, :4].reshape(-1, 3),
                           a[:, -4:].reshape(-1, 3)])
    level = np.median(edge, 0)
    core = glyphs(a) if glyphs else np.linalg.norm(a - level, axis=-1) > 40
    w = ndi.binary_dilation(core, iterations=reach).astype(np.float32)
    w = np.clip(ndi.gaussian_filter(w, reach / 2) * 1.5, 0, 1)[..., None]
    target = np.array(color, dtype=np.float32)
    return with_margin(target + (a - level) * w, target)


def with_margin(a, color):
    """`a` in a margin of `color`, so the paste's feathered edge only blends `color`."""
    p = PART_MARGIN
    out = np.empty((a.shape[0] + 2 * p, a.shape[1] + 2 * p, 3), dtype=np.float32)
    out[:] = color
    out[p:-p, p:-p] = a
    return Image.fromarray(np.clip(out, 0, 255).round().astype(np.uint8))


def under(canvas, center, size):
    """The canvas color where a part of `size` will be pasted at `center`, all in master px."""
    (x, y), (w, h) = center, size
    box = (max(0, round(x - w / 2)), max(0, round(y - h / 2)),
           min(canvas.width, round(x + w / 2)), min(canvas.height, round(y + h / 2)))
    return tuple(np.median(np.asarray(canvas.crop(box)).reshape(-1, 3), 0))


def paste_print(canvas, part, center, scale, reach=3, feather=6, glyphs=None):
    """Paste `part`'s print, cleaned onto the canvas color where it lands."""
    color = under(canvas, center, (part.width * scale, part.height * scale))
    paste_part(canvas, clean_print(part, color, reach, glyphs), center, scale, feather)


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
    src = clean_slot_group(P, grey)
    bx0, by0, bx1, by1 = PARTS["slot_group"]
    ox = TAB5_SLOT_CENTER[0] - (bx0 + bx1) / 2
    oy = TAB5_SLOT_CENTER[1] - (by0 + by1) / 2
    paste_part(im, src, ((sx + sw / 2) * S - ox * s, (sy + sh / 2) * S - oy * s), s)

    # Lens, print lines and text, display, lamps.
    fill(im, pu["lens"], pu["lens_radii"], slate)
    lx0, ly, lx1, g0, g1 = pu["print_lines"]
    for i, c in enumerate(((128, 160, 70), (176, 52, 104), (110, 98, 200))):
        yy = (ly + i * 3 * min(1, s * 1.25)) * S
        d.line((lx0 * S, yy, g0 * S, yy), fill=c, width=S)
        d.line((g1 * S, yy, lx1 * S, yy), fill=c, width=S)
    paste_print(im, P.crop(PARTS["print_text"]), ((g0 + g1) / 2 * S, (ly + 3) * S), s)
    x, y, w, h = spec["display"]
    d.rectangle((x * S, y * S, (x + w) * S - 1, (y + h) * S - 1), fill="#000000")
    for name, (cx, cy) in spec["lamps"].items():
        lx, lyy = LAMP_SRC[name]
        paste_part(im, clean_lamp(P, lx, lyy, slate), (cx * S, cy * S + 22 * s), s, feather=4)

    lc = pu["logo"]
    paste_print(im, P.crop(PARTS["logo"]), (lc["center"][0] * S, lc["center"][1] * S), lc["scale"], reach=6,
                feather=10)

    # VFD windows, with each label at its cell's top-left, on boards with a VFD.
    for win in pu.get("windows", []):
        fill(im, win["rect"], (8, 8, 8, 8), vfd)
        for line in win.get("dividers", []):
            d.line([v * S for v in line], fill=(34, 70, 62), width=S)
    for cid, (cx, cy, cw, ch) in spec.get("vfd", {}).get("cells", {}).items():
        x0, y0, x1, y1 = VFD_LABELS[cid]
        gl, gt, gr, gb = VFD_GLOW
        part = P.crop((x0 - gl, y0 - gt, x1 + gr, y1 + gb))
        center = ((cx + 6) * S + (x1 - x0 + gr - gl) / 2 * s, (cy + 6) * S + (y1 - y0 + gb - gt) / 2 * s)
        paste_print(im, part, center, s, reach=8, feather=4, glyphs=vfd_glyphs)

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
        paste_print(im, P.crop(PARTS[part]), ((c[cid][0] + ox * s) * S, (c[cid][1] + oy * s) * S), s, feather=4)

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

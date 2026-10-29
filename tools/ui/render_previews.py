# /// script
# requires-python = ">=3.12"
# dependencies = ["pillow==12.2.0", "numpy==2.5.4", "scipy==1.18.1"]
# ///
"""Render the design previews in docs/ui-layouts from assets/ui.

Each board gets idle.png (no ROM loaded), gb.png and gbc.png (a GB and a GBC
game in their black border) and gba.png, with the lamps a running game would light, and home.png
and pause.png with a menu in the game window. Games and menus are at the
largest whole scale the window holds; a board whose window is too small for
2x also gets settings.png, which then has no SCALING. The HOME of a board in
BLUETOOTH_GAMEPAD has a fourth card, WIRELESS GAMEPAD. menus/ holds every menu
screen and dialog at 2x, from menu_scenes.py. states.png lists every
control, switch and lamp state on the Tab5. The game scene comes from
preview_scene.py and VFD values are shown as uncollected; neither is
emulator output. --check compares instead of writing.
"""

import argparse
import json
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

import menu_kit
from menu_scenes import gamepad_home_screens, screens, small_window_screens
from preview_scene import scene
from uikit import ASSETS, DOCS, boards, place
from vfd_font import VfdFont, draw_cell, placeholder

# Lamps and switch per preview. Pressed states only appear in states.png.
PREVIEWS = {
    "idle": {"system": None, "run_state": "NO ROM", "lamps": {}, "speed": "1x"},
    "gb": {"system": "gb", "lamps": {"gb": "lime", "int": "green"}, "speed": "1x"},
    "gbc": {"system": "gbc", "lamps": {"gbc": "berry", "jit": "green"}, "speed": "1x"},
    "gba": {"system": "gba", "lamps": {"gba": "violet", "jit": "green", "save": "amber"}, "speed": "2x"},
    "home": {"window": "home", "run_state": "NO ROM", "lamps": {}, "speed": "1x"},
    "pause": {"window": "pause", "run_state": "PAUSE", "lamps": {"gb": "lime", "int": "green"}, "speed": "1x"},
}

# Boards whose firmware is built with the Bluetooth gamepad. Keep in step with
# their build configuration.
BLUETOOTH_GAMEPAD = {"jc1060p470", "sila_p4x"}


class Board:
    def __init__(self, name):
        self.dir = ASSETS / name
        self.m = json.load(open(self.dir / "board.json"))
        self.sprites = json.load(open(ASSETS / "common" / "sprites.json"))
        self.font = VfdFont()
        self.cache = {}

    def image(self, name):
        if name not in self.cache:
            self.cache[name] = Image.open(ASSETS / "common" / name).convert("RGBA")
        return self.cache[name]

    def render(self, controls=None, lamps=None, speed="1x", system=None, run_state=None, window=None):
        m, sp = self.m, self.sprites
        controls, lamps = controls or {}, lamps or {}
        base = Image.open(self.dir / m["background"]).convert("RGBA")
        k = m["sprite_scale"]
        for cid, c in m["controls"].items():
            meta = sp["sprites"][c["sprite"]]
            state = controls.get(cid, "normal")
            place(base, self.image(meta["states"][state]), c["center"], meta["anchor"], k)
        knob = sp["sprites"]["knob"]
        place(base, self.image(knob["states"]["normal"]), m["speed_switch"][speed], knob["anchor"], k)
        for lid, color in lamps.items():
            place(base, self.image(sp["lamps"]["colors"][color]), m["lamps"][lid]["center"],
                  sp["lamps"]["anchor"], k)
        for cid, cell in m.get("vfd", {}).items():
            lines = [placeholder(line["text"]) for line in cell.get("lines", [])]
            if cid == "speed" and run_state:
                draw_cell(base, self.font, cell, run_state, unit_text="", lines=lines)
            else:
                draw_cell(base, self.font, cell, placeholder(cell["value"]["binding"]), lines=lines)
        if system:
            x, y, w, h = m["display"]
            g = scene(system)
            k = min(w // g.width, h // g.height)
            g = g.resize((g.width * k, g.height * k), Image.NEAREST)
            base.paste((0, 0, 0, 255), (x, y, x + w, y + h))
            base.paste(g, (x + (w - g.width) // 2, y + (h - g.height) // 2))
        if window:
            base.paste(window, tuple(m["display"][:2]))
        return base.convert("RGB")


def states_sheet(board, zoom=2, pad=10):
    """Every state of every control, rendered in place and cropped, on one sheet."""
    m, sp = board.m, board.sprites
    k = m["sprite_scale"]
    W, H = m["size"]
    rows = []
    for cid, c in m["controls"].items():
        meta = sp["sprites"][c["sprite"]]
        hw, hh = meta["size"][0] * k * 0.43, meta["size"][1] * k * 0.43
        box = (max(0, round(c["center"][0] - hw)), max(0, round(c["center"][1] - hh)),
               min(W, round(c["center"][0] + hw)), min(H, round(c["center"][1] + hh)))
        rows.append([(f"{cid} {st}", board.render(controls={cid: st}).crop(box)) for st in meta["states"]])
    sx, sy, sw, sh = m["speed_switch"]["slot"]
    box = (0, 0, round(sx + sw + 60 * k * 2), round(sy + sh + 14))
    rows.append([(f"speed {v}", board.render(speed=v).crop(box)) for v in ("1x", "2x")])
    off, lit = board.render(), []
    for lid, colors in sp["lamps"]["use"].items():
        x, y = m["lamps"][lid]["center"]
        box = (round(x - 36), round(y - 30), round(x + 36), round(y + 42))
        lit += [(f"{lid} {c}", board.render(lamps={lid: c}).crop(box)) for c in colors]
    rows.append([("off", off.crop(box))] + lit)
    tiles = [[(t, im.resize((im.width * zoom, im.height * zoom), Image.NEAREST)) for t, im in r] for r in rows]
    width = max(sum(im.width + pad for _, im in r) for r in tiles) + pad
    height = sum(max(im.height for _, im in r) + 18 + pad for r in tiles) + pad
    sheet = Image.new("RGB", (width, height), (32, 32, 34))
    d = ImageDraw.Draw(sheet)
    y = pad
    for r in tiles:
        x = pad
        for t, im in r:
            d.text((x, y), t, fill=(220, 220, 220))
            sheet.paste(im, (x, y + 16))
            x += im.width + pad
        y += max(im.height for _, im in r) + 18 + pad
    return sheet


def outputs():
    """(relative path, image) for every preview."""
    result = []
    menus = dict(screens())
    for name in boards():
        b = Board(name)
        previews, windows = PREVIEWS, menus
        _, _, w, h = b.m["display"]
        small = w < 2 * menu_kit.W or h < 2 * menu_kit.H
        menu_kit.use_scale(1 if small else 2)
        if small:
            # The menus at 1x; SETTINGS has no SCALING, as frames are not scaled.
            windows = dict(screens() + small_window_screens())
            previews = PREVIEWS | {"settings": dict(PREVIEWS["home"], window="settings-no-scaling")}
        if name in BLUETOOTH_GAMEPAD:
            windows = windows | dict(gamepad_home_screens())
        menu_kit.use_scale(2)
        for key, p in previews.items():
            im = b.render(lamps=p["lamps"], speed=p["speed"], system=p.get("system"), run_state=p.get("run_state"),
                          window=windows.get(p.get("window")))
            result.append((f"{name}/{key}.png", im))
    result += [(f"menus/{key}.png", im) for key, im in menus.items()]
    result.append(("states.png", states_sheet(Board("m5stack_tab5"))))
    return result


def main():
    p = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    p.add_argument("--check", action="store_true", help="compare with the existing PNGs, write nothing")
    p.add_argument("--output", default=str(DOCS), help="output directory, default docs/ui-layouts")
    args = p.parse_args()
    out = Path(args.output)
    bad = []
    for rel, im in outputs():
        path = out / rel
        if args.check:
            if not path.exists() or not np.array_equal(np.asarray(Image.open(path).convert("RGB")), np.asarray(im)):
                bad.append(rel)
            continue
        path.parent.mkdir(parents=True, exist_ok=True)
        im.save(path, optimize=True)
    if bad:
        print("out of date: " + ", ".join(bad), file=sys.stderr)
        sys.exit(1)


if __name__ == "__main__":
    main()

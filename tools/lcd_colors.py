# /// script
# requires-python = ">=3.12"
# dependencies = ["numpy==2.5.4", "pillow==12.2.0", "colour-science==0.4.6"]
# ///
"""Derive COLOR FIX, the LCD color correction of color games, from
measurements of a GBC screen.

    uv run tools/lcd_colors.py [--compare DIR]

Pan Docs, "RGB Translation by CGBs", describes the GBC's colors but gives no
conversion. This script builds one from the measurements of one GBC by
Pokefan531 in the Handheld Color Space Project, which refs/fetch.sh pins to
a commit in refs/docs/gbc-lcd: reflectance spectra of the screen showing
white, black, red, green, blue, yellow, cyan and magenta, read from two
opposite angles, and 32-step gray ramps from both angles. The project's
shaders and derived parameters are not used. The author marks the data as
incomplete and possibly unreliable: readings below 440 nm are invalid, the
black readings are unsteady, and red, green and blue add up to 93-95 % of
white. The model below deals with each of these.

It writes components/app/src/lcd_colors.h: a channel's response by level and
a matrix and offset that give linear sRGB from the responses of a color's
three channels. The app builds its BGR555 to RGB565 table from them, see
color_fix.cpp. With --compare it also writes comparison images of each
choice below to DIR.
"""

import argparse
import zipfile
import xml.etree.ElementTree as ET
from pathlib import Path

import colour
import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
DATA = ROOT / "refs" / "docs" / "gbc-lcd"
HEADER = ROOT / "components" / "app" / "src" / "lcd_colors.h"

# The choices, made from the comparison images (--compare):
#
# ILLUMINANT and ADAPT: the light the reflective screen is seen under, and
# whether its white is adapted to the display's, D65. Under D65 the screen's
# white is xy (0.299, 0.324), bluer and greener than D65; shown as it is,
# every light color has a cyan cast, and Pan Docs' example $03EF comes out
# pale green. D50 gives a yellow-green cast instead. A player looking at the
# screen adapts to its white, as to a page under its light, so the screen's
# white becomes the display's (Bradford). The colors keep their relations:
# $03EF comes out the pale, washed out yellow Pan Docs describes, and the
# pigments' weak saturation stays. With the adaptation the illuminant
# matters little; D65 is daylight and the display's white.
#
# BLACK: whether the screen's black keeps its measured level. The screen
# reflects 28 % as much light at black as at white, which the display would
# show as a gray behind every dark color and a flat, washed out image. The
# black level is taken out: the black's light is subtracted from every
# color, so black is black. Pan Docs' "the highest intensity will produce
# light gray rather than white" is about the screen beside a lit one; here
# white is the display's white either way.
ILLUMINANT = "D65"
ADAPT = True
KEEP_BLACK = False

COLORS = ["White", "Black", "Red", "Green", "Blue", "Yellow", "Cyan", "Magenta"]
# Readings below this are invalid, see the project's notes; each spectrum
# keeps its value here below it.
VALID_FROM_NM = 440
GRID = np.arange(380, 731, 5)


def ods_rows(path):
    """The cells of each row of the first sheet, as text."""
    table = "{urn:oasis:names:tc:opendocument:xmlns:table:1.0}"
    with zipfile.ZipFile(path) as z:
        root = ET.fromstring(z.read("content.xml"))
    rows = []
    for row in next(root.iter(table + "table")).iter(table + "table-row"):
        cells = []
        for cell in row.findall(table + "table-cell"):
            cells += ["".join(cell.itertext())] * int(cell.get(table + "number-columns-repeated", "1"))
        while cells and not cells[-1]:
            cells.pop()
        if cells:
            rows.append(cells)
    return rows


def spectra(path):
    """Reflectance by color name on GRID, from one of the color sheets."""
    rows = ods_rows(path)
    head = rows[0]
    first = head.index("380") if "380" in head else 7
    wavelengths = np.array([float(w) for w in head[first:]])
    out = {}
    for row in rows[1:]:
        if row[0] in COLORS:
            values = np.array([float(v) for v in row[first:first + len(wavelengths)]])
            values = np.where(wavelengths < VALID_FROM_NM, np.interp(VALID_FROM_NM, wavelengths, values), values)
            out[row[0]] = np.interp(GRID, wavelengths, values)
    missing = set(COLORS) - set(out)
    if missing:
        raise SystemExit(f"{path.name}: no {', '.join(sorted(missing))}")
    return out


def gray_ramp(path):
    """Y of the 32 gray levels, black first: the mean of each level's five
    readings, which the export separates with blank lines."""
    levels, level = [], []
    for line in path.read_text().splitlines():
        cells = line.split("\t")
        if len(cells) > 3 and cells[0].isdigit():
            level.append(float(cells[2]))
        elif not line.strip() and level:
            levels.append(np.mean(level))
            level = []
    if level:
        levels.append(np.mean(level))
    if len(levels) != 32:
        raise SystemExit(f"{path.name}: {len(levels)} gray levels, expected 32")
    return np.array(levels)


def xyz(reflectance, illuminant):
    """XYZ of a reflectance under an illuminant, a perfect white's Y being 1."""
    sd = colour.SpectralDistribution(dict(zip(GRID.tolist(), reflectance.tolist())))
    cmfs = colour.MSDS_CMFS["CIE 1931 2 Degree Standard Observer"]
    return colour.sd_to_XYZ(sd, cmfs, colour.SDS_ILLUMINANTS[illuminant]) / 100


def model(illuminant, adapt, keep_black):
    """The response by level, and the matrix and offset to linear sRGB."""
    sheets = [spectra(DATA / "gbc-color1 2026-05-13.ods"), spectra(DATA / "gbc-color2 2026-05-13.ods")]
    # The two angles straddle what a player sees; their mean stands for it.
    measured = {name: xyz((sheets[0][name] + sheets[1][name]) / 2, illuminant) for name in COLORS}
    white, black = measured["White"], measured["Black"]
    primaries = np.stack([measured[c] - black for c in ("Red", "Green", "Blue")], axis=1)
    # Red, green and blue fall short of white: scale each so that together
    # they make it, keeping their chromaticities.
    scale = np.linalg.solve(primaries, white - black)
    channels = primaries * scale
    # The gray ramps of both angles, as a share of the way from black to
    # white; a level never comes out darker than the one below it.
    ramp = (gray_ramp(DATA / "gbc-gamma1 2026-05-09.txt") + gray_ramp(DATA / "gbc-gamma2 2026-05-09.txt")) / 2
    response = np.maximum.accumulate((ramp - ramp[0]) / (ramp[-1] - ramp[0]))
    response[0], response[-1] = 0, 1
    # Only the gray ramp covers all levels on both angles: the color ramps
    # are preliminary, of one angle, and noisy in the dark. Each channel
    # takes the gray's response.
    # XYZ with white's Y at 1: black on top of the channels, or not.
    if keep_black:
        channels_xyz, offset_xyz = channels / white[1], black / white[1]
    else:
        channels_xyz, offset_xyz = channels / (white[1] - black[1]), np.zeros(3)
    if adapt:
        top = channels_xyz.sum(axis=1) + offset_xyz
        d65 = colour.xy_to_XYZ(colour.CCS_ILLUMINANTS["CIE 1931 2 Degree Standard Observer"]["D65"])
        cat = colour.adaptation.matrix_chromatic_adaptation_VonKries(top / top[1], d65, "Bradford")
        channels_xyz, offset_xyz = cat @ channels_xyz, cat @ offset_xyz
    to_srgb = colour.models.RGB_COLOURSPACE_sRGB.matrix_XYZ_to_RGB
    matrix, offset = to_srgb @ channels_xyz, to_srgb @ offset_xyz
    # White at the display's brightest: its largest channel at 1, so that
    # the screen's white is not clipped to another color.
    brightest = (matrix.sum(axis=1) + offset).max()
    matrix, offset = matrix / brightest, offset / brightest
    report = {"scale": scale, "white": white, "black": black, "measured": measured, "channels": channels}
    return response, matrix, offset, report


def table(response, matrix, offset):
    """RGB888 of each BGR555 color, as color_fix.cpp computes it."""
    c = np.arange(32768)
    levels = np.stack([response[c & 31], response[(c >> 5) & 31], response[(c >> 10) & 31]], axis=1)
    linear = np.clip(levels @ matrix.T + offset, 0, 1)
    encoded = np.where(linear <= 0.0031308, linear * 12.92, 1.055 * linear ** (1 / 2.4) - 0.055)
    return np.round(encoded * 255).astype(np.uint8)


def write_header(response, matrix, offset):
    def floats(values):
        return ", ".join(f"{v:.6f}f" for v in values)

    lines = [
        "#pragma once",
        "",
        "// Generated by tools/lcd_colors.py from measurements of a GBC screen; see",
        "// there for the source and the choices. Do not edit.",
        "namespace gbx::app::lcd {",
        "// A channel's light by level 0..31, as a share of the way from black to",
        "// white.",
        "inline constexpr float kResponse[32] = {",
        *[f"    {floats(response[i:i + 8])}," for i in range(0, 32, 8)],
        "};",
        "// Linear sRGB of a color: kMatrix times the responses of its red, green",
        "// and blue, plus kOffset.",
        "inline constexpr float kMatrix[3][3] = {",
        *[f"    {{{floats(row)}}}," for row in matrix],
        "};",
        f"inline constexpr float kOffset[3] = {{{floats(offset)}}};",
        "}  // namespace gbx::app::lcd",
        "",
    ]
    HEADER.write_text("\n".join(lines), newline="\n")


def chart(lut):
    """Ramps of gray, red, green, blue, yellow, cyan and magenta, 32 levels
    each, then Pan Docs' example $03EF beside full green and red, 8x8 cells."""
    ramps = [(1, 1, 1), (1, 0, 0), (0, 1, 0), (0, 0, 1), (1, 1, 0), (0, 1, 1), (1, 0, 1)]
    rows = [[(r * v) | (g * v) << 5 | (b * v) << 10 for v in range(32)] for r, g, b in ramps]
    rows.append([0x03EF] * 10 + [0x03E0] * 11 + [0x001F] * 11)
    cells = np.array([[lut[c] for c in row] for row in rows], np.uint8)
    return Image.fromarray(cells).resize((32 * 8, len(rows) * 8), Image.NEAREST)


def compare(out):
    """Each choice, beside the colors as they are, on the chart and on
    cgb-acid2's reference image."""
    out.mkdir(parents=True, exist_ok=True)
    plain = np.array([[(c & 31) * 255 // 31, ((c >> 5) & 31) * 255 // 31, ((c >> 10) & 31) * 255 // 31]
                      for c in range(32768)], np.uint8)
    reference = np.asarray(Image.open(ROOT / "refs/tests/game-boy-test-roms/cgb-acid2/cgb-acid2.png").convert("RGB"))
    r, g, b = (reference[..., i].astype(np.int64) >> 3 for i in range(3))
    acid = r | g << 5 | b << 10
    variants = [("plain", plain)]
    for illuminant in ("D65", "D50"):
        for adapt, keep in ((True, False), (True, True), (False, False)):
            response, matrix, offset, report = model(illuminant, adapt, keep)
            name = f"{illuminant} {'adapted' if adapt else 'as measured'}, {'black kept' if keep else 'black removed'}"
            variants.append((name, table(response, matrix, offset)))
        print(f"{illuminant}: white xy {colour.XYZ_to_xy(report['white']).round(4)} Y {report['white'][1]:.4f}, "
              f"black Y {report['black'][1]:.4f}, scale {report['scale'].round(3)}")
        measured = report["measured"]
        for name, mix in (("Yellow", (1, 1, 0)), ("Cyan", (0, 1, 1)), ("Magenta", (1, 0, 1))):
            predicted = report["black"] + report["channels"] @ np.array(mix, float)
            print(f"  {name}: measured Y {measured[name][1]:.4f}, model {predicted[1]:.4f}")
    for name, lut in variants:
        chart(lut).save(out / f"chart {name}.png")
        Image.fromarray(lut[acid]).resize((320, 288), Image.NEAREST).save(out / f"cgb-acid2 {name}.png")
    print("wrote", out)


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--compare", type=Path, help="write comparison images to this directory")
    args = parser.parse_args()
    response, matrix, offset, _ = model(ILLUMINANT, ADAPT, KEEP_BLACK)
    write_header(response, matrix, offset)
    print("wrote", HEADER.relative_to(ROOT))
    if args.compare:
        compare(args.compare)


if __name__ == "__main__":
    main()

"""Draw the menus in the 480x320 game window, for the design previews.

The menus are pixel art on a 240x160 grid, shown at 2x: the 12 px font, lines
and corners all keep the same pixel size. Colors, palettes and icons come from
assets/ui/common/menu.json and glyphs from menu_font.bin, the same data the
firmware packs. Each screen function takes a plain description of what the
screen shows and returns the window as a 480x320 RGB image.
"""

import json

import numpy as np
from PIL import Image

from menu_font import MenuFont
from uikit import ASSETS

THEME = json.load(open(ASSETS / "common" / "menu.json"))
W, H = THEME["grid"]
SCALE = THEME["scale"]
C = {k: tuple(v) for k, v in THEME["colors"].items()}

LINE = 16      # font line box
HEADER = 16    # title bar
ROW = 18       # list row
ROWS = 7       # rows per page
FOOTER = HEADER + ROWS * ROW  # 142, the rule above the footer line
ELLIPSIS = "..."
# Insets of the first rows of a rounded corner, per radius.
CORNERS = {0: [], 1: [1], 2: [2, 1], 3: [3, 1, 1], 4: [4, 2, 1, 1]}


class Canvas:
    def __init__(self, fill=C["paper"]):
        self.a = np.zeros((H, W, 3), dtype=np.uint8)
        self.a[:] = fill
        self.font = FONT
        self.pictures = []

    def image(self):
        im = Image.fromarray(self.a).resize((W * SCALE, H * SCALE), Image.NEAREST)
        for x, y, picture in self.pictures:
            im.paste(picture, (x * SCALE, y * SCALE))
        return im

    def picture(self, x, y, im):
        """Place `im` at one screen pixel per pixel, its top left on cell (x, y),
        over whatever the grid draws."""
        self.pictures.append((x, y, im.convert("RGB")))

    def fill(self, x, y, w, h, color):
        x0, y0, x1, y1 = max(0, x), max(0, y), min(W, x + w), min(H, y + h)
        if x1 > x0 and y1 > y0:
            self.a[y0:y1, x0:x1] = color

    def blend(self, x, y, w, h, color, amount):
        """Mix the area toward `color` by `amount`, 0..1."""
        x0, y0, x1, y1 = max(0, x), max(0, y), min(W, x + w), min(H, y + h)
        part = self.a[y0:y1, x0:x1].astype(np.float32)
        self.a[y0:y1, x0:x1] = np.round(part + (np.array(color, np.float32) - part) * amount).astype(np.uint8)

    def rrect(self, x, y, w, h, r, color):
        inset = CORNERS[min(r, h // 2, w // 2)]
        for row in range(h):
            k = inset[row] if row < len(inset) else inset[h - 1 - row] if h - 1 - row < len(inset) else 0
            self.fill(x + k, y + row, w - 2 * k, 1, color)

    def frame(self, x, y, w, h, r, line, inside):
        """A rounded rectangle with a 1 px outline."""
        self.rrect(x, y, w, h, r, line)
        self.rrect(x + 1, y + 1, w - 2, h - 2, max(0, r - 1), inside)

    def icon(self, name, x, y, color, zoom=1):
        for row, bits in enumerate(THEME["icons"][name]):
            for col, bit in enumerate(bits):
                if bit == "#":
                    self.fill(x + col * zoom, y + row * zoom, zoom, zoom, color)

    def glyphs(self, text):
        """(advance, glyph or None) per character; None draws the missing-glyph box."""
        out = []
        for ch in text:
            g = self.font.glyph(ord(ch))
            out.append((g[0] if g else 12, g))
        return out

    def measure(self, text):
        return sum(a for a, _ in self.glyphs(text))

    def fit(self, text, width):
        """`text`, cut and ended with an ellipsis when it is wider than `width`."""
        if self.measure(text) <= width:
            return text
        room = width - self.measure(ELLIPSIS)
        out, used = "", 0
        for ch, (advance, _) in zip(text, self.glyphs(text)):
            if used + advance > room:
                break
            out += ch
            used += advance
        return out.rstrip() + ELLIPSIS

    def text(self, x, top, text, color, width=None, align="left"):
        """Draw one line whose line box starts at `top`; returns where the pen ends."""
        if width is not None:
            text = self.fit(text, width)
            if align == "right":
                x -= self.measure(text)
            elif align == "center":
                x -= self.measure(text) // 2
        elif align == "right":
            x -= self.measure(text)
        elif align == "center":
            x -= self.measure(text) // 2
        for advance, g in self.glyphs(text):
            if g is None:
                # A box as tall as a CJK glyph stands in for a character the font lacks.
                self.fill(x + 1, top + 3, 10, 1, color)
                self.fill(x + 1, top + 13, 10, 1, color)
                self.fill(x + 1, top + 3, 1, 11, color)
                self.fill(x + 10, top + 3, 1, 11, color)
            else:
                _, left, gtop, gw, gh, rows = g
                for r, bits in enumerate(rows):
                    for c, bit in enumerate(bits):
                        if bit:
                            self.fill(x + left + c, top + gtop + r, 1, 1, color)
            x += advance
        return x

    def wrap(self, text, width):
        """Lines of `text` no wider than `width`: break at spaces, or between CJK characters."""
        lines = []
        for paragraph in text.split("\n"):
            line = ""
            for word in split_words(paragraph):
                trial = line + word
                if line and self.measure(trial.rstrip()) > width:
                    lines.append(line.rstrip())
                    line = word.lstrip()
                else:
                    line = trial
            lines.append(line.rstrip())
        return lines


def split_words(text):
    """Words with their trailing space; each CJK character is a word of its own."""
    words, word = [], ""
    for ch in text:
        if ord(ch) >= 0x2E80:
            if word:
                words.append(word)
            words.append(ch)
            word = ""
        else:
            word += ch
            if ch == " ":
                words.append(word)
                word = ""
    if word:
        words.append(word)
    return words


FONT = MenuFont()


# Building blocks shared by the screens.

def header(cv, title, path="", right=""):
    cv.fill(0, 0, W, HEADER, C["header"])
    x = cv.text(6, 0, title, C["header_ink"])
    right_x = W - 6
    if right:
        right_x = cv.text(W - 6, 0, right, C["header_muted"], align="right") - cv.measure(right) - 8
    if path:
        cv.text(x + 6, 0, path, C["header_muted"], width=right_x - x - 6)


def footer(cv, text, color=C["muted"]):
    cv.fill(0, FOOTER, W, 1, C["rule"])
    # Fields two spaces apart, such as a ROM's title, MBC and RAM, drop from
    # the end before the text ends in an ellipsis.
    while cv.measure(text) > W - 12 and text.rfind("  ") > 0:
        text = text[:text.rfind("  ")]
    cv.text(6, FOOTER + 1, text, color, width=W - 12)


def badge(cv, x, y, machine, enabled, selected):
    w, h = 24, 13
    if selected:
        cv.rrect(x, y, w, h, 2, C["accent_ink"])
        ink = C["accent"]
    else:
        cv.rrect(x, y, w, h, 2, C[machine] if enabled else C["disabled"])
        ink = C["badge_ink"]
    label = machine.upper()
    cv.text(x + (w - cv.measure(label) + 1) // 2, y - 2, label, ink)


def scrollbar(cv, first, total):
    if total <= ROWS:
        return
    top, height = HEADER + 2, ROWS * ROW - 4
    cv.fill(W - 4, top, 2, height, C["rule"])
    thumb = max(8, height * ROWS // total)
    at = top + (height - thumb) * first // (total - ROWS)
    cv.fill(W - 4, at, 2, thumb, C["muted"])


def bar_color(enabled):
    """The selection bar: accent on what can be used, gray on what cannot."""
    return C["accent"] if enabled else C["muted"]


def selection(cv, y, h, right=W - 2, enabled=True):
    cv.rrect(2, y, right - 2, h, 2, bar_color(enabled))


def triangle(cv, x, y, direction, color):
    """A 3x5 arrow pointing left or right, its box at (x, y)."""
    for i, h in enumerate((1, 3, 5) if direction == "left" else (5, 3, 1)):
        cv.fill(x + i, y + (5 - h) // 2, 1, h, color)


def card(cv, x, y, w, h, title, tone="header"):
    """A dialog card with a title bar and a drop shadow; returns the body's top."""
    cv.blend(x + 2, y + 2, w, h, (0, 0, 0), 0.35)
    cv.rrect(x, y, w, h, 3, C["paper"])
    cv.rrect(x, y, w, HEADER, 3, C[tone])
    cv.fill(x, y + HEADER - 3, w, 3, C[tone])
    cv.text(x + 6, y, title, C["header_ink"], width=w - 12)
    return y + HEADER


def buttons(cv, cx, y, labels, selected, enabled=None):
    """A centered row of pill buttons with their top at y. `selected` may be
    None when the cursor is elsewhere; a button that cannot be used is gray."""
    pad, gap, h = 8, 6, 14
    enabled = enabled or [True] * len(labels)
    widths = [cv.measure(t) + 2 * pad for t in labels]
    x = cx - (sum(widths) + gap * (len(labels) - 1)) // 2
    for i, (label, w) in enumerate(zip(labels, widths)):
        if i == selected:
            cv.rrect(x, y, w, h, 3, bar_color(enabled[i]))
            ink = C["accent_ink"]
        else:
            cv.frame(x, y, w, h, 3, C["rule"], C["card"])
            ink = C["ink"] if enabled[i] else C["disabled"]
        cv.text(x + pad, y - 1, label, ink)
        x += w + gap


# Screens.

HOME_PLACES = 4  # cards the main screen has room for


def home(entries, selected, status, note):
    """The main screen: one card per entry, icon, label and a short detail.
    The cards keep their places when there are fewer than four; the places
    below the last one stay empty."""
    cv = Canvas()
    header(cv, "HOME", right=status)
    h, gap = 26, 4
    pad = (h - LINE) // 2
    y = HEADER + (FOOTER - HEADER - HOME_PLACES * h - (HOME_PLACES - 1) * gap) // 2
    for i, e in enumerate(entries):
        sel = i == selected
        if sel:
            cv.rrect(8, y, W - 16, h, 3, bar_color(e["enabled"]))
            ink = detail = C["accent_ink"]
        else:
            cv.frame(8, y, W - 16, h, 3, C["rule"], C["card"])
            ink = C["ink"] if e["enabled"] else C["disabled"]
            detail = C["muted"] if e["enabled"] else C["disabled"]
        cv.icon(e["icon"], 16, y + pad, ink)
        cv.text(40, y + pad, e["label"], ink)
        if e.get("detail"):
            cv.text(W - 18, y + pad, e["detail"], detail, align="right")
        y += h + gap
    # The reason an entry cannot be used is red, as in the other lists.
    footer(cv, note, C["muted"] if entries[selected]["enabled"] else C["alert"])
    return cv.image()


def rom_list(path, items, selected, first=0, status="", note=None, note_alert=False):
    """A folder of roms/: subfolders first, then ROMs with their machine badge."""
    cv = Canvas()
    header(cv, "ROMS", path=path, right=status or f"{selected + 1}/{len(items)}")
    right = W - 6 if len(items) <= ROWS else W - 8
    for row, item in enumerate(items[first:first + ROWS]):
        i = first + row
        y = HEADER + row * ROW
        sel = i == selected
        enabled = item.get("enabled", True)
        if sel:
            selection(cv, y + 1, ROW - 2, right, enabled)
            ink = side = C["accent_ink"]
        else:
            ink = C["ink"] if enabled else C["disabled"]
            side = C["muted"] if enabled else C["disabled"]
        tag = str(item["count"]) if item["kind"] == "dir" else ("SAV" if item.get("save") else "")
        tag_x = right - 4
        name_right = tag_x - (cv.measure(tag) + 8 if tag else 0)
        if item["kind"] == "dir":
            cv.icon("folder", 8, y + 1, C["accent_ink"] if sel else C["muted"])
        else:
            badge(cv, 6, y + 3, item["machine"], enabled, sel)
        cv.text(34, y + 1, item["name"], ink, width=name_right - 34)
        if tag:
            cv.text(tag_x, y + 1, tag, side, align="right")
    scrollbar(cv, first, len(items))
    item = items[selected]
    footer(cv, note if note is not None else item.get("detail", ""), C["alert"] if note_alert else C["muted"])
    return cv.image()


def message(title, icon, heading, body):
    """A screen with nothing to list, such as a missing card."""
    cv = Canvas()
    header(cv, title)
    cv.icon(icon, W // 2 - 16, 34, C["muted"], zoom=2)
    cv.text(W // 2, 74, heading, C["ink"], align="center")
    for n, line in enumerate(cv.wrap(body, W - 40)):
        cv.text(W // 2, 94 + n * LINE, line, C["muted"], align="center")
    return cv.image()


SETTING_ROW = 21  # six rows fill the page between the title bar and the footer
SETTING_LEFT, SETTING_RIGHT = 10, W - 10


def choice_width(cv, label, swatch):
    """A choice's pill: its label, after a 2x2 chip of the palette's four
    colors when it has a swatch."""
    return cv.measure(label) + 12 + (6 if swatch else 0)


def settings(items, selected, in_game):
    """Levels as segment bars, choices as pills; unavailable rows are gray.
    A row may also gray single choices, its "off" ones, which the cursor
    skips; the note then says why when the row is selected."""
    cv = Canvas()
    header(cv, "SETTINGS", right="IN GAME" if in_game else "")
    h = SETTING_ROW
    for i, it in enumerate(items):
        y = HEADER + i * h
        sel = i == selected
        on = it["enabled"]
        if sel:
            cv.rrect(4, y + 2, W - 8, 18, 2, bar_color(on))
        ink = C["accent_ink"] if sel else C["ink"] if on else C["disabled"]
        cv.text(SETTING_LEFT, y + 3, it["label"], ink)
        right = SETTING_RIGHT
        if it["kind"] == "level":
            value = f"{it['value']}%" if on or not it.get("unsupported") else "N/A"
            cv.text(right, y + 3, value, ink, align="right")
            bar_right = right - 34
            seg, gap, n = 5, 1, 10
            x = bar_right - n * (seg + gap) + gap
            if sel and on:
                triangle(cv, x - 7, y + 9, "left", C["accent_ink"])
                triangle(cv, bar_right + 4, y + 9, "right", C["accent_ink"])
            lit = it["value"] // 10 if not it.get("unsupported") else 0
            for k in range(n):
                if sel:
                    color = C["accent_ink"] if k < lit else (C["accent_dim"] if on else C["disabled"])
                elif not on:
                    color = C["disabled"] if k < lit else C["rule"]
                else:
                    color = C["accent"] if k < lit else C["rule"]
                cv.fill(x + k * (seg + gap), y + 7, seg, 9, color)
        else:
            x = right
            off = it.get("off", [])
            for k in reversed(range(len(it["choices"]))):
                label = it["choices"][k]
                swatch = it.get("swatches", [None] * len(it["choices"]))[k]
                w = choice_width(cv, label, swatch)
                x -= w
                chosen = k == it["value"]
                if not on:
                    fill, line, tink = (C["disabled"], C["disabled"], C["paper"]) if chosen else \
                        (C["paper"], C["rule"], C["disabled"])
                elif k in off:
                    fill, line, tink = (C["accent"], C["accent_dim"], C["accent_dim"]) if sel else \
                        (C["paper"], C["rule"], C["disabled"])
                elif sel:
                    fill, line, tink = (C["accent_ink"], C["accent_ink"], C["accent"]) if chosen else \
                        (C["accent"], C["accent_dim"], C["accent_ink"])
                else:
                    fill, line, tink = (C["ink"], C["ink"], C["paper"]) if chosen else \
                        (C["paper"], C["rule"], C["ink"])
                cv.frame(x, y + 4, w, 14, 3, line, fill)
                tx = x + 6
                if swatch:
                    # Lightest at the top left, darkest at the bottom right. A
                    # chip that cannot be chosen fades into its pill.
                    for s, rgb in enumerate(THEME["palettes"][swatch]):
                        cv.fill(x + 4 + s % 2 * 3, y + 8 + s // 2 * 3, 3, 3, tuple(rgb))
                    if not on or k in off:
                        cv.blend(x + 4, y + 8, 6, 6, fill, 0.6)
                    tx = x + 13
                cv.text(tx, y + 3, label, tink)
                x -= 3
    it = items[selected]
    footer(cv, it["note"], C["muted"] if it["enabled"] and not it.get("alert") else C["alert"])
    return cv.image()


def game_frame(cv, frame):
    """The paused game's last frame, dimmed, as the window shows it."""
    cv.fill(0, 0, W, H, (0, 0, 0))
    if frame is not None:
        f = np.asarray(frame.convert("RGB"))
        x, y = (W - f.shape[1]) // 2, (H - f.shape[0]) // 2
        cv.a[y:y + f.shape[0], x:x + f.shape[1]] = f
    cv.blend(0, 0, W, H, C["scrim"], 0.72)


def pause(frame, items, selected, save):
    """The pause menu over the frozen game: actions, then the save state. An
    item's "detail", such as the number of errors on ERRORS, is on its right,
    red unless the item names another "tone"."""
    cv = Canvas()
    game_frame(cv, frame)
    w = 168
    row = ROW
    h = HEADER + 4 + len(items) * row + 4 + 1 + LINE + 2
    x, y = (W - w) // 2, (H - h) // 2
    cv.blend(x + 2, y + 2, w, h, (0, 0, 0), 0.5)
    cv.rrect(x, y, w, h, 3, C["paper"])
    cv.rrect(x, y, w, HEADER, 3, C["header"])
    cv.fill(x, y + HEADER - 3, w, 3, C["header"])
    cv.text(x + 6, y, "PAUSED", C["header_ink"])
    top = y + HEADER + 4
    for i, it in enumerate(items):
        ry = top + i * row
        sel = i == selected
        if sel:
            cv.rrect(x + 3, ry + 1, w - 6, row - 2, 2, bar_color(it["enabled"]))
        ink = C["accent_ink"] if sel else C["ink"] if it["enabled"] else C["disabled"]
        cv.text(x + 10, ry + (row - LINE) // 2, it["label"], ink)
        if it.get("detail"):
            cv.text(x + w - 10, ry + (row - LINE) // 2, it["detail"],
                    C["accent_ink"] if sel else C[it.get("tone", "alert")], align="right")
    sy = top + len(items) * row + 4
    cv.fill(x + 6, sy, w - 12, 1, C["rule"])
    sy += 1
    cv.text(x + 10, sy, "SAVE", C["muted"])
    color = C[save.lower()] if save != "WRITING" else C["dirty"]
    end = x + w - 10
    tx = end - cv.measure(save)
    cv.text(end, sy, save, color, align="right")
    cv.rrect(tx - 9, sy + 5, 6, 6, 1, color)
    return cv.image()


SLOT_ROW = 30         # two lines: the slot's name, then when it was saved or its state
SLOT_RIGHT = 130      # right end of the slot rows
THUMB = (145, 19)     # the thumbnail's frame; the picture inside starts one cell in
# A GBA game's thumbnail is wider, so the rows end sooner.
WIDE_SLOT_RIGHT = 112
WIDE_THUMB = (116, 19)
# Second line of a slot that is not a plain save: text and color.
SLOT_STATES = {
    "empty": ("EMPTY", "disabled"),
    "damaged": ("DAMAGED", "alert"),
    "version": ("OTHER VERSION", "alert"),
    "rom": ("OTHER ROM", "dirty"),
    "mode": ("OTHER MODE", "alert"),
}


def slots(rom, items, selected, note, note_tone="muted", button=None, wide=False):
    """SAVE / LOAD: the slots on the left, the selected one's thumbnail and the
    SAVE and LOAD buttons on the right. A slot has a "state", "saved" or one
    of SLOT_STATES, and may have a "date", None when the board's clock was not
    trusted, and a "frame"; a slot being written has its "progress" in
    percent. The thumbnail is the game frame at one screen pixel per pixel:
    160x144 takes 80x72 cells, and a GBA game's 240x160 (`wide`) 120x80. A
    slot of another run mode keeps it. `button` is the button the cursor is on
    after A, or None while it is on the list."""
    right = WIDE_SLOT_RIGHT if wide else SLOT_RIGHT
    tx, ty = WIDE_THUMB if wide else THUMB
    tw, th = (122, 82) if wide else (82, 74)
    cv = Canvas()
    header(cv, "SAVE / LOAD", path=rom)
    writing = any("progress" in s for s in items)
    top = HEADER + 3
    for i, s in enumerate(items):
        y = top + i * SLOT_ROW
        sel = i == selected
        word, tone = SLOT_STATES.get(s["state"], (s.get("date") or "TIME UNKNOWN", "muted"))
        on_bar = sel and button is None
        if on_bar:
            # Nothing else can start until a write is done, so the bar is gray.
            selection(cv, y + 1, SLOT_ROW - 2, right, not writing)
            ink = side = C["accent_ink"]
        else:
            if sel:
                cv.frame(2, y + 1, right - 2, SLOT_ROW - 2, 2, C["accent"], C["paper"])
            ink, side = C["ink"], C[tone]
        cv.text(10, y + 1, f"SLOT {i + 1}", ink)
        if "progress" in s:
            bw = right - 20
            cv.rrect(10, y + 20, bw, 5, 2, C["disabled"] if on_bar else C["rule"])
            cv.rrect(10, y + 20, max(5, bw * s["progress"] // 100), 5, 2, C["accent_ink"] if on_bar else C["accent"])
        else:
            cv.text(10, y + 14, word, side)
    s = items[selected]
    cv.frame(tx, ty, tw, th, 0, C["rule"], C["card"])
    if s.get("frame") is not None:
        cv.picture(tx + 1, ty + 1, s["frame"])
    can_load = s["state"] in ("saved", "rom") and not writing
    buttons(cv, tx + tw // 2, top + 4 * SLOT_ROW - 15, ["SAVE", "LOAD"], button, [not writing, can_load])
    footer(cv, note, C[note_tone])
    return cv.image()


def errors(entries, selected, status=""):
    """Recent errors, newest first: how long ago and why. CLEAR empties the
    list and turns the ERR lamp off; an entry takes up to two lines."""
    cv = Canvas()
    header(cv, "ERRORS", right=status)
    y = HEADER + 4
    for e in entries:
        lines = cv.wrap(e["text"], W - 44)
        if len(lines) > 2:
            lines = [lines[0], cv.fit(e["text"][len(lines[0]):].lstrip(), W - 44)]
        if y + len(lines) * LINE > FOOTER - 2:
            break
        cv.text(30, y, e["age"], C["muted"], align="right")
        for n, line in enumerate(lines):
            cv.text(38, y + n * LINE, line, C["ink"])
        y += len(lines) * LINE + 2
    cv.fill(0, FOOTER, W, 1, C["rule"])
    buttons(cv, W // 2, FOOTER + 2, ["CLEAR", "BACK"], selected)
    return cv.image()


def loading(name, progress):
    cv = Canvas()
    cv.icon("cartridge", W // 2 - 16, 34, C["header"], zoom=2)
    cv.text(W // 2, 74, "LOADING", C["ink"], align="center")
    cv.text(W // 2, 90, name, C["muted"], width=W - 40, align="center")
    bw = 120
    bx = (W - bw) // 2
    cv.rrect(bx, 114, bw, 5, 2, C["rule"])
    cv.rrect(bx, 114, max(5, bw * progress // 100), 5, 2, C["accent"])
    return cv.image()


def dialog(under, title, lines, labels=(), selected=0, tone="header", progress=None):
    """A modal card over another screen, centered, with its buttons at the bottom."""
    cv = Canvas()
    cv.a[:] = np.asarray(under.convert("RGB").resize((W, H), Image.NEAREST))
    cv.blend(0, 0, W, H, C["scrim"], 0.55)
    w = 208
    # A line is text to wrap, or a (label, value) pair shown in two columns.
    text = [ln for t in lines for ln in ([t] if isinstance(t, tuple) else cv.wrap(t, w - 16))]
    h = HEADER + 6 + len(text) * LINE + 6 + (20 if labels else 0) + (12 if progress is not None else 0)
    x, y = (W - w) // 2, (H - h) // 2
    top = card(cv, x, y, w, h, title, tone)
    for n, line in enumerate(text):
        if isinstance(line, tuple):
            cv.text(x + 8, top + 6 + n * LINE, line[0], C["muted"])
            cv.text(x + 44, top + 6 + n * LINE, line[1], C["ink"], width=w - 52)
        else:
            cv.text(x + 8, top + 6 + n * LINE, line, C["ink"])
    by = top + 6 + len(text) * LINE + 6
    if progress is not None:
        cv.rrect(x + 24, by, w - 48, 5, 2, C["rule"])
        cv.rrect(x + 24, by, max(5, (w - 48) * progress // 100), 5, 2, C["accent"])
        by += 12
    if labels:
        buttons(cv, W // 2, by, labels, selected)
    return cv.image()

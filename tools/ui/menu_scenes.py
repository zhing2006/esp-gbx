"""Sample contents of each menu screen for the design previews.

The file names, counts and header details are made up to show how the
screens lay out; they are not read from a card or a ROM.
"""

from PIL import Image, ImageChops

from menu_kit import dialog, errors, gamepad, home, loading, message, pause, rom_list, settings, slots
from preview_scene import scene

HOME = [
    {"icon": "cartridge", "label": "ROMS", "detail": "48", "enabled": True},
    {"icon": "gear", "label": "SETTINGS", "enabled": True},
    {"icon": "alert", "label": "ERRORS", "detail": "NONE", "enabled": False},
]

ERRORS = [
    {"age": "<1m", "text": "Can't save Pokemon Yellow: the SD card is full."},
    {"age": "2h", "text": "Pokemon Gold stopped: execution stopped at PC 4a3c, which the interpreter cannot run."},
    {"age": "3d", "text": "Can't write the settings: the SD card reported an error."},
]
HOME_ERRORS = [dict(e, detail=str(len(ERRORS)), enabled=True) if e["label"] == "ERRORS" else e for e in HOME]
# A board with a Bluetooth gamepad has a fourth card; the name is made up.
PAD = "8BitDo SN30 Pro"
HOME_GAMEPAD = HOME + [{"icon": "gamepad", "label": "WIRELESS GAMEPAD", "detail": "ON", "enabled": True}]

TOP = [
    {"kind": "dir", "name": "gb", "count": 31},
    {"kind": "dir", "name": "gbc", "count": 9},
    {"kind": "dir", "name": "gba", "count": 6},
    {"kind": "dir", "name": "hacks", "count": 2},
]

RPG = [
    {"kind": "dir", "name": "jp", "count": 3},
    {"kind": "rom", "name": "Final Fantasy Legend II", "machine": "gb", "save": True,
     "detail": "SAGA2  MBC2"},
    {"kind": "rom", "name": "Pokemon Gold", "machine": "gbc", "save": True,
     "detail": "POKEMON_GLD  MBC3+RTC  RAM 32K"},
    {"kind": "rom", "name": "Pokemon Yellow", "machine": "gb", "save": True,
     "detail": "POKEMON YELLOW  MBC5  RAM 32K"},
    {"kind": "rom", "name": "精灵宝可梦 水晶版", "machine": "gbc", "save": True,
     "detail": "PM_CRYSTAL  MBC3+RTC  RAM 32K"},
    {"kind": "rom", "name": "Legend of Zelda, The - Link's Awakening DX (USA, Europe) (Rev 2)",
     "machine": "gbc", "save": True, "detail": "ZELDA  MBC5  RAM 8K"},
    {"kind": "rom", "name": "ポケットモンスター 赤", "machine": "gb", "detail": "POKEMON RED  MBC3  RAM 32K"},
    {"kind": "rom", "name": "포켓몬스터 골드", "machine": "gbc", "detail": "POKEMON_GLD  MBC3+RTC  RAM 32K"},
    {"kind": "rom", "name": "Tetris", "machine": "gb", "detail": "TETRIS  NO MBC"},
    {"kind": "rom", "name": "Advance Wars", "machine": "gba", "save": True, "detail": "ADVANCEWARS  AWRE"},
    {"kind": "rom", "name": "Kirby Tilt 'n' Tumble", "machine": "gbc", "enabled": False,
     "detail": "MBC7 cartridges cannot run yet."},
    {"kind": "rom", "name": "เกม Demo", "machine": "gb", "detail": "DEMO  NO MBC"},
]


def setting_items(in_game, pc=False, smooth=True, scaling=None, palette=0, color=None, fix=0):
    """`color` is how the game in progress runs: False in GB mode, True on a
    GBC; None from the main screen."""
    if scaling is None:
        scaling = 1 if smooth else 0
    if color is None:
        palette_note = "GB colors, or COLOR to play on a GBC."
    elif color:
        palette_note = "Set by the game: it runs on a GBC."
    else:
        palette_note = "Pick COLOR before a game starts."
    return [
        {"label": "VOLUME", "kind": "level", "value": 70, "enabled": True,
         "note": "Speaker and headphone volume."},
        {"label": "BRIGHTNESS", "kind": "level", "value": 80, "enabled": not pc, "unsupported": pc,
         "note": "No backlight control on PC." if pc else "Screen backlight."},
        {"label": "CPU", "kind": "choice", "choices": ["INT", "JIT"], "value": 0 if pc else 1, "enabled": not pc,
         "note": "JIT is not available on PC." if pc else "INT interprets, JIT compiles."},
        {"label": "PALETTE", "kind": "choice", "choices": ["GRAY", "GREEN", "COLOR"], "value": palette,
         "enabled": not color, "off": [2] if color is False else [],
         "swatches": ["gray", "green", "color"], "note": palette_note},
        {"label": "COLOR FIX", "kind": "choice", "choices": ["OFF", "ON"], "value": fix,
         "enabled": color is not False,
         "note": "Only for games in color." if color is False else "Colors as a GBC screen shows them."},
        {"label": "SCALING", "kind": "choice", "choices": ["NEAREST", "SMOOTH"], "value": scaling,
         "enabled": smooth,
         "note": "This platform uses NEAREST only." if not smooth else
                 "Sampling changes on resume." if in_game else "Game image sampling."},
    ]


PAUSE_ITEMS = [
    {"label": "RESUME", "enabled": True},
    {"label": "SAVE / LOAD", "enabled": True},
    {"label": "SETTINGS", "enabled": True},
    {"label": "ERRORS", "enabled": False},
    {"label": "QUIT", "enabled": True},
]
PAUSE_ERRORS = [dict(e, detail=str(len(ERRORS)), enabled=True) if e["label"] == "ERRORS" else e
                for e in PAUSE_ITEMS]
PAUSE_SLOT_WRITING = [dict(e, detail="50%", tone="dirty") if e["label"] == "SAVE / LOAD" else e
                      for e in PAUSE_ITEMS]

# The paused game and the slot thumbnails are mock frames: the preview scene,
# mirrored or shifted so that the slots differ. None of them is emulator output.
FRAME = scene("gb")
MOCK = [FRAME.transpose(Image.Transpose.FLIP_LEFT_RIGHT), ImageChops.offset(FRAME, 56, 0),
        ImageChops.offset(FRAME.transpose(Image.Transpose.FLIP_LEFT_RIGHT), 90, 0)]
COLOR_FRAME = scene("gbc")
COLOR_MOCK = [COLOR_FRAME.transpose(Image.Transpose.FLIP_LEFT_RIGHT), ImageChops.offset(COLOR_FRAME, 56, 0)]

SLOTS = [
    {"state": "saved", "date": "2026-10-19 14:32", "frame": MOCK[0]},
    {"state": "saved", "date": "2026-10-17 21:05", "frame": MOCK[1]},
    {"state": "empty"},
    {"state": "damaged"},
]
SLOTS_OTHER = [
    {"state": "saved", "date": "2026-10-19 14:32", "frame": MOCK[0]},
    {"state": "rom", "date": "2026-09-02 08:47", "frame": MOCK[2]},
    {"state": "version"},
    {"state": "saved", "date": None, "frame": MOCK[1]},
]
# Pokemon Gold with PALETTE at COLOR: SLOT 2 was saved in GB mode.
SLOTS_MODE = [
    {"state": "saved", "date": "2026-10-21 20:14", "frame": COLOR_MOCK[0]},
    {"state": "mode", "date": "2026-10-02 18:40", "frame": MOCK[1]},
    {"state": "saved", "date": "2026-10-20 07:55", "frame": COLOR_MOCK[1]},
    {"state": "empty"},
]
SLOTS_WRITING = [dict(SLOTS[2], state="saved", frame=FRAME, progress=40) if i == 2 else s
                 for i, s in enumerate(SLOTS)]
GBA_FRAME = scene("gba")
SLOTS_GBA = [
    {"state": "saved", "date": "2026-10-23 22:10", "frame": GBA_FRAME.transpose(Image.Transpose.FLIP_LEFT_RIGHT)},
    {"state": "saved", "date": "2026-10-22 19:48", "frame": ImageChops.offset(GBA_FRAME, 84, 0)},
    {"state": "empty"},
    {"state": "empty"},
]


def screens():
    """(name, 480x320 image) for every screen and dialog in the previews."""
    out = []
    home_screen = home(HOME, 0, "SD 48 ROMS", "Last played: Pokemon Yellow")
    out.append(("home", home_screen))
    out.append(("home-settings", home(HOME, 1, "SD 48 ROMS", "Sound, display and emulation settings.")))
    out.append(("home-errors", home(HOME_ERRORS, 2, "SD 48 ROMS", "Newest: " + ERRORS[0]["text"])))
    out.append(("roms-top", rom_list("/", TOP, 0, note="gb/  31 ROMs")))
    rpg = rom_list("/gb/rpg", RPG, 3)
    out.append(("roms", rpg))
    out.append(("roms-gbc", rom_list("/gb/rpg", RPG, 4)))
    out.append(("roms-unsupported", rom_list("/gb/rpg", RPG, 10, first=4, note="MBC7 cartridges cannot run yet.",
                                             note_alert=True)))
    out.append(("roms-long", rom_list("/gb/rpg", RPG, 5, first=1)))
    out.append(("roms-missing-glyph", rom_list("/gb/rpg", RPG, 11, first=5)))
    out.append(("roms-scanning", rom_list("/", TOP[:2], 0, status="SCANNING", note="gb/  31 ROMs")))
    out.append(("roms-no-card", message("ROMS", "card", "NO SD CARD",
                                        "Insert a card with your games in a roms folder.")))
    out.append(("roms-empty", message("ROMS", "cartridge", "NO ROMS",
                                      "Put .gb, .gbc or .gba files in the roms folder on the card.")))
    out.append(("settings", settings(setting_items(False), 0, False)))
    out.append(("settings-palette", settings(setting_items(False, palette=2), 3, False)))
    out.append(("settings-color-fix", settings(setting_items(False, palette=2, fix=1), 4, False)))
    out.append(("settings-game", settings(setting_items(True, color=False), 2, True)))
    out.append(("settings-game-palette", settings(setting_items(True, palette=1, color=False), 3, True)))
    out.append(("settings-game-color", settings(setting_items(True, palette=2, color=True, fix=1), 3, True)))
    out.append(("settings-pc", settings(setting_items(False, pc=True), 1, False)))
    out.append(("settings-scaling", settings(setting_items(True, scaling=1, color=False), 5, True)))
    out.append(("settings-scaling-unavailable", settings(setting_items(False, smooth=False), 5, False)))
    out.append(("pause", pause(FRAME, PAUSE_ITEMS, 0, "CLEAN")))
    out.append(("pause-writing", pause(FRAME, PAUSE_ITEMS, 4, "WRITING")))
    retry = [{"label": "RETRY SAVE", "enabled": True}] + PAUSE_ERRORS
    out.append(("pause-error", pause(FRAME, retry, 0, "ERROR")))
    out.append(("errors", errors(ERRORS, 0)))
    out.append(("loading", loading("Pokemon Yellow.gb", 60)))
    out.append(("dialog-load", dialog(rpg, "CAN'T LOAD", ["Tetris DX.gbc", "The file is larger than 8 MiB."],
                                      ["OK"], tone="alert")))
    out.append(("dialog-mismatch", dialog(rpg, "SAVE FROM ANOTHER ROM", [
        ("SAVE", "POKEMON RED"), ("ROM", "POKEMON BLUE"),
        "START NEW keeps the old save as .sav.old."], ["LOAD SAVE", "START NEW"])))
    out.append(("dialog-damaged", dialog(rpg, "SAVE DAMAGED", [
        "The save fails its check, so the game starts with empty save memory.",
        "A copy of the old save is kept as .sav.old."], ["OK"], tone="alert")))
    out.append(("dialog-clock", dialog(loading("Pokemon Gold.gbc", 100), "CLOCK NOT SET", [
        "The board's clock is not set, so the game's clock did not move on while the power was off."],
        ["OK"])))
    quitting = pause(FRAME, PAUSE_ITEMS, 4, "WRITING")
    out.append(("dialog-saving", dialog(quitting, "SAVING", ["Writing the save before quitting."], progress=40)))
    out.append(("dialog-save-failed", dialog(quitting, "SAVE FAILED", [
        "The save could not be written: the SD card is full."], ["RETRY", "QUIT WITHOUT SAVING"], tone="alert")))
    out += slot_screens()
    out += gamepad_screens()
    return out


def gamepad_screens():
    """HOME's WIRELESS GAMEPAD card and its page, on a board with a Bluetooth gamepad."""
    return [
        ("home-gamepad", home(HOME_GAMEPAD, 3, "SD 48 ROMS", f"Connected: {PAD}")),
        ("gamepad", gamepad("CONNECTED", PAD, 0)),
        ("gamepad-none", gamepad("NO GAMEPAD", "PAIR finds a Bluetooth gamepad in pairing mode.", 0)),
        ("gamepad-searching", gamepad("SEARCHING", "Put the gamepad in pairing mode: hold its pair button.", 0,
                                      pairing=True)),
        ("gamepad-paired", gamepad("PAIRED", PAD, 0)),
        ("gamepad-failed", gamepad("NOT PAIRED", "No gamepad in pairing mode was found within a minute.", 0,
                                   alert=True)),
    ]


def gamepad_home_screens():
    """(name, image) for the HOME a board with a Bluetooth gamepad shows: four cards."""
    return [("home", home(HOME_GAMEPAD, 0, "SD 48 ROMS", "Last played: Pokemon Yellow"))]


def small_window_screens():
    """(name, image) for the screens only a window too small for 2x frames
    shows: SETTINGS without SCALING."""
    return [("settings-no-scaling", settings(setting_items(False)[:-1], 0, False))]


def slot_screens():
    """SAVE / LOAD from the pause menu, for Pokemon Yellow, which has a battery."""
    out = []
    rom = "Pokemon Yellow"
    title = "POKEMON YELLOW"
    out.append(("slots", slots(rom, SLOTS, 0, title)))
    out.append(("slots-empty", slots(rom, SLOTS, 2, "Nothing is saved in SLOT 3.")))
    out.append(("slots-damaged", slots(rom, SLOTS, 3, "Can't load: it fails its check.", "alert")))
    out.append(("slots-rom", slots(rom, SLOTS_OTHER, 1, "From another ROM: POKEMON RED.", "dirty")))
    out.append(("slots-version", slots(rom, SLOTS_OTHER, 2, "Can't load: saved by another version.", "alert")))
    out.append(("slots-time-unknown", slots(rom, SLOTS_OTHER, 3, "Saved with the board's clock not set.")))
    gold_rom = "Pokemon Gold"
    out.append(("slots-color", slots(gold_rom, SLOTS_MODE, 0, "POKEMON_GLD")))
    out.append(("slots-mode", slots(gold_rom, SLOTS_MODE, 1, "Can't load: PALETTE was not COLOR.", "alert")))
    out.append(("slots-gba", slots("Advance Wars", SLOTS_GBA, 0, "ADVANCEWARS", wide=True)))
    out.append(("slots-no-card", message("SAVE / LOAD", "card", "NO SD CARD",
                                         "Insert the card to save to or load from a slot.")))
    acting = slots(rom, SLOTS, 0, "Save the game now over SLOT 1.", button=0)
    out.append(("slot-actions", acting))
    loading_slot = slots(rom, SLOTS, 0, "Go back to the game saved in SLOT 1.", button=1)
    out.append(("slot-actions-load", loading_slot))
    writing = slots(rom, SLOTS_WRITING, 2, "Writing SLOT 3; the game can resume.")
    out.append(("slot-writing", writing))
    out.append(("pause-slot-writing", pause(FRAME, PAUSE_SLOT_WRITING, 0, "CLEAN")))
    out.append(("dialog-slot-overwrite", dialog(acting, "REPLACE SLOT 1", [
        "SLOT 1 already holds a save."], ["REPLACE", "CANCEL"], selected=1)))
    out.append(("dialog-slot-rom", dialog(
        slots(rom, SLOTS_OTHER, 1, "Go back to the game saved in SLOT 2.", button=1), "SLOT FROM ANOTHER ROM", [
            ("SLOT", "POKEMON RED"), ("ROM", title),
            "The game may not run right."],
        ["LOAD", "CANCEL"], selected=1)))
    gold = slots("Pokemon Gold", SLOTS, 0, "Go back to the game saved in SLOT 1.", button=1)
    out.append(("dialog-slot-clock", dialog(gold, "CLOCK NOT SET", [
        "The board's clock is not set, so the game's clock did not move on since SLOT 1 was saved."], ["OK"])))
    out.append(("dialog-slot-failed", dialog(writing, "SLOT 3 NOT SAVED", [
        "SLOT 3 could not be written: the SD card is full."], ["OK"], tone="alert")))
    quitting = pause(FRAME, PAUSE_SLOT_WRITING, 4, "CLEAN")
    out.append(("dialog-slot-saving", dialog(quitting, "SAVING", ["Writing SLOT 3 before quitting."], progress=60)))
    return out

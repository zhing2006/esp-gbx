# esp-gbx

English | [简体中文](README_CN.md)

![esp-gbx on eight boards](docs/boards/esp-gbx-boards.png)

esp-gbx is a GB / GBC / GBA emulator for ESP32-P4, ESP32-S31 and ESP32-C5. The emulation core, the interpreter, the RISC-V JIT and the device models are written from scratch from hardware documentation such as [Pan Docs](https://gbdev.io/pandocs/) and [GBATEK](https://problemkaputt.de/gbatek.htm), without code from existing emulators. Games load straight from the SD card; nothing has to be converted on a PC first.

The firmware has:

- An interpreter and a JIT that translates SM83 and ARM7TDMI code to RISC-V. The interpreter is the correctness baseline and the JIT's fallback, and the CPU setting switches between them.
- Cartridge saves, the clock of MBC3 and GBA cartridges with an RTC, and four instant-save slots per game. Cartridge saves are written in turn to two files, so a power cut while writing leaves the previous save intact.
- A home screen and a pause menu. The settings cover volume, backlight, execution backend, palette, GBC colour correction and scaling, and are kept on the card.
- A touch UI drawn as a handheld, with on-screen buttons and a speed lever for 1X and 2X; 2X mutes the sound. Boards with the ports for them also take USB HID gamepads and Bluetooth LE HID gamepads.

Tests run on the board, on the interpreter and on the JIT alike, with [SingleStepTests](https://github.com/SingleStepTests) and third-party test ROMs, each judged by its suite's own criteria; see [docs/hardware-testing.md](docs/hardware-testing.md). The PC build in `host/` runs the same menus with the interpreter only, for development and debugging.

## Systems

| Chip | Systems | Execution backends |
|---|---|---|
| ESP32-P4 | GB, GBC, GBA | interpreter, SM83 JIT, ARM JIT |
| ESP32-S31 | GBA | interpreter, ARM JIT |
| ESP32-C5 | GB, GBC | interpreter, SM83 JIT |
| PC (`host/`) | GB, GBC, GBA | interpreter |

A firmware builds in only the systems whose execution core fits in the chip's internal RAM. The ESP32-S31 cannot hold the GB's and the GBA's at once, and both S31 boards build the GBA alone. Its GBA interpreter also stays out of internal RAM, which goes to the JIT's code pool instead, so the interpreter backend runs slower there. The ESP32-C5 has one core and 384 KB of SRAM, so it builds the GB and the GBC. On the C5 the game page's controls are drawn once at startup and, apart from the speed lever, no longer change as they are touched, and the page has no VFD. The SPI screen takes at most one game frame every 25 ms, which at full speed is every other frame.

Monochrome games and games with the CGB flag 0x80 run on both a GB and a GBC, and the PALETTE setting decides which of the two they run as. Games with the CGB flag 0xC0 always run in CGB mode.

GBA games need a BIOS dumped from a console, at `bios/gba_bios.bin` on the card. The emulator checks it by checksum and does not start GBA games without a valid one.

## SD card

The card uses FAT, with long file names in UTF-8.

| Path on the card | Contents |
|---|---|
| `roms/` | Game ROMs, put there by the user, in any subfolders |
| `bios/` | `gba_bios.bin`, the GBA BIOS dumped from a console; the emulator only reads it |
| `saves/` | Cartridge saves, written by the emulator |
| `states/` | Instant saves, written by the emulator |
| `config/` | Settings, one file per board, such as `config/sila_p4.ini`; the PC build uses `config/pc.ini` |
| `tests/` | Test data for the test firmware, see [docs/hardware-testing.md](docs/hardware-testing.md) |

The menu scans `roms/` recursively and skips files and folders whose names start with `.`, such as the `._` files macOS writes to FAT cards. The emulator tells the system from the ROM header, not from the folder or the file extension.

Saves follow the folders under `roms/`. Their names are the ROM's file name without the extension, followed by a number and a suffix:

| ROM | Cartridge saves | Instant saves |
|---|---|---|
| `roms/gb/Tetris.gb` | `saves/gb/Tetris.0.sav`, `saves/gb/Tetris.1.sav` | `states/gb/Tetris.1.state` to `Tetris.4.state` |

The two cartridge save files are written in turn. A game without a battery-backed save gets no save file. The number of an instant save is its slot. Because saves are named after the ROM's path, renaming or moving a ROM means renaming its saves too. Both kinds of save record the ROM header's identity, and the emulator asks before loading a save made by another ROM.

The settings file holds one `key=value` per line. Unknown keys are ignored, and missing or invalid values take their defaults, so different firmware versions can share one file. Each board has its own file because the boards support different features.

The full description of the card is in [docs/product-design.md](docs/product-design.md#sd-卡目录).

## Boards

| Board | Chip | Screen | Systems | Sound | Gamepads | RTC |
|---|---|---|---|---|---|---|
| [M5Stack Tab5](docs/boards/m5stack_tab5.md) | ESP32-P4 v1.3, 360 MHz | 5″ 720×1280 MIPI DSI | GB, GBC, GBA | speaker, stereo headphones | USB, Bluetooth LE | RX8130CE |
| [Waiken-Smart ESP32-S31 CB](docs/boards/wks_s31_cb.md) | ESP32-S31, 320 MHz | 4.3″ 800×480 RGB | GBA | two speakers | USB | none |
| [Waveshare ESP32-C5-Touch-LCD-2.8](docs/boards/waveshare_c5_lcd28.md) | ESP32-C5, 240 MHz | 2.8″ 240×320 SPI | GB, GBC | speaker | none | PCF85063A |
| [GUITION JC1060P470](docs/boards/jc1060p470.md) | ESP32-P4 v1.0, 360 MHz | 7″ 1024×600 MIPI DSI | GB, GBC, GBA | speaker | Bluetooth LE | RX8025T |
| [OSPTEK ESP32-P4C5-Module](docs/boards/osptek_p4c5.md) | ESP32-P4 v3.2, 400 MHz | 4.3″ 480×800 MIPI DSI | GB, GBC, GBA | speaker | USB, Bluetooth LE | none |
| [Sila P4](docs/boards/sila_p4.md) | ESP32-P4 v1.3, 360 MHz | 720×720 MIPI DSI | GB, GBC, GBA | speaker | none | none |
| [Sila P4X AIBox](docs/boards/sila_p4x.md) | ESP32-P4 v3.2, 400 MHz | 6.2″ 452×1280 MIPI DSI | GB, GBC, GBA | two speakers | Bluetooth LE | none |
| [Espressif ESP32-S31-Korvo-1](docs/boards/esp32_s31_korvo_1.md) | ESP32-S31, 320 MHz | 4.3″ 800×480 RGB | GBA | two speakers | USB | none |

Every board drives its screen, touch, sound and SD card. The chip revision is that of the board at hand. A P4 build is tied to the revision: v1.x and v3.x need different builds, and v1.x runs at up to 360 MHz. On a board with an RTC, the cartridge clocks catch up on the time the board was off, and instant saves record when they were made. A single speaker gets the mix of both channels. The pages under [docs/boards/](docs/boards/) hold each board's sources, pins and initialization constraints; [docs/ui-layouts/](docs/ui-layouts/index.md) shows each board's layout.

### M5Stack Tab5

Preset `m5stack_tab5`. ESP32-P4 v1.3 with 32 MB of PSRAM. The 5″ 720×1280 MIPI DSI screen is that of the Tab5 with the ST7121 driver; the board is held upright, with ST7123 touch. The default layout has a 720×480 game window that shows GBA games at 3x; turning off the `GBX_TAB5_X3_LAYOUT` option gives a 480×320 window at 2x. The ES8388 feeds the speaker its left channel only. When headphones are in, the firmware switches the speaker off and the headphones get stereo. The USB-A port takes a HID gamepad, and the ESP32-C6 on the board connects Bluetooth LE gamepads. The RX8130CE is the board's clock.

### Waiken-Smart ESP32-S31 CB V1.1

Preset `wks_s31_cb`. ESP32-S31-WROOM-3-N16R16V with 16 MB of octal PSRAM. The screen is the same vendor's 4.3″ 800×480 RGB touch screen WKS43WV067-WCT, turned to portrait, with GT1151Q touch and PWM backlight dimming. The ES8389 drives two speakers in stereo. The USB-A port takes a HID gamepad. The firmware builds the GBA alone.

### Waveshare ESP32-C5-Touch-LCD-2.8

Preset `waveshare_c5_lcd28`. ESP32-C5 in the ESP32-C5-WROOM-1-N32R8 module, with 8 MB of quad PSRAM. The 2.8″ 240×320 SPI screen has an ST7789V2 driver and CST3530 touch, and its backlight is dimmed through the CH32V003 IO expander. The SD card shares the screen's SPI bus. The ES8311 drives one speaker, and the PCF85063A is the board's clock. The firmware builds the GB and the GBC, and has no gamepad support on this board.

### GUITION JC1060P470

Preset `jc1060p470`. ESP32-P4 v1.0 with 32 MB of PSRAM. The 7″ 1024×600 IPS MIPI DSI screen has a JD9165 driver and GT911 touch; the board is held upright, so the UI is turned by 270 degrees. The ES8311 drives one speaker, and the RX8025T is the board's clock. The ESP32-C6 on the board connects Bluetooth LE gamepads.

### OSPTEK ESP32-P4C5-Module

Preset `osptek_p4c5`. OSPTEK's base board with the ESP32-P4C5 core board, ESP32-P4 v3.2 with 32 MB of PSRAM. The screen is OSPTEK's 4.3″ 480×800 YDP430BT009-V1 with an ST7102 driver and ST7123 touch, connected through OSPTEK's 4.3″ adapter board to the 15-pin DSI connector on the back of the base board. **Backlight dimming needs a jumper wire**; without it the backlight stays fully on. See the board page. The ES8311 drives one speaker. The USB-A port takes a HID gamepad, and the ESP32-C5 on the core board connects Bluetooth LE gamepads.

### Sila P4

Preset `sila_p4`. ESP32-P4 v1.3 with 32 MB of PSRAM. The 720×720 MIPI DSI screen has an ST7703 driver and GT911 touch. The ES8311 drives one speaker. The firmware has no gamepad support on this board: with the board powered from the serial port, a gamepad on the other USB-C port gets no power and the P4 does not see it.

### Sila P4X AIBox

Preset `sila_p4x`. ESP32-P4 v3.2 with 32 MB of PSRAM and 32 MB of flash. The 6.2″ 452×1280 module with an AXS15260D, which handles both display and touch, is used in landscape. The ES8389 drives two speakers in stereo. The ESP32-C5 on the board connects Bluetooth LE gamepads, over a 1-bit SDIO bus. **On the board at hand, R26 in the backlight circuit measures about 1 Ω instead of the schematic's 10 Ω. At full duty the module's ribbon heats up and the screen darkens from that end, and with the full firmware running the board loses power, so the firmware caps full brightness at 5% duty.** See the board page before changing it.

### Espressif ESP32-S31-Korvo-1

Preset `esp32_s31_korvo_1`. ESP32-S31-WROOM-3 with 16 MB of octal PSRAM. The kit's screen is the ESP32-S3-LCD-EV-Board-SUB3, a 4.3″ 800×480 RGB screen with an ST7262E43 driver and GT1151 touch, turned to portrait. Its backlight is on whenever the board has power and cannot be dimmed, so BRIGHTNESS is unavailable. The ES8389 drives two speakers in stereo. The USB-A port takes a HID gamepad. The firmware builds the GBA alone.

## Building

The PNG, JPEG and PDF files are kept in Git LFS: install Git LFS and run `git lfs install` before cloning.

The firmware builds with ESP-IDF v6.1, installed with EIM, the ESP-IDF Installation Manager, and needs [uv](https://docs.astral.sh/uv/) to pack the UI assets. Presets in `CMakePresets.json` select the board and the build type, such as `sila_p4-release` or `sila_p4-debug`:

```
eim run "idf.py --preset sila_p4-release build" v6.1
eim run "idf.py --preset sila_p4-release -p <port> -b 460800 flash monitor" v6.1
```

ESP32-S31 is a preview target in ESP-IDF v6.1. Before the first build in an S31 preset's build directory, select the target with `idf.py --preset wks_s31_cb-release --preview set-target esp32s31`, also through `eim run`; after that, builds need no `--preview`. The PC build is the CMake project in `host/`:

```
cd host
cmake --preset debug
cmake --build --preset debug
```

The build setup and the module boundaries are in [docs/architecture.md](docs/architecture.md), and the steps for a new board in [docs/adding-a-board.md](docs/adding-a-board.md).

## License

[Apache License 2.0](LICENSE)

The menus use [Fusion Pixel Font](https://github.com/TakWolf/fusion-pixel-font) 2026.10.16, licensed under the [SIL Open Font License 1.1](raws/ui/common/fonts/OFL.txt). Its source and the licenses of the fonts it draws on are in `raws/ui/common/fonts/`.

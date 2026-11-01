# Changelog

Each version has a section whose heading is the version's tag, such as `## v0.1.0`. When the tag is pushed, GitHub Actions builds the release firmware of every board and publishes a GitHub release with the section's text as its notes; see `.github/workflows/release.yml`.

## v0.1.0

The first release, for these chips, systems and boards:

| Chip | Systems | Boards |
|---|---|---|
| ESP32-P4 | GB, GBC, GBA | M5Stack Tab5, GUITION JC1060P470, OSPTEK ESP32-P4C5-Module, Sila P4, Sila P4X AIBox |
| ESP32-S31 | GBA | Waiken-Smart ESP32-S31 CB V1.1, Espressif ESP32-S31-Korvo-1 |
| ESP32-C5 | GB, GBC | Waveshare ESP32-C5-Touch-LCD-2.8 |

The firmware has:

- An interpreter and a RISC-V JIT; the CPU setting switches between them.
- Cartridge saves, the clock of MBC3 and GBA cartridges with an RTC, and four instant-save slots per game.
- A home screen, a pause menu and settings, which are kept on the SD card.
- A touch UI with on-screen buttons and a speed lever for 1X and 2X. Boards with the ports for them also take USB HID gamepads, tested with the PS5 DualSense controller, or Bluetooth LE HID gamepads, tested with the Xbox Wireless Controller of the Xbox Series X|S; the README lists which board takes which.

Each board has one complete firmware, `esp-gbx-v0.1.0-<board>.bin`, with the bootloader, the partition table, the app and the UI assets, written at 0x0; see [Flashing a release](https://github.com/zhing2006/esp-gbx/blob/v0.1.0/README.md#flashing-a-release) in the README. A P4 firmware runs only on the chip revisions it was built for: those of `jc1060p470`, `m5stack_tab5` and `sila_p4` on v1.x, those of `osptek_p4c5` and `sila_p4x` on v3.x. `SHA256SUMS` lists the SHA-256 of each firmware.

GBA games need a BIOS dumped from a console, at `bios/gba_bios.bin` on the SD card.

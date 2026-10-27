#!/usr/bin/env bash
# Download reference docs and test suites into refs/. Existing entries are skipped;
# to update one, delete its directory or file and rerun.
set -euo pipefail
cd "$(dirname "$0")"

snapshot() {
  local dir=$1 name=$2 url=$3
  if [ -e "$dir" ]; then echo "skip $dir"; return; fi
  echo "fetch $name -> $dir"
  rm -rf "$dir.tmp" && mkdir -p "$dir.tmp"
  curl -fsSL "$url" | tar -xz --strip-components=1 -C "$dir.tmp"
  mv "$dir.tmp" "$dir"
}

# GitHub repo snapshot pinned to a commit
tarball() { snapshot "$1" "$2@${3:0:7}" "https://codeload.github.com/$2/tar.gz/$3"; }

# Codeberg repo snapshot pinned to a commit
codeberg() { snapshot "$1" "codeberg:$2@${3:0:7}" "https://codeberg.org/$2/archive/$3.tar.gz"; }

file() {
  local path=$1 url=$2
  if [ -e "$path" ]; then echo "skip $path"; return; fi
  echo "fetch $url -> $path"
  mkdir -p "$(dirname "$path")"
  curl -fsSL -o "$path.tmp" "$url"
  mv "$path.tmp" "$path"
}

# A page from the Wayback Machine, which serves it gzip-encoded
page() {
  local path=$1 url=$2
  if [ -e "$path" ]; then echo "skip $path"; return; fi
  echo "fetch $url -> $path"
  mkdir -p "$(dirname "$path")"
  curl -fsSL --compressed -o "$path.tmp" "$url"
  mv "$path.tmp" "$path"
}

archive() {
  local dir=$1 url=$2
  if [ -e "$dir" ]; then echo "skip $dir"; return; fi
  echo "fetch $url -> $dir"
  rm -rf "$dir.tmp" && mkdir -p "$dir.tmp"
  curl -fsSL -o "$dir.tmp/archive.zip" "$url"
  unzip -q "$dir.tmp/archive.zip" -d "$dir.tmp"
  rm "$dir.tmp/archive.zip"
  mv "$dir.tmp" "$dir"
}

# Docs
tarball docs/riscv-isa riscv/riscv-isa-manual 6ee57d8997021aaa8025e208164763fe12e03c19
tarball docs/riscv-psabi riscv-non-isa/riscv-elf-psabi-doc 74936a9e25dec3c5b297b19e4c37e97d14868b22
tarball docs/pandocs gbdev/pandocs 0191af06ac49661587dcde3d57a241a626b8df75
tarball docs/gbedg   Ashiepaws/GBEDG 97f198d330a51be558aa8fc9f3f0760846d02d95
file docs/gbctr.pdf     https://gekkio.fi/files/gb-docs/gbctr.pdf
file docs/Opcodes.json  https://raw.githubusercontent.com/gbdev/gb-opcodes/376f61c86fdac2048f7ce5fe838ae756b306017e/Opcodes.json
file docs/gbz80.7       https://raw.githubusercontent.com/gbdev/rgbds/6818759a8ca8cd11bf092aa052e2a992b52ac8e5/man/gbz80.7
file docs/tcagbd.pdf    https://raw.githubusercontent.com/AntonioND/giibiiadvance/ccb40c3cf7d9efec538858a36d4ab69bb4d0ce5c/docs/TCAGBD.pdf
file docs/gb-sound-hardware.html https://gbdev.gg8.se/wiki/articles/Gameboy_sound_hardware
file docs/gbatek.htm    https://problemkaputt.de/gbatek.htm
# TCRF revision 2183057, as archived on 2026-09-18
page docs/tcrf-cgb-boot-rom.html https://web.archive.org/web/20260918011510id_/https://tcrf.net/Notes:Game_Boy_Color_Bootstrap_ROM
# Measurements of one GBC screen by Pokefan531, Handheld Color Space Project at
# 66b7f4c, for tools/lcd_colors.py
GBC_LCD=https://raw.githubusercontent.com/Brankale/Handheld-Color-Space-Project/66b7f4c01c8528b07bed8e2cec5a5023327cd315/handhelds/Nintendo%20GBC/%5B2026-05-13%5D%5BPokefan531%5D/measurements
file "docs/gbc-lcd/README.md" "$GBC_LCD/README.md"
file "docs/gbc-lcd/gbc-color1 2026-05-13.ods" "$GBC_LCD/gbc-color1%202026-05-13.ods"
file "docs/gbc-lcd/gbc-color2 2026-05-13.ods" "$GBC_LCD/gbc-color2%202026-05-13.ods"
file "docs/gbc-lcd/gbc-gamma1 2026-05-09.txt" "$GBC_LCD/gbc-gamma1%202026-05-09.txt"
file "docs/gbc-lcd/gbc-gamma2 2026-05-09.txt" "$GBC_LCD/gbc-gamma2%202026-05-09.txt"

# Test suites: GB / GBC
archive tests/game-boy-test-roms https://github.com/c-sp/game-boy-test-roms/releases/download/v7.0/game-boy-test-roms-v7.0.zip
tarball tests/mooneye-source Gekkio/mooneye-test-suite 8d742b9d55055f6878a2f3017e0ccf2234cd692c
tarball tests/sm83           SingleStepTests/sm83 f9c30210245dd691661db39f5ace022c465ecc2f
tarball tests/gameboy-doctor robert/gameboy-doctor cc742777af7d71de3061c65733ecfa250237012b
tarball tests/same-suite-source LIJI32/SameSuite f71b4b3c3773b771f94955f5081dd2b36aabe54d
tarball tests/age-source c-sp/age-test-roms cd3f654d13bf7137fa4f7fb7e5dd041d10f7098e
# Gambatte's test runner, for the digit patterns its tests display
file tests/gambatte-testrunner/testrunner.cpp https://raw.githubusercontent.com/pokemon-speedrunning/gambatte-core/d819bad196942db6bc31fd4196b911584c415594/test/testrunner.cpp

# Test suites: GBA
tarball  tests/arm7tdmi          SingleStepTests/ARM7TDMI e3097d88d428752736b949d94c094d5da50f0db6
tarball  tests/jsmolka-gba-tests jsmolka/gba-tests a7113b67e63f83a9b321696ddd7042ccfad6c881
tarball  tests/alyosha-gba-tests alyosha-tas/gba-tests 66f5f1d60cd7030b600606886c76f5c821ecaf9d
codeberg tests/nba-hw-test       nba-emu/hw-test fbc99140e06f083c0a47612467cfbb02470e56dc

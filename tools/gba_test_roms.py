"""Build the GBX GBA test ROMs from tests/gba/roms/.

    python3 tools/gba_test_roms.py OUTPUT_DIR [--toolchain DIR]

The ROMs are built with Arm GNU Toolchain 15.3.Rel2 (arm-none-eabi), the
version pinned in TOOLCHAIN_VERSION. --toolchain takes the toolchain's root or
bin directory; without it the compilers are looked up in ARM_GNU_TOOLCHAIN
and then in PATH. The version is checked before building, because a different
compiler gives different ROM bytes and the data manifest records them.

Download the toolchain from Arm (https://developer.arm.com/downloads/-/arm-gnu-toolchain-downloads),
for example:
  macOS (Apple silicon): arm-gnu-toolchain-15.3.rel2-darwin-arm64-arm-none-eabi.tar.xz
  Windows:               arm-gnu-toolchain-15.3.rel2-mingw-w64-x86_64-arm-none-eabi.zip
and unpack it anywhere; nothing has to be installed.

Output layout (relative to OUTPUT_DIR):
  ppu/<scene>.gba      PPU scenes from tests/gba/roms/ppu/scenes.py
  save/*.gba           EEPROM, SRAM and Flash tests
  rtc/rtc.gba          cartridge RTC test, game code 0RTC
  rom/rom32_*.gba      32 MiB ROM reads by the CPU and by DMA

The ROM header carries no Nintendo logo (see tests/gba/roms/common/crt0.s).
Game codes start with a digit; GBATEK "GBA Cartridge Header" lists only
letters as the first character of the codes Nintendo assigns.
"""

import argparse
import importlib.util
import os
import shutil
import struct
import subprocess
import sys
import tempfile
from array import array
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SRC = ROOT / 'tests/gba/roms'
TOOLCHAIN_VERSION = 'Arm GNU Toolchain 15.3.Rel2'
CFLAGS = ['-mcpu=arm7tdmi', '-marm', '-O2', '-std=gnu23', '-ffreestanding', '-fno-builtin',
          '-fno-strict-aliasing', '-nostdlib', '-Wall', '-Wextra', '-Werror']
ROM32_SIZE = 32 << 20
HIGH_BASE = 0x09F00000


def rom(path, title, code, sources, defines=(), scene=None, rom32=False):
    return {'path': path, 'title': title, 'code': code, 'sources': sources, 'defines': list(defines),
            'scene': scene, 'rom32': rom32}


def load_scenes():
    sys.path.insert(0, str(SRC / 'ppu'))
    spec = importlib.util.spec_from_file_location('gbx_scenes', SRC / 'ppu/scenes.py')
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def roms():
    scenes = load_scenes().SCENES
    out = [rom(f'ppu/{name}.gba', 'GBX PPU', '0GBX', ['ppu/player.c'], scene=name) for name in scenes]
    for size in (512, 8192):
        label = '512' if size == 512 else '8k'
        for with_id in (True, False):
            suffix = 'id' if with_id else 'noid'
            out.append(rom(f'save/eeprom{label}_{suffix}.gba', 'GBX EEPROM', '0GBX', ['save/eeprom.c'],
                           [f'EEPROM_SIZE={size}', f'WITH_ID={int(with_id)}']))
    out.append(rom('save/sram_noid.gba', 'GBX SRAM', '0GBX', ['save/sram.c']))
    out.append(rom('save/flash64_noid.gba', 'GBX FLASH', '0GBX', ['save/flash.c']))
    out.append(rom('rtc/rtc.gba', 'GBX RTC', '0RTC', ['rtc/rtc.c']))
    out.append(rom('rom/rom32_cpu.gba', 'GBX ROM32', '0GBX', ['rom/rom32_cpu.c', 'rom/rom32_high.s'], rom32=True))
    out.append(rom('rom/rom32_dma.gba', 'GBX ROM32', '0GBX', ['rom/rom32_dma.c'], rom32=True))
    return out


def find_tool(toolchain, name):
    exe = name + ('.exe' if os.name == 'nt' else '')
    candidates = []
    for base in filter(None, (toolchain, os.environ.get('ARM_GNU_TOOLCHAIN'))):
        candidates += [Path(base) / exe, Path(base) / 'bin' / exe]
    for path in candidates:
        if path.is_file():
            return str(path)
    found = shutil.which(exe)
    if not found:
        raise SystemExit(f'{name} not found; pass --toolchain or set ARM_GNU_TOOLCHAIN')
    return found


def check_version(gcc):
    banner = subprocess.run([gcc, '--version'], capture_output=True, text=True, check=True).stdout
    if TOOLCHAIN_VERSION not in banner.splitlines()[0]:
        raise SystemExit(f'{gcc}: need {TOOLCHAIN_VERSION}, found: {banner.splitlines()[0]}')


def patch_header(data, title, code):
    """GBATEK "GBA Cartridge Header": title at 0A0h, game code at 0ACh, and
    the complement check at 0BDh = -(sum of 0A0h..0BCh) - 19h."""
    data[0xA0:0xAC] = title.encode('ascii').ljust(12, b'\0')
    data[0xAC:0xB0] = code.encode('ascii')
    data[0xBD] = (-(sum(data[0xA0:0xBD]) + 0x19)) & 0xFF


def rom32_pattern():
    """32 MiB of halfwords: the halfword at byte offset o is
    ((o >> 1) + (o >> 17) * 0x3B) & 0xFFFF, the formula rom/rom32.h checks."""
    base = array('H', range(65536))
    if sys.byteorder != 'little':
        base.byteswap()
    base = base.tobytes()
    out = bytearray()
    for block in range(ROM32_SIZE >> 17):
        k = (block * 0x3B) & 0xFFFF
        out += base[2 * k:] + base[:2 * k]
    return out


def build_one(spec, tools, work, output):
    gcc, objcopy = tools
    objects = []
    sources = ['common/crt0.s'] + spec['sources']
    defines = [f'-D{d}' for d in spec['defines']]
    if spec['scene']:
        scenes = load_scenes()
        blob = work / f'{spec["scene"]}.scene'
        blob.write_bytes(scenes.SCENES[spec['scene']]().blob())
        asm = work / f'{spec["scene"]}_blob.s'
        asm.write_text('    .section .rodata\n    .balign 4\n    .global scene_blob\n'
                       f'scene_blob:\n    .incbin "{blob.as_posix()}"\n')
        sources.append(str(asm))
    for i, source in enumerate(sources):
        src = Path(source) if Path(source).is_absolute() else SRC / source
        obj = work / f'{i}_{src.stem}.o'
        subprocess.run([gcc, *CFLAGS, *defines, '-I', str(SRC), '-c', str(src), '-o', str(obj)], check=True)
        objects.append(str(obj))
    elf = work / 'rom.elf'
    subprocess.run([gcc, *CFLAGS, '-Wl,--no-warn-rwx-segments', '-T', str(SRC / 'common/gba.ld'), *objects, '-lgcc', '-o', str(elf)],
                   check=True)
    low = work / 'low.bin'
    subprocess.run([objcopy, '-O', 'binary', '--remove-section=.rom_high', str(elf), str(low)], check=True)
    data = bytearray(low.read_bytes())
    patch_header(data, spec['title'], spec['code'])
    if spec['rom32']:
        high = work / 'high.bin'
        subprocess.run([objcopy, '-O', 'binary', '--only-section=.rom_high', str(elf), str(high)], check=True)
        image = rom32_pattern()
        image[:len(data)] = data
        high_data = high.read_bytes()
        start = HIGH_BASE - 0x08000000
        image[start:start + len(high_data)] = high_data
        data = image
    else:
        data += bytes(-len(data) % 4)
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_bytes(data)


def build_all(output, toolchain=None, only=None):
    gcc = find_tool(toolchain, 'arm-none-eabi-gcc')
    objcopy = find_tool(toolchain, 'arm-none-eabi-objcopy')
    check_version(gcc)
    built = []
    for spec in roms():
        if only and spec['path'] not in only:
            continue
        with tempfile.TemporaryDirectory() as tmp:
            build_one(spec, (gcc, objcopy), Path(tmp), output / spec['path'])
        built.append(spec['path'])
    return built


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('output', type=Path)
    parser.add_argument('--toolchain', type=Path)
    parser.add_argument('--only', nargs='*', help='Build only these ROM paths')
    args = parser.parse_args()
    for path in build_all(args.output, args.toolchain, args.only):
        print(path)


if __name__ == '__main__':
    main()

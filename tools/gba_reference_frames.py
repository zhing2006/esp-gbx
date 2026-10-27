#!/usr/bin/env python3
"""Reference frames of the GBA test ROMs that are judged by their picture.

    python3 tools/gba_reference_frames.py check
    python3 tools/gba_reference_frames.py generate --nba EXE --bios FILE
        [--mgba EXE] [--toolchain DIR]

The frames are committed in tests/gba/frames/ and copied to the card by
prepare_gba_tests.py. The card's GBA test data does not change after task
gba 1-2, so generate is used again only when a reference frame has been
found wrong and the user has agreed to replace it.

Reference emulator: NanoBoyAdvance v1.8.3, tag v1.8.3 of
https://github.com/nba-emu/NanoBoyAdvance (commit in REFERENCE). Frames come
from a small local program that links NanoBoyAdvance's core library through
its public headers (nba/core.hpp, config.hpp, device/video_device.hpp,
save_state.hpp). As agreed for task gba 1-2, that program and the emulator's
source stay out of the repository; the local copy is in refs/emulators/,
which git ignores. It runs with:
  - Config::skip_bios = true and the BIOS dump given by --bios (No-Intro
    xB02, see prepare_gba_tests.py --check-bios);
  - no color correction or frame blending: both live in NanoBoyAdvance's
    front-end shaders, the core hands over raw colors;
  - no save chip for these ROMs, no input.
Program contract: EXE --bios FILE --rom FILE --frames N --dump-from 1
--out DIR writes DIR/frames.bin, N frames of 240x160 little-endian u32
pixels, 0xAARRGGBB with each channel expanded from 5 bits as (c << 3) |
(c >> 2). Frame k is the picture that VideoDevice::Draw hands over for the
k-th time, at the k-th V-Blank after power-on.

--mgba takes the same kind of program for mGBA 0.10.5 (pixels 0xAABBGGRR)
and records whether its frame agrees. mGBA computes color effects in 8-bit
channels, so its frames are compared on the top 5 bits.

Capture rule: the reference frame is frame CAPTURE; frames STABLE_FROM to
STABLE_TO must all equal it, so the frame holds for any boot phase within
half a second. A runner on this emulator runs a ROM from power-on and takes
frame CAPTURE the same way.

The GBX scenes (tests/gba/roms/ppu/) also have to equal the frame that
tests/gba/roms/ppu/model.py computes from GBATEK; generate refuses a scene
that does not, and check verifies the committed frames against the model.

Files: tests/gba/frames/<suite>/<ROM path without .gba>.png, 240x160 RGB
with each channel (c << 3) | (c >> 2) of the BGR555 frame, as for the CGB
frames in docs/hardware-testing.md. frames.tsv lists them with the SHA-256
of the ROM each frame was taken from; prepare_gba_tests.py requires the
ROMs it puts on the card to have these digests.
"""

import argparse
import hashlib
import struct
import subprocess
import sys
import tempfile
import zlib
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FRAMES = ROOT / 'tests/gba/frames'
REFERENCE = ('NanoBoyAdvance', 'v1.8.3', '8c3b159c649b027ea8fc87df3290bad4cdcdcf4b')
CAPTURE, STABLE_FROM, STABLE_TO = 60, 30, 90

# Third-party ROMs judged by the picture, task gba 1-1 and 1-2. alyosha's
# ppu/stripes_x_offset.gba has no reference frame: it times a mid-line
# write by polling VCOUNT, and on NanoBoyAdvance its picture changes from
# frame to frame.
THIRD_PARTY = [
    ('jsmolka-gba-tests', 'ppu/hello.gba'),
    ('jsmolka-gba-tests', 'ppu/shades.gba'),
    ('jsmolka-gba-tests', 'ppu/stripes.gba'),
    ('alyosha-gba-tests', 'ppu/Sprite_Disable_Midline.gba'),
    ('alyosha-gba-tests', 'ppu/Sprite_Disable_on_VRAM_Access.gba'),
    ('nba-hw-test', 'ppu/bgpd/bgpd.gba'),
    ('nba-hw-test', 'ppu/bgx/bgx.gba'),
    ('nba-hw-test', 'ppu/dispcnt-latch/dispcnt-latch.gba'),
    ('nba-hw-test', 'ppu/greenswap/greenswap.gba'),
    ('nba-hw-test', 'ppu/sprite-hmosaic/sprite-hmosaic.gba'),
]


def load_ppu():
    sys.path.insert(0, str(ROOT / 'tests/gba/roms/ppu'))
    import model
    import scenes
    return model, scenes


def expand(c):
    return (c << 3) | (c >> 2)


def to555(pixel, order):
    if order == 'argb':
        r, g, b = (pixel >> 16) & 255, (pixel >> 8) & 255, pixel & 255
    else:
        r, g, b = pixel & 255, (pixel >> 8) & 255, (pixel >> 16) & 255
    if order == 'argb' and (expand(r >> 3), expand(g >> 3), expand(b >> 3)) != (r, g, b):
        raise ValueError(f'pixel {pixel:#010x} is not expanded from 5 bits')
    return (r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10)


def run(exe, order, bios, rom, frames):
    with tempfile.TemporaryDirectory() as tmp:
        subprocess.run([str(exe), '--bios', str(bios), '--rom', str(rom), '--frames', str(frames),
                        '--dump-from', '1', '--out', tmp], check=True, capture_output=True)
        data = (Path(tmp) / 'frames.bin').read_bytes()
    if len(data) != frames * 153600:
        raise ValueError(f'{rom}: expected {frames} frames')
    return [[to555(p, order) for p in struct.unpack_from('<38400I', data, k * 153600)] for k in range(frames)]


def write_png(path, frame):
    rows = b''.join(b'\0' + bytes(v for c in frame[y * 240:(y + 1) * 240]
                                  for v in (expand(c & 31), expand((c >> 5) & 31), expand((c >> 10) & 31)))
                    for y in range(160))

    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body))
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', struct.pack('>IIBBBBB', 240, 160, 8, 2, 0, 0, 0))
                     + chunk(b'IDAT', zlib.compress(rows, 9)) + chunk(b'IEND', b''))


def read_png(path):
    """Reads the PNGs written above: 8-bit RGB, no interlace."""
    data = path.read_bytes()
    pos, idat = 8, b''
    while pos < len(data):
        size, kind = struct.unpack_from('>I4s', data, pos)
        body = data[pos + 8:pos + 8 + size]
        if kind == b'IHDR' and body != struct.pack('>IIBBBBB', 240, 160, 8, 2, 0, 0, 0):
            raise ValueError(f'{path}: not a 240x160 8-bit RGB image')
        if kind == b'IDAT':
            idat += body
        pos += 12 + size
    raw = zlib.decompress(idat)
    frame, prev = [], bytearray(720)
    for y in range(160):
        kind, line = raw[y * 721], bytearray(raw[y * 721 + 1:(y + 1) * 721])
        for x in range(720):
            a = line[x - 3] if x >= 3 else 0
            b, c = prev[x], prev[x - 3] if x >= 3 else 0
            if kind == 1:
                line[x] = (line[x] + a) & 255
            elif kind == 2:
                line[x] = (line[x] + b) & 255
            elif kind == 3:
                line[x] = (line[x] + (a + b) // 2) & 255
            elif kind == 4:
                pa, pb, pc = abs(b - c), abs(a - c), abs(a + b - 2 * c)
                line[x] = (line[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        for x in range(240):
            r, g, b = line[3 * x:3 * x + 3]
            if (expand(r >> 3), expand(g >> 3), expand(b >> 3)) != (r, g, b):
                raise ValueError(f'{path}: color at ({x}, {y}) is not expanded from 5 bits')
            frame.append((r >> 3) | ((g >> 3) << 5) | ((b >> 3) << 10))
        prev = line
    return frame


def png_path(suite, rom):
    return FRAMES / suite / Path(rom).with_suffix('.png')


def repository_commit():
    head = subprocess.run(['git', 'rev-parse', 'HEAD'], cwd=ROOT, capture_output=True, text=True, check=True)
    dirty = subprocess.run(['git', 'status', '--porcelain'], cwd=ROOT, capture_output=True, text=True, check=True)
    return head.stdout.strip() + ('-dirty' if dirty.stdout.strip() else '')


def generate(args):
    model, scenes = load_ppu()
    sys.path.insert(0, str(ROOT / 'tools'))
    import gba_test_roms
    rows = []
    with tempfile.TemporaryDirectory() as tmp:
        built = Path(tmp)
        names = [f'ppu/{name}.gba' for name in scenes.SCENES]
        gba_test_roms.build_all(built, args.toolchain, names)
        jobs = [('gbx', name, built / name) for name in names]
        jobs += [(suite, rom, ROOT / 'refs/tests' / suite / rom) for suite, rom in THIRD_PARTY]
        for suite, rom, path in jobs:
            frames = run(args.nba, 'argb', args.bios, path, STABLE_TO)
            reference = frames[CAPTURE - 1]
            if any(f != reference for f in frames[STABLE_FROM - 1:]):
                raise SystemExit(f'{suite}/{rom}: frames {STABLE_FROM}-{STABLE_TO} are not all equal')
            first = CAPTURE
            while first > 1 and frames[first - 2] == reference:
                first -= 1
            model_note = '-'
            if suite == 'gbx':
                scene = Path(rom).stem
                expected = [c for row in model.render(scenes.SCENES[scene]()) for c in row]
                if expected != reference:
                    raise SystemExit(f'{suite}/{rom}: the reference frame differs from model.py')
                model_note = 'same'
            mgba_note = '-'
            if args.mgba:
                other = run(args.mgba, 'abgr', args.bios, path, CAPTURE)[-1]
                diff = sum(a != b for a, b in zip(reference, other))
                mgba_note = 'same' if diff == 0 else f'{diff} px differ'
            write_png(png_path(suite, rom), reference)
            rows.append((suite, rom, hashlib.sha256(path.read_bytes()).hexdigest(), first, model_note, mgba_note))
            print(f'{suite}/{rom}: first equal frame {first}, model {model_note}, mGBA {mgba_note}', flush=True)
    lines = [
        '# GBX reference frames v1',
        f'# reference\t{REFERENCE[0]}\t{REFERENCE[1]}\t{REFERENCE[2]}',
        f'# repository\t{repository_commit()}',
        f'# capture\tframe {CAPTURE}; frames {STABLE_FROM}-{STABLE_TO} equal',
        'suite\trom\trom_sha256\tfirst_equal_frame\tmodel\tmgba_0.10.5',
    ]
    lines += ['\t'.join(str(v) for v in row) for row in rows]
    (FRAMES / 'frames.tsv').write_text('\n'.join(lines) + '\n', newline='\n')


def listed():
    """(suite, ROM path, SHA-256 of the ROM) per frame in frames.tsv."""
    rows = []
    for line in (FRAMES / 'frames.tsv').read_text().splitlines():
        if line.startswith('#') or line.startswith('suite\t'):
            continue
        rows.append(tuple(line.split('\t')[:3]))
    return rows


def check():
    """Every listed frame exists and decodes; GBX scene frames equal the
    model. Returns listed()."""
    model, scenes = load_ppu()
    rows = listed()
    wanted = {('gbx', f'ppu/{name}.gba') for name in scenes.SCENES} | set(THIRD_PARTY)
    if {row[:2] for row in rows} != wanted:
        raise SystemExit(f'frames.tsv does not list the expected ROMs: {sorted({row[:2] for row in rows} ^ wanted)}')
    for suite, rom, _ in rows:
        frame = read_png(png_path(suite, rom))
        if suite == 'gbx':
            expected = [c for row in model.render(scenes.SCENES[Path(rom).stem]()) for c in row]
            if frame != expected:
                raise SystemExit(f'{suite}/{rom}: committed frame differs from model.py')
    return rows


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = parser.add_subparsers(dest='command', required=True)
    sub.add_parser('check', help='Verify the committed frames')
    gen = sub.add_parser('generate', help='Generate the frames with the reference emulator')
    gen.add_argument('--nba', type=Path, required=True, help='NanoBoyAdvance frame program, see above')
    gen.add_argument('--mgba', type=Path, help='mGBA frame program for cross-checking')
    gen.add_argument('--bios', type=Path, required=True)
    gen.add_argument('--toolchain', type=Path, help='Arm GNU Toolchain, see gba_test_roms.py')
    args = parser.parse_args()
    if args.command == 'generate':
        generate(args)
    print(f'{len(check())} reference frames checked')


if __name__ == '__main__':
    main()

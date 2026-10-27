"""Compare a board frame with the suite's exact reference: a DMG's shades with
the four suite shades, a CGB's colors with the suite's RGB image.

The board sends the frame encoded, see FRAME in tests/protocol/rom_test.h.
Each 5-bit CGB component becomes 8 bits as (X << 3) | (X >> 2), as the
suites' howto files ask (cgb-acid2, dmg-acid2, blargg, Mealybug, AGE).

Gambatte's tests are judged as its testrunner.cpp does (the Gambatte howto):
its DMG shades, its CGB color formula and the top five bits of each
component only, against a PNG or against the hexadecimal digits the file
name gives, drawn with the runner's patterns in the frame's top left. The
patterns are read from refs/tests/gambatte-testrunner/testrunner.cpp at run
time and not kept in this repository.
"""
import hashlib
from pathlib import Path
import re
import struct

WIDTH, HEIGHT = 160, 144
PIXELS = WIDTH * HEIGHT
PALETTE = {(v, v, v): shade for shade, v in enumerate((255, 170, 85, 0))}
TESTRUNNER = Path(__file__).resolve().parents[2] / 'refs/tests/gambatte-testrunner/testrunner.cpp'
# testrunner.cpp compares each component's top five bits.
GAMBATTE_MASK = 0xF8


def reference_shades(path):
    from PIL import Image
    with Image.open(path) as source:
        if source.size != (WIDTH, HEIGHT):
            raise ValueError(f'Reference is not a native DMG frame: {path}')
        pixels = source.convert('RGB').tobytes()
        colors = zip(pixels[0::3], pixels[1::3], pixels[2::3])
    try:
        return bytes(PALETTE[color] for color in colors)
    except KeyError as error:
        raise ValueError(f'Reference uses a non-suite shade {error.args[0]}: {path}') from error


def reference_colors(path):
    """The reference's RGB bytes; each component must be one a 5-bit value
    gives, or the image is not an exact CGB frame."""
    from PIL import Image
    with Image.open(path) as source:
        if source.size != (WIDTH, HEIGHT):
            raise ValueError(f'Reference is not a native CGB frame: {path}')
        pixels = source.convert('RGB').tobytes()
    allowed = {(x << 3) | (x >> 2) for x in range(32)}
    if not set(pixels) <= allowed:
        raise ValueError(f'Reference uses a color no 5-bit component gives: {path}')
    return pixels


def reference_any(path):
    """A Gambatte PNG's RGB bytes; its runner compares the top five bits of
    each component, so any component value is valid."""
    from PIL import Image
    with Image.open(path) as source:
        if source.size != (WIDTH, HEIGHT):
            raise ValueError(f'Reference is not a native frame: {path}')
        return source.convert('RGB').tobytes()


def decode_frame(encoded, rows=HEIGHT):
    """('shades', bytes of 0..3) for a DMG frame, or ('bgr555', the pixels),
    `rows` rows from the top."""
    if not encoded:
        raise ValueError('Empty frame')
    pixel_count = WIDTH * rows
    kind, body = encoded[0], encoded[1:]
    if kind == 0:
        if len(body) != pixel_count // 4:
            raise ValueError('Invalid packed DMG frame length')
        return 'shades', bytes((value >> shift) & 3 for value in body for shift in (0, 2, 4, 6))
    if kind == 1:
        count, bits = struct.unpack_from('<HB', body)
        if not 1 <= count <= 256 or bits not in (1, 2, 4, 8) or (bits < 8 and count > 1 << bits):
            raise ValueError('Invalid CGB frame palette')
        colors = struct.unpack_from(f'<{count}H', body, 3)
        packed = body[3 + count * 2:]
        if len(packed) != pixel_count * bits // 8:
            raise ValueError('Invalid CGB frame length')
        per_byte, mask = 8 // bits, (1 << bits) - 1
        indices = [(byte >> (k * bits)) & mask for byte in packed for k in range(per_byte)]
        if max(indices) >= count:
            raise ValueError('CGB frame index outside its palette')
        return 'bgr555', [colors[i] for i in indices]
    if kind == 2:
        if len(body) != pixel_count * 2:
            raise ValueError('Invalid raw CGB frame length')
        return 'bgr555', list(struct.unpack(f'<{pixel_count}H', body))
    raise ValueError('Unknown frame format')


def to_rgb(kind, pixels, colors=None):
    """RGB bytes: the suites' shades, and either the howto's (X << 3) | (X >> 2)
    or, with colors='gambatte', the formula of the Gambatte howto."""
    if kind == 'shades':
        return bytes(channel for shade in pixels for channel in (255 - shade * 85,) * 3)
    if colors == 'gambatte':
        out = bytearray()
        for p in pixels:
            r, g, b = p & 31, (p >> 5) & 31, (p >> 10) & 31
            out += bytes(((r * 13 + g * 2 + b) >> 1, (g * 3 + b) << 1, (r * 3 + g * 2 + b * 11) >> 1))
        return bytes(out)
    eight = [(x << 3) | (x >> 2) for x in range(32)]
    return bytes(eight[(p >> shift) & 31] for p in pixels for shift in (0, 5, 10))


def save_frame(encoded, directory, rows=HEIGHT):
    """Saves the actual frame as actual.png and returns the directory."""
    from PIL import Image
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=True)
    kind, pixels = decode_frame(encoded, rows)
    Image.frombytes('RGB', (WIDTH, rows), to_rgb(kind, pixels)).save(directory / 'actual.png')
    return str(directory)


def report(encoded, actual, same, directory, rows=HEIGHT):
    """The result of a comparison, with the actual frame and the differing
    pixels in red saved when any differ."""
    from PIL import Image
    differences = [i for i, equal in enumerate(same) if not equal]
    result = dict(different_pixels=len(differences), sha256=hashlib.sha256(encoded).hexdigest())
    if differences:
        directory = Path(save_frame(encoded, directory, rows))
        diff = bytes(channel for i in range(len(same))
                     for channel in ((255, 0, 0) if not same[i] else actual[i * 3:i * 3 + 3]))
        Image.frombytes('RGB', (WIDTH, rows), diff).save(directory / 'difference.png')
        result['first_difference'] = dict(x=differences[0] % WIDTH, y=differences[0] // WIDTH)
        result['images'] = str(directory)
    return result


def compare_frame(encoded, reference, directory, report_only=False, colors=None):
    kind, pixels = decode_frame(encoded)
    try:
        expected = (reference_any(reference) if colors == 'gambatte' else
                    reference_shades(reference) if kind == 'shades' else reference_colors(reference))
    except ValueError as error:
        if not report_only:
            raise
        return dict(different_pixels=None, status='UNJUDGEABLE_REFERENCE', reason=str(error),
                    sha256=hashlib.sha256(encoded).hexdigest(), images=save_frame(encoded, directory))
    if colors == 'gambatte':
        actual = to_rgb(kind, pixels, colors)
        same = [all((actual[i * 3 + c] ^ expected[i * 3 + c]) & GAMBATTE_MASK == 0 for c in range(3))
                for i in range(PIXELS)]
    elif kind == 'shades':
        actual = to_rgb(kind, pixels)
        same = [a == b for a, b in zip(pixels, expected)]
    else:
        actual = to_rgb(kind, pixels)
        same = [actual[i * 3:i * 3 + 3] == expected[i * 3:i * 3 + 3] for i in range(PIXELS)]
    return report(encoded, actual, same, directory)


_digit_tiles = None


def digit_tiles():
    """The sixteen 8x8 digit patterns of testrunner.cpp's tileFromChar(), True
    for black (O), False for white (_), row by row."""
    global _digit_tiles
    if _digit_tiles is None:
        text = TESTRUNNER.read_text()
        body = re.search(r'tiles\[0x10 \* 8 \* 8\] = \{(.*?)\};', text, re.S)
        if not body:
            raise ValueError(f'No digit patterns in {TESTRUNNER}')
        # The cells between the #define and #undef lines of _ and O.
        values = ' '.join(line for line in body[1].splitlines() if not line.strip().startswith('#'))
        cells = [cell.strip() for cell in values.split(',')]
        if len(cells) != 16 * 64 or set(cells) - {'_', 'O'}:
            raise ValueError(f'Expected 1024 cells of _ and O in {TESTRUNNER}')
        _digit_tiles = [[cell == 'O' for cell in cells[i * 64:(i + 1) * 64]] for i in range(16)]
    return _digit_tiles


def compare_digits(encoded, digits, directory, rows=8):
    """testrunner.cpp's frameBufferMatchesOut(): digit i is the 8x8 tile at
    x = 8i in the top rows, white (F8F8F8 after the mask) or black (0)."""
    kind, pixels = decode_frame(encoded, rows)
    actual = to_rgb(kind, pixels, 'gambatte')
    tiles = digit_tiles()
    same = [True] * (WIDTH * rows)
    for i, digit in enumerate(digits):
        tile = tiles[int(digit, 16)]
        for y in range(8):
            for x in range(8):
                at = y * WIDTH + i * 8 + x
                want = 0 if tile[y * 8 + x] else GAMBATTE_MASK
                same[at] = all(actual[at * 3 + c] & GAMBATTE_MASK == want for c in range(3))
    return report(encoded, actual, same, directory, rows)

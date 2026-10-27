#!/usr/bin/env python3
"""Talk to the firmware's serial console, see docs/debug-design.md.

    python3 tools/console.py [--run DIR] serve PORT [--reset]
    python3 tools/console.py [--run DIR] send COMMAND ... [--until REGEX]
    python3 tools/console.py [--run DIR] replay FILE [--backend int|jit]

"serve" holds the port: it writes everything the board prints to
DIR/console.log, saves the frames of "shot" as PNG files in DIR/shots, the
files of "file" under DIR/files at their card paths, and sends the lines
appended to DIR/in.txt. "send" and "replay" run beside it and
talk through these files, so one board log keeps every command and reply. DIR
is build/console by default; serve needs pyserial.

"send" appends commands to DIR/in.txt and prints the board's lines until one
matches --until or the timeout passes; without --all, only warnings, errors,
shots and the commands themselves.

"replay" sends the console commands of FILE one a line; "{backend}" is
replaced by --backend. Its other lines:
  wait <seconds>      waits on the PC
  # <text>            a comment, also after two spaces on a line
  @ <label>           starts a part, for --from and the printed output
  expect <png>        takes a shot and stops unless it matches <png>
  until <png> <n> <keys steps>
                      takes shots, pressing the keys between them, until one
                      matches <png>; stops after n presses
<png> is a shot saved by "serve", relative to FILE's directory. Shots match
when 99.5 % of their pixels are equal, so a blinking text arrow passes;
<png>@<share> sets another share, for scenes where people walk around. After
"keys", "shot" and "load" the replay waits for the board's reply. A shot that
fails to arrive is taken again before any key is pressed, since pressing keys
after a lost shot can walk past the checkpoint. When the replay stops, it
prints the line it stopped at; once the game is back where that line expects
it, --line continues from there. The timing of the key presses is wall clock:
a replay can drift where the game runs slower or faster, which is what the
shot checkpoints catch.
"""
import argparse
import base64
import os
from pathlib import Path
import re
import struct
import sys
import time
import zlib

ROOT = Path(__file__).resolve().parent.parent
ANSI = re.compile(r'\x1b\[[0-9;]*m')
# Lines worth showing from the board: warnings and errors, which include
# every reply of the console, shots, sent commands and crashes.
NOTABLE = re.compile(r'[EW] \(\d+\)|shot|>>>|Guru|abort|panic|rst:|assert', re.I)


def save_png(path, width, height, data, scale=2, format='BGR555'):
    """Writes BGR555 or RGB565 pixels as an RGB PNG, scaled up."""
    rows = []
    for y in range(height):
        row = bytearray([0])
        for x in range(width):
            pixel = data[2 * (y * width + x)] | data[2 * (y * width + x) + 1] << 8
            if format == 'RGB565':
                r, g, b = pixel >> 11, pixel >> 5 & 63, pixel & 31
                rgb = (r << 3 | r >> 2, g << 2 | g >> 4, b << 3 | b >> 2)
            else:
                r, g, b = pixel & 31, pixel >> 5 & 31, pixel >> 10 & 31
                rgb = (r << 3 | r >> 2, g << 3 | g >> 2, b << 3 | b >> 2)
            row += bytes(rgb) * scale
        rows += [bytes(row)] * scale

    def chunk(kind, body):
        return struct.pack('>I', len(body)) + kind + body + struct.pack('>I', zlib.crc32(kind + body))

    header = struct.pack('>IIBBBBB', width * scale, height * scale, 8, 2, 0, 0, 0)
    path.write_bytes(b'\x89PNG\r\n\x1a\n' + chunk(b'IHDR', header) + chunk(b'IDAT', zlib.compress(b''.join(rows))) +
                     chunk(b'IEND', b''))


def load_png(path):
    """The pixels of a PNG that save_png() wrote: 8-bit RGB, filter 0."""
    data = path.read_bytes()
    pos, idat, width, height = 8, b'', 0, 0
    while pos < len(data):
        size = struct.unpack('>I', data[pos:pos + 4])[0]
        kind, body = data[pos + 4:pos + 8], data[pos + 8:pos + 8 + size]
        if kind == b'IHDR':
            width, height = struct.unpack('>II', body[:8])
        elif kind == b'IDAT':
            idat += body
        pos += 12 + size
    raw = zlib.decompress(idat)
    stride = 1 + width * 3
    return [raw[y * stride + 1 + x * 3:y * stride + 4 + x * 3] for y in range(height) for x in range(width)]


def serve(args):
    import serial

    shots = args.run / 'shots'
    shots.mkdir(parents=True, exist_ok=True)
    commands = args.run / 'in.txt'
    commands.touch()
    port = serial.Serial(port=None, baudrate=args.baud, timeout=0.05, xonxoff=False, rtscts=False, dsrdtr=False)
    port.port = args.port
    port.dtr = False
    port.rts = False
    port.open()
    if args.reset:
        # EN low through RTS while IO0 stays high, as the test runners do.
        port.rts = True
        time.sleep(0.1)
        port.rts = False
    start = time.monotonic()
    with (args.run / 'console.log').open('w', encoding='utf-8') as log, commands.open(encoding='utf-8') as source:
        source.seek(0, os.SEEK_END)

        def emit(text):
            print(text, flush=True)
            log.write(text + '\n')
            log.flush()

        pending, typed, shot, file, count = b'', '', None, None, 0
        try:
            while not args.seconds or time.monotonic() - start < args.seconds:
                typed += source.read()
                *lines, typed = typed.split('\n')
                for line in lines:
                    port.write((line.rstrip('\r') + '\n').encode())
                    emit(f'[{time.monotonic() - start:7.2f}] >>> {line.rstrip()}')
                data = port.read(4096)
                if not data:
                    continue
                pending += data
                *lines, pending = pending.split(b'\n')
                for raw in lines:
                    text = raw.decode('utf-8', 'replace').rstrip('\r')
                    stamp = f'[{time.monotonic() - start:7.2f}]'
                    if text.startswith('SHOT BEGIN'):
                        # SHOT BEGIN <width> <height> [<format>], BGR555 by default
                        fields = text.split()
                        shot = (int(fields[2]), int(fields[3]), fields[4] if len(fields) > 4 else 'BGR555', [])
                    elif text.startswith('SHOT END') and shot:
                        width, height, format, parts = shot
                        shot = None
                        try:
                            frame = base64.b64decode(''.join(parts))
                        except ValueError:
                            frame = b''
                        if len(frame) != width * height * 2:
                            emit(f'{stamp} shot: bad frame, {len(frame)} bytes')
                            continue
                        count += 1
                        path = shots / f'shot_{int(time.time())}_{count}.png'
                        save_png(path, width, height, frame, format=format)
                        emit(f'{stamp} shot saved: {path}')
                    elif text.startswith('SHOT ') and shot:
                        shot[3].append(text[5:].strip())
                    elif text.startswith('FILE BEGIN'):
                        # FILE BEGIN <size> <card path>
                        fields = text.split(maxsplit=3)
                        file = (int(fields[2]), fields[3], [])
                    elif text.startswith('FILE END') and file:
                        size, name, parts = file
                        file = None
                        try:
                            body = base64.b64decode(''.join(parts))
                        except ValueError:
                            body = b''
                        if len(body) != size:
                            emit(f'{stamp} file: bad transfer, {len(body)} bytes')
                            continue
                        # The card path stays under DIR/files, whatever
                        # the board printed: FAT names hold no '\\' or ':',
                        # which Windows reads as separators and drives.
                        root = (args.run / 'files').resolve()
                        kept = [part for part in name.split('/') if part not in ('', '.')]
                        path = root.joinpath(*kept).resolve() if kept else root
                        if (not kept or '..' in kept or any('\\' in part or ':' in part for part in kept)
                                or not path.is_relative_to(root) or path == root):
                            emit(f'{stamp} file: bad path {name}')
                            continue
                        path.parent.mkdir(parents=True, exist_ok=True)
                        path.write_bytes(body)
                        emit(f'{stamp} file saved: {path}')
                    elif text.startswith('FILE ') and file:
                        file[2].append(text[5:].strip())
                    else:
                        emit(f'{stamp} {text}')
        except KeyboardInterrupt:
            pass
    port.close()


class Board:
    """The console through the files of a running "serve"."""

    def __init__(self, run):
        self.commands = run / 'in.txt'
        self.log = (run / 'console.log').open(encoding='utf-8', errors='replace')
        self.log.seek(0, os.SEEK_END)
        self.pending = ''

    def send(self, line):
        with self.commands.open('a', encoding='utf-8') as f:
            f.write(line + '\n')

    def lines(self, timeout):
        """The board's new lines until the timeout, without colors."""
        end = time.monotonic() + timeout
        while True:
            self.pending += self.log.read()
            *lines, self.pending = self.pending.split('\n')
            for line in lines:
                yield ANSI.sub('', line)
            if time.monotonic() >= end:
                return
            time.sleep(0.05)

    def wait(self, pattern, timeout, show=NOTABLE):
        """Prints notable lines until one matches; False on timeout."""
        for line in self.lines(timeout):
            if show and show.search(line):
                print(line, flush=True)
            if pattern and re.search(pattern, line):
                return True
        return pattern is None

    def shot(self):
        """The path of the next frame's PNG, or None."""
        # A corrupted transfer is taken again.
        for _ in range(3):
            self.send('shot')
            for line in self.lines(15):
                match = re.search(r'shot saved: (.*)', line)
                if match:
                    return Path(match.group(1).strip())
                if 'shot:' in line:
                    break
        return None


def send(args):
    board = Board(args.run)
    for line in args.commands:
        board.send(line)
    if board.wait(args.until, args.timeout, None if args.all else NOTABLE) or not args.until:
        return 0
    print('console: timed out', file=sys.stderr)
    return 1


def matches(path, reference, share):
    a, b = load_png(path), load_png(reference)
    same = sum(1 for p, q in zip(a, b) if p == q)
    return len(a) == len(b) and same >= share * len(a), same / max(len(a), 1)


def replay(args):
    board = Board(args.run)
    # The first line after a reset can pick up stray bytes; spend it on nothing.
    board.send('')
    if args.stat:
        board.send(f'stat {args.stat}')
    skipping = args.start is not None
    if args.line:
        print(f'--- line {args.line}', flush=True)
    for number, raw in enumerate(args.file.open(encoding='utf-8'), 1):
        line = raw.split('  #')[0].strip()
        if number < args.line or not line or line.startswith('#'):
            continue
        if line.startswith('@'):
            label = line[1:].strip()
            skipping = skipping and label != args.start
            print(f'--- {label}', flush=True)
            continue
        if skipping:
            continue
        line = line.replace('{backend}', args.backend)
        if line.startswith('wait '):
            board.wait(None, float(line.split()[1]))
            continue
        if line.startswith(('expect ', 'until ')):
            parts = line.split(maxsplit=3)
            name, _, share = parts[1].partition('@')
            reference = args.file.resolve().parent / name
            presses = int(parts[2]) if parts[0] == 'until' else 0
            for attempt in range(presses + 1):
                path = board.shot()
                ok, ratio = matches(path, reference, float(share or 0.995)) if path else (False, 0)
                print(f'{parts[0]} {parts[1]}: {ratio:.3f} {path}', flush=True)
                if ok or attempt == presses:
                    break
                board.send('keys ' + parts[3])
                board.wait(r'keys: (done|no key)', 300)
            if not ok:
                print(f'console: the screen does not match {parts[1]}; stopped at line {number}', flush=True)
                return 1
            continue
        board.send(line)
        reply = (r'keys: (done|no key)' if line.startswith('keys ') else r'shot saved|shot:' if line == 'shot' else
                 r'console: load ' if line.startswith('load ') else None)
        if reply and not board.wait(reply, 300 if line.startswith('keys ') else 15):
            print(f'console: no reply to {line}; stopped at line {number}', flush=True)
            return 1
    if args.stat:
        board.send('stat 0')
    return 0


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--run', type=Path, default=ROOT / 'build' / 'console',
                        help='Directory of the log, the command file and the shots')
    commands = parser.add_subparsers(dest='command', required=True)
    p = commands.add_parser('serve', help='Hold the serial port')
    p.add_argument('port')
    p.add_argument('--reset', action='store_true', help='Reset the board through RTS first')
    p.add_argument('--baud', type=int, default=460800)
    p.add_argument('--seconds', type=float, default=0, help='Stop after this long; 0 runs until Ctrl+C')
    p = commands.add_parser('send', help='Send commands and print the replies')
    p.add_argument('commands', nargs='*')
    p.add_argument('--until', help='Stop at the first line matching this regular expression')
    p.add_argument('--timeout', type=float, default=10)
    p.add_argument('--all', action='store_true', help='Print every line')
    p = commands.add_parser('replay', help='Replay a command file')
    p.add_argument('file', type=Path)
    p.add_argument('--backend', default='jit', choices=('int', 'jit'))
    p.add_argument('--from', dest='start', help='Start at this @ label')
    p.add_argument('--line', type=int, default=0, help='Start at this line of the file')
    p.add_argument('--stat', type=int, default=0, help='Print the status every this many seconds')
    args = parser.parse_args()
    args.run.mkdir(parents=True, exist_ok=True)
    return {'serve': serve, 'send': send, 'replay': replay}[args.command](args) or 0


if __name__ == '__main__':
    sys.exit(main())

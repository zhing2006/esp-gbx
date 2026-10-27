#!/usr/bin/env python3
"""Summarize the samples of the profiling builds.

    python3 tools/profile_report.py REPORT ELF [--top N] [--rom-image FILE]
    python3 tools/profile_report.py LOG ELF [--top N] [--all] [--rom-image FILE]

REPORT is a run_roms.py or run_gba_roms.py report directory from the
profiling test firmware and ELF the firmware it ran. Prints the run times of
both backends, then for every ROM, backend and sampled core the samples by
category and by function, and the counters: the GB JIT's, or the GBA CPU
core's waits for the renderer.

LOG is a console log of the profiling firmware, such as the one that
tools/console.py serve writes, and ELF that firmware. Prints the latest
"prof" in it, or every one with --all, by category and by function.
Samples in the JIT's gateway, and in the copies of it that start the
segments of its second level, are split by stub family, from the layout the
firmware logs when the JIT starts.

Samples in the chip's mask ROM are named with --rom-image, the ROM read
from the board, such as with `esptool dump-mem 0x4fc00000 0x20000 FILE` on
ESP32-P4. Every ROM function the firmware links against has an entry in the
ROM's jump table that jumps to it. A function is taken to run up to the next
such one, so the ROM's internal code between them counts as the one before.
The ROM ELFs of esp-rom-elfs do not match every chip revision, such as
ESP32-P4 v1.x.

riscv32-esp-elf-nm is taken from PATH, or run through `eim run ... v6.1`
when it is not there.
"""
import argparse
import bisect
import collections
import json
from pathlib import Path
import re
import shutil
import subprocess

# The first matching pattern names a function's category.
CATEGORIES = [
    ('compile', r'Compiler|CodeCache::(insert|gap|retire|erase|clear)|(Sm83|Arm7)Jit::(retired|link|free_link|'
                r'compile)|CodeStore::store|'
                r'memmove|memcpy|publish|esp_cache|Cache_|cache_hal'),
    ('jit-runtime', r'sm83_runtime|Sm83Jit::|CodeCache::find|sm83_halt|sm83_stop|arm7_runtime|Arm7Jit::|'
                    r'CodeStore::(find|target)'),
    ('interpreter', r'sm83_run_block|sm83_run_double_speed|Instruction<|sm83_step|interpreter_block|arm7_step|'
                    r'arm7_run|arm7_execute'),
    ('critical', r'vPortExitCritical|vPortClearInterruptMask|xPortEnterCritical'),
    ('sound', r'Apu::|Mixer::|WaveTimer::'),
    ('worker', r'Offload::'),
    ('hash', r'psa_|mbedtls_|esp_sha|sha_hal|sha256'),
    ('device', r'Scheduler|Ppu|Memory::|Timer|Serial|Cartridge|Rtc::|Renderer'),
    ('system', r'System::'),
    ('idle', r'esp_cpu_wait_for_intr|prvIdleTask'),
]


def symbols(elf, nm, kinds='tTwW'):
    """(address, bytes, name) of the symbols of the given nm kinds."""
    command = [nm, '-n', '-S', '-C', str(elf)]
    if shutil.which(nm):
        output = subprocess.run(command, capture_output=True, text=True, check=True).stdout
    else:
        output = subprocess.run(['eim', 'run', ' '.join(command), 'v6.1'],
                                capture_output=True, text=True, check=True).stdout
    table = []
    for line in output.splitlines():
        match = re.match(rf'^([0-9a-f]{{8}})(?: ([0-9a-f]{{8}}))? [{kinds}] (.*)$', line)
        if match:
            table.append((int(match[1], 16), int(match[2] or '0', 16), match[3]))
    table.sort()
    return table


def rom_functions(absolute, image, base):
    """Symbols of the ROM functions that the firmware's absolute symbols
    reach through the jump table in image, a ROM read from base."""
    starts = {}
    for address, _, name in absolute:
        offset = address - base
        if not 0 <= offset <= len(image) - 4:
            continue
        word = int.from_bytes(image[offset:offset + 4], 'little')
        if word & 0xfff != 0x06f:  # Not jal zero, a jump.
            continue
        target = (word >> 31 << 20 | (word >> 12 & 0xff) << 12 | (word >> 20 & 1) << 11 |
                  (word >> 21 & 0x3ff) << 1)
        starts.setdefault(address + target - (word >> 31 << 21), f'{name} (ROM)')
    ends = sorted(starts)[1:] + [base + len(image)]
    return [(start, end - start, starts[start]) for start, end in zip(sorted(starts), ends)]


def resolver(table):
    starts = [entry[0] for entry in table]

    def name(address):
        i = bisect.bisect_right(starts, address) - 1
        if i >= 0 and address < table[i][0] + max(table[i][1], 1):
            return table[i][2]
        return f'?{address:x}'
    return name


def parse(path):
    """Profiles in file order: (rom, header, counters, samples by address)."""
    jobs, rom = [], None
    for line in path.read_text().splitlines():
        kind, _, rest = line.partition(' ')
        if kind == 'rom':
            rom = rest
        elif kind == 'job':
            header = dict(field.split('=') for field in rest.split())
            jobs.append((rom, header, {}, collections.Counter()))
        elif kind == 'counter':
            name, value = rest.split()
            jobs[-1][2][name] = int(value)
        elif kind == 's':
            address, count = rest.split()
            jobs[-1][3][int(address, 16)] += int(count)
    return jobs


def parse_console(path):
    """The console's "prof" blocks in file order: (header, samples by address)."""
    blocks, current, gateway, store = [], None, None, None
    for line in path.read_text(encoding='utf-8', errors='replace').splitlines():
        # The JIT logs its gateway's stub families and its second level when
        # it starts.
        layout = re.search(r'gateway ([0-9a-f]{8}) (\d+) bytes: (.*?)\s*$', line)
        if layout:
            families = [('enter', 0)] + [(name, int(start)) for name, start in re.findall(r'(\w+) (\d+)', layout[3])]
            gateway = (int(layout[1], 16), int(layout[2]), families)
            continue
        layout = re.search(r'store ([0-9a-f]{8}) (\d+) bytes, segments of (\d+) bytes', line)
        if layout:
            store = (int(layout[1], 16), int(layout[2]), int(layout[3]))
            continue
        match = re.search(r'PROF (BEGIN .*|END|[0-9a-f]+ [0-9a-f]+)\s*$', line)
        if not match:
            continue
        body = match[1]
        if body.startswith('BEGIN'):
            fields = body.split()[1:]
            current = (dict(zip(fields[::2], fields[1::2]), gateway=gateway, store=store), collections.Counter())
        elif body == 'END':
            if current:
                blocks.append(current)
            current = None
        elif current:
            address, count = body.split()
            current[1][int(address, 16)] += int(count, 16)
    return blocks


def span(text, size_base=10):
    base, size = text.split(':')
    return int(base, 16), int(size, size_base)


def jit_code(address, gateway, store):
    """(function, category) of an address in the JIT's gateway or second
    level, or None. gateway is (base, bytes, stub families or None), store
    (base, bytes, segment bytes); either may be None."""
    def stub(offset):
        if not gateway[2]:
            return 'JIT gateway', 'JIT gateway'
        family = [family for family, start in gateway[2] if start <= offset][-1]
        return f'gateway: {family}', 'JIT gateway'
    if gateway and gateway[0] <= address < gateway[0] + gateway[1]:
        return stub(address - gateway[0])
    if store and store[0] <= address < store[0] + store[1]:
        # Each segment starts with a copy of the gateway.
        offset = (address - store[0]) % store[2]
        if gateway and offset < gateway[1]:
            return stub(offset)
        return 'JIT second level', 'JIT second level'
    return None


def category(function, otherwise='other'):
    return next((key for key, pattern in CATEGORIES if re.search(pattern, function)), otherwise)


def in_rom(address, rom):
    return rom is not None and rom[0] <= address < rom[0] + rom[1]


def show(title, samples, total, other, classify, top):
    """Samples by category and by function; classify(address) gives the
    function and a category, or None to find it in CATEGORIES."""
    functions, categories = collections.Counter(), collections.Counter()
    for address, count in samples.items():
        function, found = classify(address)
        functions[function] += count
        categories[found or category(function)] += count
    categories['outside the sampled ranges'] += other
    print(f'\n== {title}: {total} samples')
    for key, count in categories.most_common():
        print(f'  {100 * count / total:5.1f}% {count:8d}  {key}')
    print('  Functions:')
    for function, count in functions.most_common(top):
        print(f'  {100 * count / total:5.1f}% {count:8d}  {function[:100]}')


def console(args, names):
    blocks = parse_console(args.report)
    if not blocks:
        raise SystemExit(f'{args.report}: no prof output')
    for header, samples in blocks if args.all else blocks[-1:]:
        ram = span(header['ram'], 16)
        mask_rom = span(header['rom'], 16) if 'rom' in header else None
        code = [span(region, 16) for region in header.get('code', '-').split(',') if region != '-']
        gateway, store = header['gateway'], header['store']
        name = names(mask_rom)

        def classify(address):
            found = jit_code(address, gateway, store)
            if found:
                return found
            function = name(address)
            if in_rom(address, mask_rom):
                return function, category(function, 'chip ROM')
            # IRAM functions have symbols; the rest of internal RAM that runs
            # code is the JIT's pool, as is generated code elsewhere.
            if function.startswith('?') and any(base <= address < base + size for base, size in [ram] + code):
                return 'JIT code', 'JIT code'
            return function, None
        show(f'core {header["core"]}, {header["ms"]} ms', samples, int(header['samples']), int(header['other']),
             classify, args.top)


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('report', type=Path)
    parser.add_argument('elf', type=Path)
    parser.add_argument('--top', type=int, default=25, help='Functions listed per profile')
    parser.add_argument('--nm', default='riscv32-esp-elf-nm')
    parser.add_argument('--rom-image', type=Path, help="The chip's mask ROM, read from the board")
    parser.add_argument('--all', action='store_true', help='Every prof of a console log, not only the latest')
    args = parser.parse_args()

    table = symbols(args.elf, args.nm)
    image = args.rom_image.read_bytes() if args.rom_image else None
    absolute = symbols(args.elf, args.nm, 'A') if image else []

    def names(mask_rom):
        """Function names, with those of the mask ROM if it was read."""
        return resolver(sorted(table + (rom_functions(absolute, image, mask_rom[0]) if image and mask_rom else [])))

    if args.report.is_file():
        console(args, names)
        return

    print('Run times (cold JIT cache, including compilation):')
    # run_roms.py writes results.jsonl; run_gba_roms.py keeps them in summary.json.
    jsonl = args.report / 'results.jsonl'
    rows = ([json.loads(line) for line in jsonl.open()] if jsonl.exists()
            else json.loads((args.report / 'summary.json').read_text())['results'])
    for row in rows:
        actual = row.get('actual', {})
        interpreter, jit = actual.get('interpreter', {}).get('wall_us'), actual.get('jit', {}).get('wall_us')
        if not interpreter or not jit:
            continue
        stats = actual['jit'].get('jit', {})
        print(f'  {row["id"]:60s} {row["status"]:5s} interpreter {interpreter / 1e6:7.3f} s  '
              f'JIT {jit / 1e6:7.3f} s  ratio {jit / interpreter:4.2f}  compiled {stats.get("compiled")} '
              f'invalidated {stats.get("invalidated")} evicted {stats.get("evicted")}')

    for rom, header, counters, samples in parse(args.report / 'profile.txt'):
        total = int(header['samples'])
        backend = 'JIT' if header['backend'] == '1' else 'interpreter'
        if 'core' in header:
            backend += f', core {header["core"]}'
        pool, gateway = span(header['pool']), span(header['gateway']) + (None,)
        store = span(header['store']) + (int(header['segment']),) if 'store' in header else None
        mask_rom = span(header['rom']) if 'rom' in header else None
        name = names(mask_rom)

        def classify(address):
            found = jit_code(address, gateway if gateway[1] else None, store if store and store[1] else None)
            if found:
                return found
            if pool[0] <= address < pool[0] + pool[1]:
                return 'JIT code', 'JIT code'
            if in_rom(address, mask_rom):
                return name(address), category(name(address), 'chip ROM')
            return name(address), None
        show(f'{rom}, {backend}, every {header["period_us"]} us', samples, total, int(header['other']), classify,
             args.top)
        if counters:
            print('  Counters: ' + ', '.join(f'{key}={value}' for key, value in counters.items()))


if __name__ == '__main__':
    main()

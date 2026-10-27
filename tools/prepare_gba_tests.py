#!/usr/bin/env python3
"""Prepare GBA test data from local references for the card's tests/gba/.

Output layout:
  arm7tdmi/cases.bin   the ARM7TDMI single-step cases, format below, with the
                       suite's README.md and LICENSE
  roms/<suite>/...     the selected test ROMs of jsmolka-gba-tests,
                       alyosha-gba-tests and nba-hw-test, at their paths in
                       refs/tests/<suite>/, with each suite's README.md and
                       LICENSE; nba-hw-test keeps the expected.png of its PPU
                       tests for manual checks
  roms/gbx/...         the GBX test ROMs, built from tests/gba/roms/ by
                       gba_test_roms.py with the toolchain it pins
                       (--toolchain); their layout is listed there
  frames/              the reference frames committed in tests/gba/frames/,
                       frames.tsv and <suite>/<ROM path without .gba>.png;
                       see gba_reference_frames.py
  manifest.tsv         source versions, then SHA-256, size and path per file

Before writing the output, the reference frames are checked as
gba_reference_frames.py check does; afterwards every ROM with a frame must
have the SHA-256 that frames.tsv records for it. A mismatch means the frame
was taken from another ROM: generate the frames again.

ARM7TDMI source format (SingleStepTests/ARM7TDMI v1/*.json.bin, documented
by its README.md and v1/transcode_json.py), little-endian:
  File: u32 magic 0xD33DBAE0, u32 case count (50000 in every file; the README
  says 20,000 in one place and 50000 in another), then the cases.
  Case: i32 size, then four chunks, each starting with i32 size and i32 tag:
  initial state (tag 1), final state (tag 2), transactions (tag 3, then an
  i32 count and 6 u32 per transaction), opcode and base_addr (tag 4, two u32).
  A state is 40 u32 in this order: R0-R15; R8-R14 of FIQ; R13-R14 of SVC,
  ABT, IRQ and UND; CPSR; SPSR of FIQ, SVC, ABT, IRQ and UND; pipeline[0..1];
  access. The packer checks every size and tag and rejects other values.

Field meanings, from the README unless marked "data", which means
check_meaning() requires it of every case; the pinned commit's 2,250,000
cases all pass:
  - R15 is the address of the second pipeline entry: base_addr + 8 in ARM
    state, + 4 in Thumb state (data). pipeline[0] is the opcode about to
    execute and equals opcode (data); pipeline[1] is the next fetched word.
    The CPU starts from the initial state and executes one instruction.
  - State "access" is NanoBoyAdvance's internal access type of the next code
    fetch, with the transaction access bits below. It is 2 or 3 (Code,
    nonsequential or sequential) in every case (data).
  - Transaction kind: 0 instruction fetch, 1 data read, 2 write. Only these
    occur (data); transcode_json.py tolerates kind 3 without explaining it.
  - size is the access width in bytes: 1, 2 or 4.
  - access ORs NanoBoyAdvance's bits: 1 Sequential (0 means Nonsequential),
    2 Code, 4 Dma, 8 Lock. Fetches have 2 or 3, data reads 0 or 1, writes
    0, 1 or 8; Lock occurs only on writes in arm_swp (data).
  - cycle is the cycle number of the transaction, 1 to 19 and never
    decreasing within a case (data). The README says cycle numbers are not
    recorded correctly but the order of transactions is, so cycle is kept for
    diagnostics and not judged.
  - Reads return the data of the matching transaction. A fetch from base_addr
    returns opcode; any other fetch returns the requested address itself,
    truncated to the access width, so a halfword fetch returns its low 16
    bits (data; the README omits the truncation).
README "Known Issues" and "Disclaimers": some illegal encodings, which an
assembler cannot produce, with r15 as both the write-back and the
destination register may not behave correctly; the tests do not restrict
the alignment of data reads and writes; the address space is a flat 32-bit
RAM with no I/O; cycle counts and the cycle of each transaction are not
correct, their order is. Cases found wrong are not edited here; they are
listed for tasks gba 1-3 and 1-4.

cases.bin is little-endian, version 1:
  Header <8sII>: magic GBXARM7 followed by NUL, version, source file count.
  Index <32sIIQQQ> per source file, sorted by name: the file name without
  .json.bin, NUL-padded; case count; reserved zero; offset of the file's case
  offset table; offset and byte size of its records.
  The case offset table has a u32 per case, the record's offset from the start
  of the file's records. Record i ends where record i + 1 starts, the last one
  at the end of the records. Cases keep the source order, from 0.
  Record: the initial state as 40 u32 in source order; a u64 mask whose bit i
  is set when final word i differs from initial word i, then those final
  words as u32 in order; opcode and base_addr as u32; a u8 transaction count,
  then <BBBBII> per transaction: kind, size, access, cycle, addr, data.
The source's chunk sizes and tags are framing and are not stored; no other
field is dropped or changed. A board reads the index and the offsets of the
selected cases, then only those records. Packing verifies every case read
back from the written file against its source.

BIOS: the user's dump of the GBA BIOS stays out of the data and the manifest
and goes to bios/gba_bios.bin on the card. A dump is valid when it matches
No-Intro's verified entry xB02, "[BIOS] Nintendo Game Boy Advance Boot ROM
(World)" (https://datomatic.no-intro.org/index.php?page=show_record&s=23&n=xB02):
16384 bytes, CRC32 81977335, SHA-1 300c20df6731a33952ded8c436f7f186d25d3492,
SHA-256 in BIOS_SHA256. The test firmware and the app check the same value.
--check-bios checks a file without preparing data.

The ROM selection and each ROM's criterion are in docs/plans/gba/task-1-1.md
("用例选择"), and for the GBX ROMs and the reference frames in
docs/plans/gba/task-1-2.md ("核对结果"). This tool does not implement
emulation or judge tests.
"""

import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import struct

import gba_reference_frames
import gba_test_roms


ROOT = Path(__file__).resolve().parents[1]
SOURCE_MAGIC = 0xD33DBAE0
WORDS = 40
STATE = struct.Struct(f'<{WORDS}I')
CHUNK = struct.Struct('<ii')
SOURCE_TRANSACTION = struct.Struct('<6I')
HEADER = struct.Struct('<8sII')
INDEX = struct.Struct('<32sIIQQQ')
MASK = struct.Struct('<Q')
TAIL = struct.Struct('<IIB')
TRANSACTION = struct.Struct('<BBBBII')
U32 = struct.Struct('<I')
MAGIC = b'GBXARM7\0'
VERSION = 1
KINDS = (0, 1, 2)
SIZES = (1, 2, 4)
BIOS_SIZE = 16384
BIOS_SHA256 = 'fd2547724b505f487e6dcb29ec2ecff3af35a841a77ab2e85fd87350abd36570'

# Directories and files under refs/tests/<suite>/ that go on the card. ROMs
# are the .gba files below them; reasons for each choice are in the task.
ROMS = {
    'jsmolka-gba-tests': {
        'dirs': ('arm', 'thumb', 'memory', 'bios', 'nes', 'save', 'ppu'),
    },
    'alyosha-gba-tests': {
        'dirs': ('Bus', 'DMA', 'Interactions', 'Serial', 'fifo_dma', 'irq', 'prefetcher', 'psr',
                 'timer', 'timing'),
        # No source; see refs/README.md.
        'exclude': ('prefetcher/bounday_test_1.gba',),
        # The tests of arm/, memory/, thumb/ and ppu/ that jsmolka-gba-tests
        # does not have, and the top-level STM_ALU_IMM. The other files of
        # these directories equal jsmolka's, except an older arm/arm.gba.
        'files': (
            'arm/Coprocessor_14.gba', 'memory/PAL_8_bit_writes.gba', 'memory/VRAM_8_bit_writes.gba',
            'thumb/blx.gba', 'thumb/cpy.gba', 'thumb/Pop_no_regs.gba', 'thumb/Push_no_regs.gba',
            'STM_ALU_IMM.gba',
            *(f'ppu/{name}.gba' for name in (
                'Sprite_Disable_Midline', 'Sprite_Disable_on_VRAM_Access', 'Sprite_Last_VRAM_Access',
                'Sprite_Last_VRAM_Access_Free', 'start_up', 'start_up_vbl', 'start_up_vbl_irq',
                'start_up_vbl_irq_halt', 'stripes_x_offset'))),
    },
    'nba-hw-test': {
        'dirs': ('bus', 'dma', 'haltcnt', 'irq', 'timer'),
        'files': tuple(f'ppu/{name}/{name}.gba' for name in (
            'status-irq-dma', 'vram-mirror', 'bgpd', 'bgx', 'dispcnt-latch', 'greenswap',
            'sprite-hmosaic')),
        # Photos or resized captures, for manual checks only.
        'references': tuple(f'ppu/{name}/expected.png' for name in (
            'bgpd', 'bgx', 'dispcnt-latch', 'greenswap', 'sprite-hmosaic')),
    },
}
SUITE_FILES = ('README.md', 'LICENSE')


def read_source(path):
    """Yields (initial, final, transactions, opcode, base_addr) per case."""
    data = path.read_bytes()
    magic, count = struct.unpack_from('<II', data, 0)
    if magic != SOURCE_MAGIC:
        raise ValueError(f'Bad magic: {path.name}')
    offset = 8
    for index in range(count):
        size, = struct.unpack_from('<i', data, offset)
        end = offset + size
        at = offset + 4
        states = []
        for tag in (1, 2):
            chunk, found = CHUNK.unpack_from(data, at)
            if (chunk, found) != (CHUNK.size + STATE.size, tag):
                raise ValueError(f'Bad state chunk: {path.name}: {index}')
            states.append(STATE.unpack_from(data, at + CHUNK.size))
            at += chunk
        chunk, found, transactions = struct.unpack_from('<iii', data, at)
        if found != 3 or chunk != 12 + SOURCE_TRANSACTION.size * transactions:
            raise ValueError(f'Bad transaction chunk: {path.name}: {index}')
        accesses = [SOURCE_TRANSACTION.unpack_from(data, at + 12 + SOURCE_TRANSACTION.size * i)
                    for i in range(transactions)]
        at += chunk
        chunk, found = CHUNK.unpack_from(data, at)
        if (chunk, found) != (CHUNK.size + 8, 4):
            raise ValueError(f'Bad opcode chunk: {path.name}: {index}')
        opcode, base = struct.unpack_from('<II', data, at + CHUNK.size)
        at += chunk
        if at != end:
            raise ValueError(f'Case size mismatch: {path.name}: {index}')
        check_meaning(path.name, index, states[0], states[1], accesses, opcode, base)
        yield states[0], states[1], accesses, opcode, base
        offset = end
    if offset != len(data):
        raise ValueError(f'Trailing data: {path.name}')


def check_meaning(name, index, initial, final, transactions, opcode, base):
    """Checks the field meanings the module documentation marks "data"."""
    thumb = initial[31] >> 5 & 1
    if (initial[15] - base) & 0xFFFFFFFF != (4 if thumb else 8) or initial[37] != opcode:
        raise ValueError(f'Unexpected R15 or pipeline: {name}: {index}')
    if initial[39] not in (2, 3) or final[39] not in (2, 3):
        raise ValueError(f'Unexpected state access: {name}: {index}')
    cycle = 1
    for kind, size, address, value, now, access in transactions:
        allowed = {0: (2, 3), 1: (0, 1), 2: (0, 1, 8)}.get(kind, ())
        if size not in SIZES or access not in allowed or not cycle <= now <= 255:
            raise ValueError(f'Unexpected transaction: {name}: {index}')
        cycle = now
        if kind == 0 and value != (opcode if address == base else address & ((1 << 8 * size) - 1)):
            raise ValueError(f'Unexpected fetch data: {name}: {index}')


def encode_case(initial, final, transactions, opcode, base):
    mask = 0
    changed = []
    for i, (before, after) in enumerate(zip(initial, final)):
        if before != after:
            mask |= 1 << i
            changed.append(after)
    if len(transactions) > 255:
        raise ValueError('Too many transactions')
    data = bytearray(STATE.pack(*initial))
    data += MASK.pack(mask)
    data += struct.pack(f'<{len(changed)}I', *changed)
    data += TAIL.pack(opcode, base, len(transactions))
    for kind, size, address, value, cycle, access in transactions:
        if kind not in KINDS or size not in SIZES or access > 15 or cycle > 255:
            raise ValueError(f'Unsupported transaction: {kind} {size} {access} {cycle}')
        data += TRANSACTION.pack(kind, size, access, cycle, address, value)
    return data


def decode_case(data, offset=0):
    """Decodes a record into the source's tuple form; returns it and its end."""
    initial = STATE.unpack_from(data, offset)
    offset += STATE.size
    mask, = MASK.unpack_from(data, offset)
    offset += MASK.size
    final = list(initial)
    for i in range(WORDS):
        if mask & (1 << i):
            final[i], = U32.unpack_from(data, offset)
            offset += 4
    if mask >> WORDS:
        raise ValueError('Bad final mask')
    opcode, base, count = TAIL.unpack_from(data, offset)
    offset += TAIL.size
    transactions = []
    for _ in range(count):
        kind, size, access, cycle, address, value = TRANSACTION.unpack_from(data, offset)
        offset += TRANSACTION.size
        transactions.append((kind, size, address, value, cycle, access))
    return (initial, tuple(final), transactions, opcode, base), offset


class Pack:
    """Reads cases.bin the way the board does: the index, then single cases."""

    def __init__(self, path):
        self.stream = path.open('rb')
        magic, version, count = HEADER.unpack(self.stream.read(HEADER.size))
        if magic != MAGIC or version != VERSION:
            raise ValueError('Unsupported ARM7TDMI pack')
        self.files = {}
        for _ in range(count):
            name, cases, reserved, offsets, records, size = INDEX.unpack(self.stream.read(INDEX.size))
            if reserved:
                raise ValueError('Bad index entry')
            self.files[name.rstrip(b'\0').decode()] = (cases, offsets, records, size)

    def case(self, name, index):
        cases, offsets, records, size = self.files[name]
        if not 0 <= index < cases:
            raise IndexError(index)
        self.stream.seek(offsets + 4 * index)
        bounds = self.stream.read(8 if index + 1 < cases else 4)
        start, = U32.unpack_from(bounds)
        end = U32.unpack_from(bounds, 4)[0] if index + 1 < cases else size
        self.stream.seek(records + start)
        record = self.stream.read(end - start)
        case, used = decode_case(record)
        if used != len(record):
            raise ValueError(f'Record length mismatch: {name}: {index}')
        return case

    def close(self):
        self.stream.close()


def pack_arm7tdmi(source, destination):
    paths = sorted((source / 'v1').glob('*.json.bin'), key=lambda path: path.name)
    if not paths:
        raise ValueError('No ARM7TDMI case files')
    names = [path.name.removesuffix('.json.bin') for path in paths]
    if any(len(name.encode()) > 32 for name in names):
        raise ValueError('File name too long for the index')
    index = []
    with destination.open('xb') as stream:
        stream.write(HEADER.pack(MAGIC, VERSION, len(paths)))
        stream.write(bytes(INDEX.size * len(paths)))
        for path, name in zip(paths, names):
            offsets = []
            records = bytearray()
            for case in read_source(path):
                offsets.append(len(records))
                records += encode_case(*case)
            table = stream.tell()
            stream.write(struct.pack(f'<{len(offsets)}I', *offsets))
            start = stream.tell()
            stream.write(records)
            index.append((name.encode(), len(offsets), 0, table, start, len(records)))
            print(f'  {name}: {len(offsets)} cases, {len(records)} bytes', flush=True)
        stream.seek(HEADER.size)
        for entry in index:
            stream.write(INDEX.pack(*entry))

    # Read the written file back, sequentially through the offset tables,
    # and compare every case with a fresh parse of its source.
    verified = 0
    with destination.open('rb') as stream:
        data = stream.read()
    if HEADER.unpack_from(data) != (MAGIC, VERSION, len(paths)):
        raise ValueError('Pack header verification failed')
    expected_offset = HEADER.size + INDEX.size * len(paths)
    for i, (path, name) in enumerate(zip(paths, names)):
        stored = INDEX.unpack_from(data, HEADER.size + INDEX.size * i)
        entry_name, count, reserved, table, start, size = stored
        if entry_name.rstrip(b'\0').decode() != name or reserved or table != expected_offset:
            raise ValueError(f'Index verification failed: {name}')
        offsets = struct.unpack_from(f'<{count}I', data, table)
        if start != table + 4 * count:
            raise ValueError(f'Index verification failed: {name}')
        cases = 0
        for k, case in enumerate(read_source(path)):
            if k >= count:
                raise ValueError(f'Case count mismatch: {name}')
            end = offsets[k + 1] if k + 1 < count else size
            decoded, used = decode_case(data, start + offsets[k])
            if used != start + end or decoded != (case[0], case[1], list(case[2]), case[3], case[4]):
                raise ValueError(f'Round-trip mismatch: {name}: {k}')
            cases += 1
        if cases != count:
            raise ValueError(f'Case count mismatch: {name}')
        verified += cases
        expected_offset = start + size
    if expected_offset != len(data):
        raise ValueError('Pack size verification failed')
    # Check the random-access path the board uses on a few cases per file.
    pack = Pack(destination)
    try:
        for path, name in zip(paths, names):
            count = pack.files[name][0]
            wanted = {0, count // 2, count - 1}
            for k, case in enumerate(read_source(path)):
                if k in wanted and pack.case(name, k) != (case[0], case[1], list(case[2]), case[3], case[4]):
                    raise ValueError(f'Random access mismatch: {name}: {k}')
    finally:
        pack.close()
    return len(paths), verified


def selected_roms(suite, root):
    rule = ROMS[suite]
    files = set(rule.get('files', ()))
    for directory in rule.get('dirs', ()):
        files.update(path.relative_to(root).as_posix() for path in (root / directory).rglob('*.gba'))
    files.difference_update(rule.get('exclude', ()))
    files.update(rule.get('references', ()))
    files.update(SUITE_FILES)
    missing = sorted(name for name in files if not (root / name).is_file())
    if missing:
        raise ValueError(f'Missing files in {suite}: {missing}')
    return sorted(files)


def check_bios(path):
    data = path.read_bytes()
    digest = hashlib.sha256(data).hexdigest()
    if len(data) != BIOS_SIZE or digest != BIOS_SHA256:
        raise SystemExit(f'{path}: not the No-Intro GBA BIOS ({len(data)} bytes, SHA-256 {digest})')
    print(f'{path}: matches No-Intro xB02, SHA-256 {digest}')


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('output', type=Path, nargs='?', default=ROOT / 'tests/sdcard/gba',
                        help='Directory for the contents of the card\'s tests/gba/, replaced on success '
                             '(default: tests/sdcard/gba)')
    parser.add_argument('--arm7tdmi-source', type=Path, default=ROOT / 'refs/tests/arm7tdmi',
                        help='ARM7TDMI suite directory; custom sources require revision.json')
    parser.add_argument('--check-bios', type=Path, metavar='FILE', help='Only check a BIOS dump and exit')
    parser.add_argument('--toolchain', type=Path, help='Arm GNU Toolchain for the GBX ROMs, see gba_test_roms.py')
    args = parser.parse_args()
    if args.check_bios:
        check_bios(args.check_bios)
        return
    refs = ROOT / 'refs/tests'
    fetch = (ROOT / 'refs/fetch.sh').read_text()
    pins = {}
    for suite in ('arm7tdmi', *ROMS):
        found = re.search(rf'^(?:tarball|codeberg)\s+tests/{re.escape(suite)}\s+(\S+)\s+(\S+)', fetch, re.M)
        if not found:
            raise ValueError(f'Reference version of {suite} not found in refs/fetch.sh')
        pins[suite] = found.groups()

    # A copy of the suite other than refs/tests/arm7tdmi needs its own
    # provenance, as prepare_gb_tests.py requires for SM83.
    revision_path = args.arm7tdmi_source / 'revision.json'
    revision_data = None
    if revision_path.exists():
        revision_data = revision_path.read_bytes()
        metadata = json.loads(revision_data)
        upstream = metadata.get('upstream')
        revision = metadata.get('revision')
        match = (re.fullmatch(r'([\w.-]+/[\w.-]+)@([0-9a-fA-F]{40})', upstream)
                 if isinstance(upstream, str) else None)
        if not match or not isinstance(revision, str) or not re.fullmatch(r'[\w.-]+', revision):
            raise ValueError('Invalid ARM7TDMI revision.json: expected revision and upstream repository@commit')
        pins['arm7tdmi'] = match.groups()
    else:
        if args.arm7tdmi_source.resolve() != (refs / 'arm7tdmi').resolve():
            raise ValueError('Custom ARM7TDMI source requires revision.json with revision and upstream repository@commit')
        revision = 'upstream'

    frames = gba_reference_frames.check()
    frames_header = {}
    for line in (gba_reference_frames.FRAMES / 'frames.tsv').read_text().splitlines():
        if line.startswith('# '):
            key, _, value = line[2:].partition('\t')
            frames_header[key] = value
    repository_commit = gba_reference_frames.repository_commit()

    # Build next to the target and replace it only when everything succeeded.
    destination = args.output
    args.output = destination.with_name(destination.name + '.new')
    shutil.rmtree(args.output, ignore_errors=True)
    args.output.mkdir(parents=True)
    arm_output = args.output / 'arm7tdmi'
    arm_output.mkdir()
    files, cases = pack_arm7tdmi(args.arm7tdmi_source, arm_output / 'cases.bin')
    print(f'Packed and verified {cases} cases in {files} files.', flush=True)
    for name in SUITE_FILES:
        shutil.copyfile(args.arm7tdmi_source / name, arm_output / name)
    if revision_data is not None:
        (arm_output / revision_path.name).write_bytes(revision_data)

    # Sources of ROMs: each copies its files below roms/.
    for suite in ROMS:
        root = refs / suite
        for name in selected_roms(suite, root):
            target = args.output / 'roms' / suite / name
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(root / name, target)
    gbx = gba_test_roms.build_all(args.output / 'roms/gbx', args.toolchain)
    print(f'Built {len(gbx)} GBX ROMs.', flush=True)

    for suite, rom, digest in frames:
        if hashlib.sha256((args.output / 'roms' / suite / rom).read_bytes()).hexdigest() != digest:
            raise ValueError(f'{suite}/{rom} differs from the ROM its reference frame was taken from')
        name = Path(suite) / Path(rom).with_suffix('.png')
        (args.output / 'frames' / name).parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(gba_reference_frames.FRAMES / name, args.output / 'frames' / name)
    shutil.copyfile(gba_reference_frames.FRAMES / 'frames.tsv', args.output / 'frames/frames.tsv')

    # Case-sensitive order by path component, as in prepare_gb_tests.py.
    paths = sorted((path for path in args.output.rglob('*') if path.is_file()),
                   key=lambda path: path.relative_to(args.output).parts)
    lines = ['# GBX file manifest v1']
    for suite, (repository, commit) in pins.items():
        lines.append(f'# source\t{suite}\t{repository}\t{commit}')
    lines += [
        f'# source\tgbx\tzhing2006/esp-gbx\t{repository_commit}',
        f'# toolchain\tgbx\t{gba_test_roms.TOOLCHAIN_VERSION}',
        f'# frames\t{frames_header["reference"]}\tgenerated at {frames_header["repository"]}',
        f'# revision\tarm7tdmi\t{revision}',
        '# selection\tARM7TDMI single-step; jsmolka arm/thumb/memory/bios/nes/save/ppu; '
        'alyosha Bus/DMA/Interactions/Serial/fifo_dma/irq/prefetcher/psr/timer/timing, STM_ALU_IMM '
        'and its own arm/memory/thumb/ppu tests; '
        'nba-hw-test bus/dma/haltcnt/irq/timer and ppu without ram-access-timing; '
        'GBX ppu/save/rtc/rom; reference frames of GBX ppu, jsmolka ppu, alyosha Sprite_Disable_* '
        'and the nba-hw-test ppu tests with expected.png',
        'sha256\tbytes\tpath',
    ]
    for path in paths:
        data = path.read_bytes()
        lines.append(f'{hashlib.sha256(data).hexdigest()}\t{len(data)}\t{path.relative_to(args.output).as_posix()}')
    (args.output / 'manifest.tsv').write_text('\n'.join(lines) + '\n', newline='\n')
    roms = sum(path.suffix == '.gba' for path in paths)
    print(f'Prepared {roms} ROMs and {len(paths) + 1} files including the manifest.', flush=True)
    print(f'ARM7TDMI pack: {(arm_output / "cases.bin").stat().st_size} bytes', flush=True)
    shutil.rmtree(destination, ignore_errors=True)
    args.output.rename(destination)
    print(f'Output: {destination}', flush=True)


if __name__ == '__main__':
    main()

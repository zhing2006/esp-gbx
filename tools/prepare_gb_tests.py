#!/usr/bin/env python3
"""Prepare GB test data from local references without copying JSON to the card.

cases.bin is little-endian, version 1:
  Header <8sII>: magic GBXSM83 followed by NUL, version, opcode group count.
  Index <HHIQQ> per group: opcode, reserved zero, case count, offset, byte size.
  CB opcodes use 0xCB00..0xCBFF; other opcodes use 0x0000..0x00FF.
  Each case starts with a u32 payload size, then a u16 UTF-8 name length/name,
  initial state, final state, and a u16 bus sample count followed by samples.
  State: <HH10B> for PC, SP, A/B/C/D/E/F/H/L/IME and an optional-field mask.
  Mask bits 0/1 indicate IE/EI, each stored as a u8 when present. A u16 RAM
  entry count follows, then <HB> address/value pairs in original order.
  Bus sample <HBB>: address, data, flags. Flag bits 0/1 indicate non-null
  address/data; bits 2/3/4 are read/write/memory. Null values are stored as zero.

The index selects an opcode without scanning the pack. Case lengths permit
skipping individual records. Packing always verifies every decoded case
against its source. This tool does not implement emulation or judge tests.
"""

import argparse
import hashlib
import importlib.util
import json
from pathlib import Path
import re
import shutil
import struct


ROOT = Path(__file__).resolve().parents[1]
HEADER = struct.Struct('<8sII')
INDEX = struct.Struct('<HHIQQ')
STATE = struct.Struct('<HH10B')
RAM = struct.Struct('<HB')
BUS = struct.Struct('<HBB')
U16 = struct.Struct('<H')
U32 = struct.Struct('<I')
MAGIC = b'GBXSM83\0'
REGISTERS = ('pc', 'sp', 'a', 'b', 'c', 'd', 'e', 'f', 'h', 'l', 'ime')
OPTIONAL = ('ie', 'ei')
NON_DMG_C_BOOT_TESTS = {
    'boot_div-S.gb', 'boot_div2-S.gb', 'boot_hwio-S.gb',
    'boot_regs-mgb.gb', 'boot_regs-sgb.gb', 'boot_regs-sgb2.gb',
    'boot_div-dmg0.gb', 'boot_hwio-dmg0.gb', 'boot_regs-dmg0.gb',
}
# Mooneye misc tests for the C group (CGB, AGB, AGS) or CPU CGB E, excluding
# the AGB-only and CGB0-only variants.
CGB_E_MOONEYE_MISC = {
    'boot_div-cgbABCDE.gb', 'boot_hwio-C.gb', 'boot_regs-cgb.gb',
    'unused_hwio-C.gb', 'vblank_stat_intr-C.gb',
}
MEALYBUG_REFERENCES = ('_dmg_blob.png', '_cgb_c.png', '_cgb_d.png')


def indexed_files(suites):
    """The ROMs and references the runner's index uses from these suites,
    as paths relative to game-boy-test-roms."""
    spec = importlib.util.spec_from_file_location('rom_suite', ROOT / 'tests/runner/rom_suite.py')
    rom_suite = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(rom_suite)
    files = set()
    for row in rom_suite.index():
        if row['suite'] in suites:
            files.update(path.removeprefix('roms/') for path in (row['path'], row['reference']) if path)
    return files


# AGE and Gambatte: the files the index uses; it selects them by model.
INDEXED = None


def encode_state(state):
    allowed = set(REGISTERS) | set(OPTIONAL) | {'ram'}
    if set(state) - allowed:
        raise ValueError(f'Unsupported state fields: {set(state) - allowed}')
    mask = sum(1 << i for i, name in enumerate(OPTIONAL) if name in state)
    data = bytearray(STATE.pack(*(state[name] for name in REGISTERS), mask))
    for name in OPTIONAL:
        if name in state:
            data.append(state[name])
    data.extend(U16.pack(len(state['ram'])))
    for address, value in state['ram']:
        data.extend(RAM.pack(address, value))
    return data


def encode_case(case):
    if set(case) != {'name', 'initial', 'final', 'cycles'}:
        raise ValueError(f'Unsupported case fields: {set(case)}')
    name = case['name'].encode('utf-8')
    data = bytearray(U16.pack(len(name)))
    data.extend(name)
    data.extend(encode_state(case['initial']))
    data.extend(encode_state(case['final']))
    data.extend(U16.pack(len(case['cycles'])))
    for address, value, signals in case['cycles']:
        if len(signals) != 3 or any(s not in ('-', expected) for s, expected in zip(signals, 'rwm')):
            raise ValueError(f'Unsupported bus signals: {signals}')
        flags = int(address is not None) | (int(value is not None) << 1)
        flags |= sum(1 << (i + 2) for i, signal in enumerate(signals) if signal != '-')
        data.extend(BUS.pack(address if address is not None else 0,
                             value if value is not None else 0, flags))
    return U32.pack(len(data)) + data


class Reader:
    def __init__(self, data):
        self.data = data
        self.offset = 0

    def unpack(self, layout):
        values = layout.unpack_from(self.data, self.offset)
        self.offset += layout.size
        return values

    def state(self):
        *values, mask = self.unpack(STATE)
        state = dict(zip(REGISTERS, values))
        for i, name in enumerate(OPTIONAL):
            if mask & (1 << i):
                state[name] = self.data[self.offset]
                self.offset += 1
        count, = self.unpack(U16)
        state['ram'] = [list(self.unpack(RAM)) for _ in range(count)]
        return state

    def case(self):
        length, = self.unpack(U32)
        end = self.offset + length
        name_size, = self.unpack(U16)
        name = self.data[self.offset:self.offset + name_size].decode('utf-8')
        self.offset += name_size
        initial = self.state()
        final = self.state()
        count, = self.unpack(U16)
        cycles = []
        for _ in range(count):
            address, value, flags = self.unpack(BUS)
            signals = ''.join(s if flags & (1 << (i + 2)) else '-' for i, s in enumerate('rwm'))
            cycles.append([address if flags & 1 else None, value if flags & 2 else None, signals])
        if self.offset != end:
            raise ValueError('Case length mismatch')
        return {'name': name, 'initial': initial, 'final': final, 'cycles': cycles}


def opcode(path):
    parts = path.stem.split()
    if len(parts) == 1:
        return int(parts[0], 16)
    if len(parts) == 2 and parts[0] == 'cb':
        return 0xCB00 | int(parts[1], 16)
    raise ValueError(f'Unsupported opcode filename: {path.name}')


def pack_sm83(source, destination):
    paths = sorted((source / 'v1').glob('*.json'), key=opcode)
    if not paths or len({opcode(path) for path in paths}) != len(paths):
        raise ValueError('Missing or duplicate opcode groups')
    index = []
    with destination.open('xb') as stream:
        stream.write(HEADER.pack(MAGIC, 1, len(paths)))
        stream.write(bytes(INDEX.size * len(paths)))
        for path in paths:
            cases = json.loads(path.read_bytes())
            start = stream.tell()
            for case in cases:
                stream.write(encode_case(case))
            index.append((opcode(path), 0, len(cases), start, stream.tell() - start))
        stream.seek(HEADER.size)
        for entry in index:
            stream.write(INDEX.pack(*entry))

    # Read the on-disk index and records, not the encoder's in-memory output.
    verified = 0
    with destination.open('rb') as stream:
        if HEADER.unpack(stream.read(HEADER.size)) != (MAGIC, 1, len(paths)):
            raise ValueError('Pack header verification failed')
        stored_index = [INDEX.unpack(stream.read(INDEX.size)) for _ in paths]
        expected_offset = HEADER.size + INDEX.size * len(paths)
        for path, entry in zip(paths, stored_index):
            code, reserved, count, offset, size = entry
            cases = json.loads(path.read_bytes())
            if code != opcode(path) or reserved != 0 or count != len(cases) or offset != expected_offset:
                raise ValueError(f'Index verification failed: {path.name}')
            stream.seek(offset)
            block = stream.read(size)
            reader = Reader(block)
            for case in cases:
                if reader.case() != case:
                    raise ValueError(f'Round-trip mismatch: {path.name}: {case["name"]}')
                verified += 1
            if reader.offset != size:
                raise ValueError(f'Group length mismatch: {path.name}')
            expected_offset += size
        if expected_offset != destination.stat().st_size:
            raise ValueError('Pack size verification failed')
    return len(paths), verified


def include_rom_file(relative):
    parts = relative.parts
    name = relative.name
    if len(parts) == 1:
        return name in ('README.md', 'CHANGELOG.md')
    if parts[0] in ('blargg', 'dmg-acid2', 'cgb-acid2', 'cgb-acid-hell'):
        return True
    if parts[0] == 'mealybug-tearoom-tests':
        if len(parts) == 2:
            return name in ('README.md', 'LICENSE', 'game-boy-test-roms-howto.md')
        if name.endswith(MEALYBUG_REFERENCES):
            return True
        # ROMs without a DMG-blob, CGB C or CGB D reference cannot be judged.
        source = ROOT / 'refs/tests/game-boy-test-roms' / relative
        return name.endswith('.gb') and any(source.with_name(source.stem + suffix).is_file()
                                            for suffix in MEALYBUG_REFERENCES)
    if parts[0] == 'mooneye-test-suite':
        if len(parts) == 2:
            return name in ('README.markdown', 'game-boy-test-roms-howto.md')
        if parts[1] == 'misc':
            return name in CGB_E_MOONEYE_MISC
        return parts[1] in ('acceptance', 'emulator-only') and name not in NON_DMG_C_BOOT_TESTS
    if parts[0] in ('mbc3-tester', 'rtc3test'):
        return name.endswith(('.gb', '-dmg.png', '.md'))
    if parts[0] in ('age-test-roms', 'gambatte'):
        if len(parts) == 2:
            return name in ('README.md', 'README', 'game-boy-test-roms-howto.md')
        global INDEXED
        if INDEXED is None:
            INDEXED = indexed_files({'age-test-roms', 'gambatte'})
        return relative.as_posix() in INDEXED
    if parts[0] == 'same-suite':
        if len(parts) == 2:
            return name in ('README.md', 'game-boy-test-roms-howto.md')
        if parts[1] == 'dma' or relative.as_posix() == 'same-suite/ppu/blocking_bgpi_increase.gb':
            return name.endswith('.gb')
        if parts[1] == 'apu':
            # Per the suite's notes, unsuffixed APU tests work only on CPU CGB E.
            revision = re.search(r'-(\w+)\.gb$', name)
            return name.endswith('.gb') and (not revision or (revision[1].startswith('cgb') and
                                                               'E' in revision[1][3:]))
    return False


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('output', type=Path, nargs='?', default=ROOT / 'tests/sdcard/gb',
                        help='Directory for the contents of the card\'s tests/gb/, replaced on success '
                             '(default: tests/sdcard/gb)')
    parser.add_argument('--sm83-source', type=Path, default=ROOT / 'refs/tests/sm83',
                        help='SM83 suite directory; custom sources require revision.json')
    args = parser.parse_args()
    refs = ROOT / 'refs/tests'
    fetch = (ROOT / 'refs/fetch.sh').read_text()
    sm83_source = re.search(r'tarball\s+tests/sm83\s+(\S+)\s+(\S+)', fetch)
    rom_source = re.search(r'archive\s+tests/game-boy-test-roms\s+(\S+)', fetch)
    if not sm83_source or not rom_source:
        raise ValueError('Reference versions not found in refs/fetch.sh')
    revision_path = args.sm83_source / 'revision.json'
    revision_data = None
    if revision_path.exists():
        # A retained revision can predate the current fetch pin. Use its own
        # provenance, and copy the same metadata bytes into the output pack.
        revision_data = revision_path.read_bytes()
        metadata = json.loads(revision_data)
        upstream = metadata.get('upstream')
        revision = metadata.get('revision')
        source_match = (re.fullmatch(r'([\w.-]+/[\w.-]+)@([0-9a-fA-F]{40})', upstream)
                        if isinstance(upstream, str) else None)
        if not source_match or not isinstance(revision, str) or not re.fullmatch(r'[\w.-]+', revision):
            raise ValueError('Invalid SM83 revision.json: expected revision and upstream repository@commit')
        sm83_repo, sm83_commit = source_match.groups()
    else:
        if args.sm83_source.resolve() != (refs / 'sm83').resolve():
            raise ValueError('Custom SM83 source requires revision.json with revision and upstream repository@commit')
        sm83_repo, sm83_commit = sm83_source.groups()
        revision = 'upstream'
    # Build next to the target and replace it only when everything succeeded.
    destination = args.output
    args.output = destination.with_name(destination.name + '.new')
    shutil.rmtree(args.output, ignore_errors=True)
    args.output.mkdir(parents=True)
    sm83_output = args.output / 'sm83'
    sm83_output.mkdir()
    groups, cases = pack_sm83(args.sm83_source, sm83_output / 'cases.bin')
    print(f'Packed and round-trip verified {cases} cases in {groups} opcode groups.', flush=True)
    for name in ('README.MD', 'LICENSE'):
        shutil.copyfile(args.sm83_source / name, sm83_output / name)
    if revision_data is not None:
        (sm83_output / revision_path.name).write_bytes(revision_data)
    print(f'SM83 revision: {revision}', flush=True)
    roms = refs / 'game-boy-test-roms'
    for path in sorted(roms.rglob('*')):
        if path.is_file() and include_rom_file(path.relative_to(roms)):
            target = args.output / 'roms' / path.relative_to(roms)
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(path, target)
    # Case-sensitive order by path component, as POSIX paths compare. Windows
    # paths compare case-insensitively, which would change the manifest bytes.
    files = sorted((path for path in args.output.rglob('*') if path.is_file()),
                   key=lambda path: path.relative_to(args.output).parts)
    lines = [
        '# GBX file manifest v1',
        f'# source\tsm83\t{sm83_repo}\t{sm83_commit}',
        f'# revision\tsm83\t{revision}',
        f'# source\troms\t{rom_source[1]}',
        '# selection\tDMG-C: SM83, Blargg DMG, Mooneye acceptance/emulator-only, dmg-acid2, Mealybug DMG-blob, MBC3 tester, rtc3test; '
        'CGB E: Blargg CGB, Mooneye acceptance/misc, cgb-acid2, cgb-acid-hell, dmg-acid2 CGB, Mealybug CGB C/D, '
        'AGE speed-switch, SameSuite dma/ppu/apu; both: AGE STAT/LY/HALT/OAM/mode 3, '
        'Gambatte STAT/sprites/window/oam_access',
        'sha256\tbytes\tpath',
    ]
    for path in files:
        data = path.read_bytes()
        lines.append(f'{hashlib.sha256(data).hexdigest()}\t{len(data)}\t{path.relative_to(args.output).as_posix()}')
    (args.output / 'manifest.tsv').write_text('\n'.join(lines) + '\n', newline='\n')
    rom_count = sum(path.suffix.lower() in ('.gb', '.gbc') for path in files)
    print(f'Prepared {rom_count} ROMs and {len(files) + 1} files including the manifest.', flush=True)
    print(f'SM83 pack: {(sm83_output / "cases.bin").stat().st_size} bytes', flush=True)
    shutil.rmtree(destination, ignore_errors=True)
    args.output.rename(destination)
    print(f'Output: {destination}', flush=True)


if __name__ == '__main__':
    main()

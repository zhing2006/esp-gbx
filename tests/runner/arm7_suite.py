"""Match the local ARM7TDMI suite to the prepared SD manifest and read its cases."""

import hashlib
import json
from pathlib import Path
import re
import struct
import sys


ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
import prepare_gba_tests as pack  # noqa: E402
SOURCE_MAGIC = 0xD33DBAE0
WORDS = 40
STATE = struct.Struct(f'<{WORDS}I')
TRANSACTION = struct.Struct('<6I')
# The 40 state words of a case, in the suite's order (tools/prepare_gba_tests.py).
WORD_NAMES = (tuple(f'r{i}' for i in range(16)) + tuple(f'r{i}_fiq' for i in range(8, 15)) +
              tuple(f'r{i}_{mode}' for mode in ('svc', 'abt', 'irq', 'und') for i in (13, 14)) +
              ('cpsr',) + tuple(f'spsr_{mode}' for mode in ('fiq', 'svc', 'abt', 'irq', 'und')) +
              ('pipeline[0]', 'pipeline[1]', 'access'))
CPSR = WORD_NAMES.index('cpsr')
JUDGED, R15_WRITE, CP14 = range(3)
CLASSES = ('judged', 'r15_write', 'cp14')


def digest(data):
    return hashlib.sha256(data).hexdigest()


def condition_passes(condition, flags):
    n, z, c, v = flags >> 3 & 1, flags >> 2 & 1, flags >> 1 & 1, flags & 1
    return (z, not z, c, not c, n, not n, v, not v, c and not z, not c or z, n == v, n != v,
            not z and n == v, z or n != v, True, False)[condition]


def classify(opcode, cpsr):
    """The class of tests/protocol/arm7_execution.h, decoded as src/gba/executor.h does."""
    if cpsr & 0x20 or not condition_passes(opcode >> 28, cpsr >> 28):
        return JUDGED
    rn, rd = opcode >> 16 & 15, opcode >> 12 & 15
    writeback = not opcode >> 24 & 1 or opcode >> 21 & 1
    base_write = rn == 15 and writeback and not (opcode >> 20 & 1 and rd == 15)
    group = opcode >> 25 & 7
    if group == 0:
        if opcode & 0x90 == 0x90:
            return R15_WRITE if opcode & 0x60 and base_write else JUDGED
        if opcode & 0x01900000 == 0x01000000 and opcode & 0x0ffffff0 != 0x012fff10:
            return R15_WRITE if not opcode >> 21 & 1 and rd == 15 else JUDGED
        return JUDGED
    if group == 1:
        return R15_WRITE if opcode & 0x01b00000 == 0x01000000 and rd == 15 else JUDGED
    if group == 2 or (group == 3 and not opcode & 0x10):
        return R15_WRITE if base_write else JUDGED
    if group == 7:
        return CP14 if not opcode >> 24 & 1 and opcode & 0x10 and opcode >> 8 & 15 == 14 else JUDGED
    return JUDGED


class CaseFile:
    """One source file, parsed one case at a time as results need them."""

    def __init__(self, raw, name):
        magic, count = struct.unpack_from('<II', raw)
        if magic != SOURCE_MAGIC:
            raise ValueError(f'Bad magic: {name}')
        self.raw, self.name, self.offsets = raw, name, []
        offset = 8
        for _ in range(count):
            self.offsets.append(offset)
            offset += struct.unpack_from('<i', raw, offset)[0]
        if offset != len(raw):
            raise ValueError(f'Trailing data: {name}')

    def __len__(self):
        return len(self.offsets)

    def classes(self):
        """The class of every case, read from its opcode and initial CPSR alone."""
        if not hasattr(self, '_classes'):
            raw, cpsr = self.raw, 12 + 4 * CPSR
            ends = self.offsets[1:] + [len(raw)]
            self._classes = bytes(classify(struct.unpack_from('<I', raw, end - 8)[0],
                                           struct.unpack_from('<I', raw, start + cpsr)[0])
                                  for start, end in zip(self.offsets, ends))
        return self._classes

    def __getitem__(self, index):
        """The case's chunks are checked by tools/prepare_gba_tests.py when packing."""
        at = self.offsets[index] + 12
        initial = STATE.unpack_from(self.raw, at)
        final = STATE.unpack_from(self.raw, at + STATE.size + 8)
        at += 2 * (STATE.size + 8)  # The transaction chunk's count.
        count, = struct.unpack_from('<i', self.raw, at)
        transactions = [TRANSACTION.unpack_from(self.raw, at + 4 + TRANSACTION.size * i) for i in range(count)]
        opcode, base = struct.unpack_from('<II', self.raw, at + 4 + TRANSACTION.size * count + 8)
        # Source order: kind, size, address, data, cycle, access.
        return {'initial': initial, 'final': final, 'transactions': transactions, 'opcode': opcode, 'base': base}


class Suite:
    def __init__(self, source, data):
        self.source = source.resolve()
        self.data = data.resolve()
        manifest = (data / 'manifest.tsv').read_bytes()
        self.manifest_sha256 = digest(manifest)
        lines = manifest.decode('utf-8').splitlines()
        if not lines or lines[0] != '# GBX file manifest v1':
            raise ValueError('Unsupported data manifest')
        self.files = {}
        sources, revisions = [], []
        for line in lines[1:]:
            fields = line.split('\t')
            if fields[:2] == ['# source', 'arm7tdmi']:
                sources.append(fields[2:])
            elif fields[:2] == ['# revision', 'arm7tdmi']:
                revisions.append(fields[2:])
            elif not line.startswith('#') and fields != ['sha256', 'bytes', 'path']:
                if len(fields) != 3 or not re.fullmatch(r'[0-9a-f]{64}', fields[0]) or fields[2] in self.files:
                    raise ValueError('Invalid or duplicate manifest entry')
                self.files[fields[2]] = (fields[0], int(fields[1]))
        if len(sources) != 1 or len(sources[0]) != 2 or len(revisions) != 1 or len(revisions[0]) != 1:
            raise ValueError('Missing or duplicate ARM7TDMI provenance')
        # The local pack must be the card's: its index maps files to the board's
        # file numbers, and its records are what the board executes.
        self.pack_path = data / 'arm7tdmi/cases.bin'
        with self.pack_path.open('rb') as stream:
            if self.files.get('arm7tdmi/cases.bin') != (hashlib.file_digest(stream, 'sha256').hexdigest(),
                                                       stream.tell()):
                raise ValueError('Prepared cases.bin differs from manifest')
            stream.seek(0)
            magic, version, count = pack.HEADER.unpack(stream.read(pack.HEADER.size))
            if magic != pack.MAGIC or version != pack.VERSION:
                raise ValueError('Unsupported ARM7TDMI pack')
            self.pack_files = {}
            for number in range(count):
                name, cases, _, table, records, size = pack.INDEX.unpack(stream.read(pack.INDEX.size))
                self.pack_files[name.rstrip(b'\0').decode()] = (number, cases, table, size)
        self.upstream = '@'.join(sources[0])
        self.revision = revisions[0][0]
        if (source / 'revision.json').exists():
            raw = (source / 'revision.json').read_bytes()
            metadata = json.loads(raw)
            if metadata.get('upstream') != self.upstream or metadata.get('revision') != self.revision:
                raise ValueError('ARM7TDMI revision metadata does not match the manifest')
            self.check_bytes('arm7tdmi/revision.json', raw)
        else:
            fetch = (ROOT / 'refs/fetch.sh').read_text()
            match = re.search(r'tarball\s+tests/arm7tdmi\s+(\S+)\s+(\S+)', fetch)
            if (self.source != (ROOT / 'refs/tests/arm7tdmi').resolve() or self.revision != 'upstream' or
                    match is None or '@'.join(match.groups()) != self.upstream or
                    'arm7tdmi/revision.json' in self.files):
                raise ValueError('Unversioned source does not match the upstream manifest')
        for name in ('README.md', 'LICENSE'):
            self.check_bytes(f'arm7tdmi/{name}', (source / name).read_bytes())
        # The pack's file order (tools/prepare_gba_tests.py).
        self.paths = sorted((self.source / 'v1').glob('*.json.bin'), key=lambda path: path.name)
        if not self.paths:
            raise ValueError('No ARM7TDMI case files')
        self.groups = []
        self.case_count = 0

    def check_bytes(self, path, raw):
        if self.files.get(path) != (digest(raw), len(raw)):
            raise ValueError(f'Local expectations differ from manifest: {path}')

    def select(self, names=(), case_ids=(), select_all=False):
        """Selects whole files by name, such as arm_bx, and single cases by ID."""
        requested_names, requested_ids = set(names), {}
        for case_id in case_ids:
            path, _, index = case_id.partition('::')
            if not index.isdigit():
                raise ValueError(f'Invalid case ID: {case_id}')
            requested_ids.setdefault(path, set()).add(int(index))
        names = [path.name.removesuffix('.json.bin') for path in self.paths]
        if sorted(names) != sorted(self.pack_files):
            raise ValueError('Local source files differ from the files in cases.bin')
        found = set()
        for name, path in zip(names, self.paths):
            relative = path.relative_to(self.source).as_posix()
            number, packed, _, _ = self.pack_files[name]
            with path.open('rb') as stream:
                magic, count = struct.unpack('<II', stream.read(8))
            if magic != SOURCE_MAGIC or count != packed:
                raise ValueError(f'Local source differs from cases.bin: {relative}')
            self.case_count += count
            if select_all or name in requested_names:
                indices = list(range(count))
            else:
                indices = sorted(requested_ids.get(relative, ()))
                if indices and indices[-1] >= count:
                    raise ValueError(f'Unknown case: {relative}::{indices[-1]}')
            if name in requested_names:
                found.add(name)
            if relative in requested_ids:
                found.add(relative)
            if indices:
                self.groups.append({'path': relative, 'name': name, 'file': number, 'count': count,
                                    'indices': indices, 'source_sha256': digest(path.read_bytes())})
        unknown = (requested_names | set(requested_ids)) - found
        if unknown:
            raise ValueError(f'Unknown selection: {sorted(unknown)}')
        return sum(len(group['indices']) for group in self.groups)

    def verify(self, group):
        """Encodes the selected cases as the packer does and compares them with the pack's records."""
        _, count, table, size = self.pack_files[group['name']]
        cases = load_cases(self.source, group)
        with self.pack_path.open('rb') as stream:
            stream.seek(table)
            offsets = struct.unpack(f'<{count}I', stream.read(4 * count)) + (size,)
            records = stream.read(size)
        for index in group['indices']:
            case = cases[index]
            if (pack.encode_case(case['initial'], case['final'], case['transactions'], case['opcode'], case['base'])
                    != records[offsets[index]:offsets[index + 1]]):
                raise ValueError(f'Local expectations differ from cases.bin: {group["path"]}::{index}')

    def cases(self, group):
        return load_cases(self.source, group)


def load_cases(source, group):
    """Use the exact expectations recorded by a live or saved selection."""
    raw = (source / group['path']).read_bytes()
    if digest(raw) != group['source_sha256']:
        raise ValueError(f'Source differs from the recorded selection: {group["path"]}')
    cases = CaseFile(raw, group['path'])
    if len(cases) != group['count']:
        raise ValueError(f'Source case count differs from the recorded selection: {group["path"]}')
    return cases

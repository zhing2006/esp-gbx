"""Match local SM83 expectations to the exact prepared SD manifest and pack."""

import hashlib
import importlib.util
import json
from pathlib import Path
import re


ROOT = Path(__file__).resolve().parents[2]
spec = importlib.util.spec_from_file_location('prepare_gb_tests', ROOT / 'tools/prepare_gb_tests.py')
pack = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pack)


def digest(data):
    return hashlib.sha256(data).hexdigest()


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
            if fields[:2] == ['# source', 'sm83']:
                sources.append(fields[2:])
            elif fields[:2] == ['# revision', 'sm83']:
                revisions.append(fields[2:])
            elif not line.startswith('#') and fields != ['sha256', 'bytes', 'path']:
                if len(fields) != 3 or not re.fullmatch(r'[0-9a-f]{64}', fields[0]) or fields[2] in self.files:
                    raise ValueError('Invalid or duplicate manifest entry')
                self.files[fields[2]] = (fields[0], int(fields[1]))
        if len(sources) != 1 or len(sources[0]) != 2 or len(revisions) != 1 or len(revisions[0]) != 1:
            raise ValueError('Missing or duplicate SM83 provenance')
        self.upstream = '@'.join(sources[0])
        self.revision = revisions[0][0]
        if (source / 'revision.json').exists():
            raw = (source / 'revision.json').read_bytes()
            metadata = json.loads(raw)
            if metadata.get('upstream') != self.upstream or metadata.get('revision') != self.revision:
                raise ValueError('SM83 revision metadata does not match the manifest')
            self.check_bytes('sm83/revision.json', raw)
        else:
            fetch = (ROOT / 'refs/fetch.sh').read_text()
            match = re.search(r'tarball\s+tests/sm83\s+(\S+)\s+(\S+)', fetch)
            if (self.source != (ROOT / 'refs/tests/sm83').resolve() or self.revision != 'upstream' or
                    match is None or '@'.join(match.groups()) != self.upstream or 'sm83/revision.json' in self.files):
                raise ValueError('Unversioned source does not match the upstream manifest')
        for name in ('README.MD', 'LICENSE'):
            self.check_bytes(f'sm83/{name}', (source / name).read_bytes())
        self.groups = []
        self.case_count = 0

    def check_bytes(self, path, raw):
        if self.files.get(path) != (digest(raw), len(raw)):
            raise ValueError(f'Local expectations differ from manifest: {path}')

    def verify(self, opcodes=(), case_ids=(), select_all=False):
        """Verify every packed case, not just this run's selected opcodes."""
        paths = sorted((self.source / 'v1').glob('*.json'), key=pack.opcode)
        requested_opcodes, requested_ids = set(opcodes), set(case_ids)
        found_opcodes, found_ids = set(), set()
        with (self.data / 'sm83/cases.bin').open('rb') as stream:
            pack_hash = hashlib.file_digest(stream, 'sha256').hexdigest()
            file_size = stream.seek(0, 2)
            if self.files.get('sm83/cases.bin') != (pack_hash, file_size):
                raise ValueError('Prepared cases.bin differs from manifest')
            stream.seek(0)
            if stream.read(pack.HEADER.size) != pack.HEADER.pack(pack.MAGIC, 1, len(paths)) or not paths:
                raise ValueError('SM83 pack header differs from source')
            entries = [pack.INDEX.unpack(stream.read(pack.INDEX.size)) for _ in paths]
            expected_offset = pack.HEADER.size + pack.INDEX.size * len(paths)
            for path, entry in zip(paths, entries):
                raw = path.read_bytes()
                cases = json.loads(raw)
                opcode, reserved, count, offset, size = entry
                if (opcode != pack.opcode(path) or reserved or count != len(cases) or offset != expected_offset or
                        b''.join(pack.encode_case(case) for case in cases) != stream.read(size)):
                    raise ValueError(f'Packed cases differ from local expectations: {path.name}')
                expected_offset += size
                relative = path.relative_to(self.source).as_posix()
                ids = [f'{relative}::{case["name"]}' for case in cases]
                if len(set(ids)) != len(ids):
                    raise ValueError(f'Duplicate case names in {relative}')
                indices = [i for i, case_id in enumerate(ids)
                           if select_all or opcode in requested_opcodes or case_id in requested_ids]
                found_opcodes.add(opcode)
                found_ids.update(requested_ids.intersection(ids))
                self.case_count += len(cases)
                if indices:
                    self.groups.append({'path': relative, 'opcode': opcode, 'count': count,
                                        'indices': indices, 'source_sha256': digest(raw)})
            if stream.read(1):
                raise ValueError('Trailing data in SM83 pack')
        if requested_opcodes - found_opcodes or requested_ids - found_ids:
            raise ValueError(f'Unknown selection: opcodes={sorted(requested_opcodes - found_opcodes)}, '
                             f'cases={sorted(requested_ids - found_ids)}')
        return sum(len(group['indices']) for group in self.groups)

    def cases(self, group):
        return load_cases(self.source, group)


def load_cases(source, group):
    """Use the exact expectations recorded by a live or saved selection."""
    raw = (source / group['path']).read_bytes()
    if digest(raw) != group['source_sha256']:
        raise ValueError(f'Source differs from the recorded selection: {group["path"]}')
    return json.loads(raw)

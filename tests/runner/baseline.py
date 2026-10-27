"""Reuse suite validation coverage, never interpreter output as golden data."""

import hashlib
import json
from pathlib import Path

from single_step import ROOT


# Runner code that judges or transfers results; a change makes old reports unusable as baselines.
SM83_FILES = ('baseline.py', 'single_step.py', 'execution.py', 'protocol.py', 'transport.py', 'sm83_suite.py',
              'run_sm83.py')
ARM7_FILES = ('baseline.py', 'single_step.py', 'arm7_execution.py', 'arm7_suite.py', 'protocol.py',
              'transport.py', 'run_arm7.py')


def reference_key(device, identity, manifest_sha256, files, **settings):
    """identity names the HELLO field of the system's interpreter identity."""
    if not device.get(identity):
        raise ValueError('Device does not identify its interpreter dependencies')
    runner = {name: hashlib.sha256((ROOT / 'tests/runner' / name).read_bytes()).hexdigest() for name in files}
    return {'device': {name: device[name] for name in
            (identity, 'board', 'chip_revision', 'cpu_hz', 'idf', 'preset', 'execution_version')},
            'manifest_sha256': manifest_sha256, **settings, 'runner': runner}


def validate(directory, key, groups, operation, group_key, requests):
    directory = Path(directory)
    summary = json.loads((directory / 'summary.json').read_text())
    selection_bytes = (directory / 'selection.json').read_bytes()
    selection = json.loads(selection_bytes)
    count = sum(len(group['indices']) for group in selection)
    if (summary.get('operation') != operation or summary.get('oracle') != 'suite' or
            summary.get('status') not in ('PASS', 'FAIL') or summary.get('reference_key') != key or
            count != summary.get('selected') or summary.get('received') != count or
            summary.get('not_run') != 0 or summary.get('interpreter_passed') != count or
            summary.get('completed_requests') != sum(1 for _ in requests(selection)) or
            summary.get('selection_sha256') != hashlib.sha256(selection_bytes).hexdigest()):
        raise ValueError('Interpreter baseline is incompatible or incomplete; run without --baseline to validate it')
    covered = {group[group_key]: group for group in selection}
    for group in groups:
        old = covered.get(group[group_key])
        if (old is None or any(old[name] != group[name] for name in ('path', 'count', 'source_sha256')) or
                not set(group['indices']).issubset(old['indices'])):
            raise ValueError(f'Interpreter baseline does not cover the selection of {group["path"]}')
    return {'report': str(directory.resolve()),
            'summary_sha256': hashlib.sha256((directory / 'summary.json').read_bytes()).hexdigest()}

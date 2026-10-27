#!/usr/bin/env python3
"""Create a local SM83 suite copy with the normal STOP entry PC corrected.

The upstream JSON stays untouched. Only v1/10.json final.pc changes, using
the hardware rule, never emulator output. Conditions: no pending interrupt
and no asserted wake input. See docs/plans/gb/task-1-1.md for the source evidence.
"""

import argparse
import hashlib
import json
from pathlib import Path
import shutil


ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / 'refs/tests/sm83'
SOURCE_COMMIT = 'f9c30210245dd691661db39f5ace022c465ecc2f'
STOP_SHA256 = '647c163b8feace421fceb7863feb247b4305c890e32f0cab4de4f278e68b9a69'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('output', type=Path, help='New directory for the corrected suite')
    args = parser.parse_args()
    original = (SOURCE / 'v1/10.json').read_bytes()
    if hashlib.sha256(original).hexdigest() != STOP_SHA256:
        raise ValueError('STOP source differs from the pinned upstream version; review the correction')
    cases = json.loads(original)
    for case in cases:
        case['final']['pc'] = (case['initial']['pc'] + 2) & 0xffff
    shutil.copytree(SOURCE, args.output)
    (args.output / 'v1/10.json').write_text(json.dumps(cases, separators=(',', ':')) + '\n', newline='\n')
    revision = {
        'revision': 'sm83-stop-pc-v1',
        'upstream': f'SingleStepTests/sm83@{SOURCE_COMMIT}',
        'source_sha256': STOP_SHA256,
        'file': 'v1/10.json',
        'field': 'final.pc',
        'formula': '(initial.pc + 2) & 0xFFFF',
        'conditions': 'No pending interrupt or asserted wake input',
        'cases': len(cases),
    }
    (args.output / 'revision.json').write_text(json.dumps(revision, indent=2) + '\n', newline='\n')
    print(f'Corrected {len(cases)} STOP cases: {args.output} (sm83-stop-pc-v1)')


if __name__ == '__main__':
    main()

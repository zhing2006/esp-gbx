#!/usr/bin/env python3
"""Check committed audio accounting with the mixer worker deliberately held.

Expected counts follow the sample rates, durations and speed adjustments in
one fixture, not an emulator recording. Uses regular device test firmware.
A firmware for one core mixes each batch as it is submitted; it has no
worker to hold, and the check of pending sound is skipped.
"""
import argparse
from datetime import datetime, timezone
import json
from pathlib import Path
import struct
import sys

from protocol import DeviceError, DeviceResetError, Kind, ProtocolError
from single_step import git_state, save_json
from sm83_suite import ROOT
from transport import Link

VERSION = 2
CONTROL = struct.Struct('<HHI')
RESULT = struct.Struct('<HHII')
# Name, expected count, and whether the check needs the worker.
CHECKS = (
    ('Submitted sound counts before the worker runs', 750),
    ('Pending sound is not readable PCM', 0, True),
    ('Rate changes and fractional frames carry across batches', 2135),
    ('Mixing preserves the committed count', 2135),
    ('All committed frames become readable', 2135),
    ('Read transfers one PCM chunk', 256),
    ('Read consumes the reservation once', 1879),
    ('Reset discards old reservations and resets sample phase', 497),
    ('Reset exposes only new PCM', 497),
    ('Reading all reset PCM empties the committed buffer', 0),
    ('Overflow leaves only retained frames committed', 8192),
    ('Retained frames remain readable after overflow', 8192),
    ('Overflow accounts for lost PCM', 1758),
    ('Reading retained PCM leaves no lost-frame reservation', 0),
)


def run(args):
    stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')
    report = args.report or ROOT / 'build/test-reports' / (stamp + '-audio')
    report.mkdir(parents=True, exist_ok=False)
    summary = {'format_version': 1, 'operation': 'audio_accounting', 'status': 'PREPARING',
               'started_utc': stamp, 'runner': git_state(), 'command': sys.argv,
               'test_version': VERSION, 'received': 0, 'failed': 0, 'skipped': 0}
    worker = None
    link = None
    try:
        link = Link.open(args.port, args.baud, args.reset, write_timeout=min(1, args.timeout / 2))
        summary['device'] = link.connect(args.timeout)
        link.ping(timeout=args.timeout)
        if not summary['device']['board_ready']:
            raise ValueError('Device board initialization failed')
        summary['status'] = 'RUNNING'
        link.send(Kind.AUDIO_TEST, CONTROL.pack(VERSION, 0, 1))
        with (report / 'results.jsonl').open('x') as results:
            while True:
                frame = link.receive(args.timeout)
                if frame.kind == Kind.DONE:
                    if (frame.payload != CONTROL.pack(VERSION, 1 + len(CHECKS), 1) or
                            summary['received'] != len(CHECKS)):
                        raise ProtocolError('Incomplete or invalid audio completion')
                    break
                if frame.kind != Kind.DATA or len(frame.payload) != RESULT.size:
                    raise ProtocolError('Invalid audio result frame')
                version, result_id, request, actual = RESULT.unpack(frame.payload)
                received = summary['received'] + (worker is not None)
                if version != VERSION or request != 1 or result_id != received or result_id > len(CHECKS):
                    raise ProtocolError('Wrong audio test version, request or check order')
                link.acknowledge(frame)
                if result_id == 0:
                    if actual not in (0, 1):
                        raise ProtocolError('Invalid mixer worker result')
                    worker = actual == 1
                    summary['worker'] = worker
                    print(f'Mixer worker: {"other core" if worker else "none, one core"}', flush=True)
                    continue
                name, expected, *needs = CHECKS[result_id - 1]
                if needs and not worker:
                    status = 'SKIP'
                else:
                    status = 'PASS' if actual == expected else 'FAIL'
                results.write(json.dumps({'id': result_id - 1, 'name': name, 'expected': expected,
                                          'actual': actual, 'status': status}) + '\n')
                results.flush()
                summary['received'] += 1
                summary['failed'] += status == 'FAIL'
                summary['skipped'] += status == 'SKIP'
                print(f'{status}: {name}; expected={expected} actual={actual}', flush=True)
        summary['status'] = 'FAIL' if summary['failed'] else 'PASS'
        return 1 if summary['failed'] else 0
    except (DeviceError, DeviceResetError, ProtocolError, TimeoutError, OSError, ValueError, ImportError) as error:
        summary.update(status='ERROR', error=str(error))
    except KeyboardInterrupt:
        summary.update(status='INTERRUPTED', error='Stopped by user')
    finally:
        summary['missing_results'] = len(CHECKS) - summary['received']
        if link:
            summary['device_logs'] = list(link.logs)
            link.close()
        save_json(report / 'summary.json', summary)
        print(f'{summary["status"]}. Report: {report}', flush=True)
        if 'error' in summary:
            print(summary['error'], file=sys.stderr)
    return 2


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('port')
    parser.add_argument('--report', type=Path)
    parser.add_argument('--baud', type=int, default=460800)
    parser.add_argument('--timeout', type=float, default=10)
    parser.add_argument('--reset', action='store_true')
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error('Timeout must be positive')
    return run(args)


if __name__ == '__main__':
    raise SystemExit(main())

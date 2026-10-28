#!/usr/bin/env python3
"""Run display pattern checks in the regular device test firmware.

Uses mathematical color/coordinate expectations, not captured emulator frames.
Wire layout and fixture ordering: tests/protocol/display_test.h.
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

VERSION = 4
CONTROL = struct.Struct('<HHI')
RESULT = struct.Struct('<HHII')
GROUPS = (
    ('Copy dispatch', 20, False),
    ('Hardware copy', 20, True),
    ('Fill', 20, False),
    ('Nearest scaling', 12, False),
    ('Smooth constant images', 20, True),
    ('Bilinear interpolation', 4, True),
    ('Damage containment', 4, False),
    ('Game bands', 24, False),
)
RESULT_COUNT = 1 + len(GROUPS)


def run(args):
    stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')
    report = args.report or ROOT / 'build/test-reports' / (stamp + '-display')
    report.mkdir(parents=True, exist_ok=False)
    summary = {'format_version': 1, 'operation': 'display_patterns', 'status': 'PREPARING',
               'started_utc': stamp, 'runner': git_state(), 'command': sys.argv,
               'port': args.port, 'baud': args.baud, 'test_version': VERSION,
               'received': 0, 'passed_groups': 0, 'failed_groups': 0, 'skipped_groups': 0,
               'patterns': 0, 'physical_panel': 'NOT CHECKED'}
    link = None
    accelerated = None
    save_json(report / 'summary.json', summary)
    try:
        link = Link.open(args.port, args.baud, args.reset, write_timeout=min(1, args.timeout / 2))
        summary['device'] = link.connect(args.timeout)
        link.ping(timeout=args.timeout)
        if not summary['device']['board_ready']:
            raise ValueError('Device board initialization failed')
        summary['status'] = 'RUNNING'
        link.send(Kind.DISPLAY_TEST, CONTROL.pack(VERSION, 0, 1))
        with (report / 'actual.bin').open('xb') as raw, (report / 'results.jsonl').open('x') as results:
            while True:
                frame = link.receive(args.timeout)
                if frame.kind == Kind.DONE:
                    if (frame.payload != CONTROL.pack(VERSION, RESULT_COUNT, 1) or
                            summary['received'] != RESULT_COUNT):
                        raise ProtocolError('Incomplete or invalid display completion')
                    break
                if frame.kind != Kind.DATA or len(frame.payload) != RESULT.size:
                    raise ProtocolError('Invalid display result frame')
                version, result_id, request, actual = RESULT.unpack(frame.payload)
                if (version != VERSION or request != 1 or result_id != summary['received'] or
                        result_id >= RESULT_COUNT):
                    raise ProtocolError('Wrong display test version, request or result order')
                if result_id == 0:
                    if actual not in (0, 1):
                        raise ProtocolError('Unknown display backend')
                    accelerated = bool(actual)
                    summary['backend'] = 'PPA/DMA2D' if accelerated else 'CPU'
                    row = {'id': result_id, 'name': 'Display backend', 'actual': summary['backend']}
                    print(f'Display backend: {summary["backend"]}', flush=True)
                else:
                    name, count, hardware_only = GROUPS[result_id - 1]
                    skipped = hardware_only and not accelerated
                    expected = 0 if skipped else (1 << count) - 1
                    status = 'FAIL' if actual != expected else 'SKIP' if skipped else 'PASS'
                    row = {'id': result_id, 'name': name, 'status': status,
                           'expected': expected, 'actual': actual, 'patterns': 0 if skipped else count}
                    summary[{'PASS': 'passed_groups', 'FAIL': 'failed_groups',
                             'SKIP': 'skipped_groups'}[status]] += 1
                    summary['patterns'] += row['patterns']
                    print(f'{status}: {name}; expected=0x{expected:08x} actual=0x{actual:08x}', flush=True)
                raw.write(struct.pack('<II', frame.sequence, len(frame.payload)) + frame.payload)
                results.write(json.dumps(row) + '\n')
                raw.flush()
                results.flush()
                link.acknowledge(frame)
                summary['received'] += 1
        summary['status'] = 'FAIL' if summary['failed_groups'] else 'PASS'
        return 1 if summary['failed_groups'] else 0
    except DeviceResetError as error:
        summary.update(status='DEVICE RESET', error=str(error))
    except TimeoutError as error:
        summary.update(status='TIMEOUT', error=str(error))
    except DeviceError as error:
        summary.update(status='EXECUTION ERROR', error=str(error))
    except ProtocolError as error:
        summary.update(status='COMMUNICATION ERROR', error=str(error))
    except (OSError, ValueError, ImportError) as error:
        summary.update(status='ERROR', error=str(error))
    except KeyboardInterrupt:
        summary.update(status='INTERRUPTED', error='Stopped by user')
    finally:
        summary['missing_results'] = RESULT_COUNT - summary['received']
        if link:
            summary['device_logs'] = list(link.logs)
            link.close()
        save_json(report / 'summary.json', summary)
        print(f'{summary["status"]}; physical panel: NOT CHECKED. Report: {report}', flush=True)
        if 'error' in summary:
            print(summary['error'], file=sys.stderr)
    return 2


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('port')
    parser.add_argument('--report', type=Path, help='New report directory')
    parser.add_argument('--baud', type=int, default=460800)
    parser.add_argument('--timeout', type=float, default=10)
    parser.add_argument('--reset', action='store_true', help='Reset through CH340 RTS before connecting')
    args = parser.parse_args()
    if args.timeout <= 0:
        parser.error('Timeout must be positive')
    return run(args)


if __name__ == '__main__':
    raise SystemExit(main())

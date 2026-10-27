"""Single-step runs and offline rejudging shared by the SM83 and ARM7TDMI runners.

A system object (run_sm83.Sm83, run_arm7.Arm7) supplies the suite, the wire
codec and the judge. This module owns the report, the connection, saving each
batch before its ACK, the status of a run and the replay of saved batches, so
that both CPUs follow the same rules.
"""

from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import struct
import subprocess
import sys
import time

from protocol import MAX_PAYLOAD, DeviceError, DeviceResetError, Kind, ProtocolError
from transport import Link


ROOT = Path(__file__).resolve().parents[2]


def save_json(path, value):
    temporary = path.with_suffix(path.suffix + '.tmp')
    temporary.write_text(json.dumps(value, indent=2) + '\n', encoding='utf-8')
    # On Windows a virus scanner or an indexer may hold the old file open for
    # a moment, and the replace is then denied.
    for attempt in range(50):
        try:
            temporary.replace(path)
            return
        except PermissionError:
            if attempt == 49:
                raise
            time.sleep(0.1)


def git_state():
    def git(*args):
        return subprocess.check_output(['git', '-C', str(ROOT), *args], text=True).strip()
    return {'commit': git('rev-parse', 'HEAD'), 'dirty': bool(git('status', '--porcelain'))}


def utc_stamp():
    return datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')


def write_rows(rows, results, failures, printed):
    for row in rows:
        encoded = json.dumps(row, separators=(',', ':')) + '\n'
        results.write(encoded)
        if row['status'] in ('FAIL', 'TIMEOUT'):
            failures.write(encoded)
            if printed is not None and printed[0] < 10:
                print(f'{row["status"]} {row["id"]}: {row["first_difference"]}', flush=True)
                printed[0] += 1


def exchange(link, system, suite, group, indices, request_id, files, summary, args, printed):
    kind, payload = system.payload(request_id, group, indices, args)
    link.send(kind, payload)
    summary['run_payload_bytes'] += len(payload)
    raw, results, failures = files
    position = 0
    while True:
        frame = link.receive(args.timeout)
        if frame.kind == Kind.DONE:
            system.done(frame.payload, request_id, group, indices, summary)
            if position != len(indices):
                raise ProtocolError('Device completed a request with missing results')
            return
        if frame.kind != Kind.DATA:
            raise ProtocolError(f'Unexpected response kind {frame.kind}')
        count, rows = system.batch(frame.payload, request_id, suite, group, indices, position, summary, args)
        summary['result_payload_bytes'] += len(frame.payload)
        position += count
        # Preserve exact verdicts/deltas and judgments before ACK.
        raw.write(struct.pack('<II', frame.sequence, len(frame.payload)))
        raw.write(frame.payload)
        write_rows(rows, results, failures, printed)
        raw.flush()
        results.flush()
        failures.flush()
        link.acknowledge(frame)
        summary['last_acknowledged_batch'] = frame.sequence


def run(args, system):
    started = time.perf_counter()
    stamp = utc_stamp()
    report = args.report or ROOT / 'build/test-reports' / stamp
    report.mkdir(parents=True, exist_ok=False)
    summary = {'format_version': 3, 'operation': system.operation, 'started_utc': stamp, 'status': 'PREPARING'}
    summary.update(system.summary(args))
    summary.update(runner=git_state(), command=sys.argv, mode=args.mode, execution_version=system.version,
                   actual_coverage='all' if args.mode == 'diagnostic' else system.coverage,
                   run_payload_bytes=0, result_payload_bytes=0, port=args.port, baud=args.baud,
                   frame_timeout_seconds=args.timeout, selected=0, received=0, passed=0, failed=0,
                   last_acknowledged_batch=0)
    save_json(report / 'summary.json', summary)
    link = None
    execution_started = None
    try:
        requested_ids = list(args.case)
        previous = None
        if args.rerun_failed:
            previous = json.loads((args.rerun_failed / 'summary.json').read_text())
            requested_ids.extend(json.loads(line)['id']
                                 for line in (args.rerun_failed / 'failures.jsonl').read_text().splitlines())
            summary['rerun_of'] = str(args.rerun_failed.resolve())
            if not requested_ids:
                raise ValueError('Previous report contains no failed or timed-out cases')
        suite = system.open_suite(args, summary)
        if previous and previous.get('manifest_sha256') != suite.manifest_sha256:
            raise ValueError('Rerun data differs from the previous report')
        total = system.select(suite, args, requested_ids)
        summary.update(selected=total, suite_cases=suite.case_count,
                       skipped=[{'count': suite.case_count - total, 'reason': 'Not selected'}])
        save_json(report / 'selection.json', suite.groups)
        summary['selection_sha256'] = hashlib.sha256((report / 'selection.json').read_bytes()).hexdigest()
        system.describe(suite, total, args)
        if not total:
            raise ValueError('No cases selected')
        if args.list:
            summary['status'] = 'NOT RUN'
            return 0
        if not args.port:
            raise ValueError('Specify the board serial port unless using --list')
        link = Link.open(args.port, args.baud, args.reset, write_timeout=min(1, args.timeout / 2))
        info = link.connect(args.timeout)
        summary['device'] = info
        # Drain startup LOG frames even when a failed mount prevents execution.
        link.ping(timeout=args.timeout)
        system.check_device(info, suite)
        system.prepare(summary, info, suite, args)
        print(f'{info["board"]}, revision {info["chip_revision"]}, '
              f'{info["cpu_hz"] / 1e6:g} MHz, {info["preset"]}', flush=True)
        summary['status'] = 'RUNNING'
        save_json(report / 'summary.json', summary)
        execution_started = time.perf_counter()
        last_progress = execution_started
        request_id = 0
        printed = [0]
        with (report / 'actual.bin').open('xb') as raw, (report / 'results.jsonl').open('x') as results, \
                (report / 'failures.jsonl').open('x') as failures:
            for group, indices in system.requests(suite.groups):
                request_id += 1
                exchange(link, system, suite, group, indices, request_id, (raw, results, failures),
                         summary, args, printed)
                summary['completed_requests'] = request_id
                save_json(report / 'summary.json', summary)
                now = time.perf_counter()
                if now - last_progress >= 5 or summary['received'] == total:
                    print(f'{summary["received"]:,}/{total:,}: {system.progress(summary)}; '
                          f'{now - execution_started:.1f}s', flush=True)
                    last_progress = now
        failed = summary['failed'] or summary.get('timed_out', 0)
        summary['status'] = 'FAIL' if failed else 'PASS'
        system.conclude(summary)
        return 1 if failed else 0
    except DeviceResetError as error:
        summary.update(status='DEVICE RESET', error=str(error))
        return 2
    except TimeoutError as error:
        summary.update(status='TIMEOUT', error=str(error))
        return 2
    except DeviceError as error:
        summary.update(status='EXECUTION ERROR', error=str(error))
        return 2
    except ProtocolError as error:
        summary.update(status='COMMUNICATION ERROR', error=str(error))
        return 2
    except (OSError, ValueError, ImportError, struct.error) as error:
        summary.update(status='ERROR', error=str(error))
        return 2
    except KeyboardInterrupt:
        summary.update(status='INTERRUPTED', error='Stopped by user')
        return 130
    finally:
        system.close()
        finished = time.perf_counter()
        summary['total_seconds'] = round(finished - started, 3)
        summary['execution_seconds'] = round(finished - execution_started, 3) if execution_started else None
        summary['not_run'] = summary['selected'] - summary['received']
        if link:
            summary['device_logs'] = list(link.logs)
            summary['dropped_logs'] = link.dropped_logs
            summary['truncated_logs'] = link.truncated_logs
            link.close()
        save_json(report / 'summary.json', summary)
        print(f'{summary["status"]}; {system.final(summary)}. Report: {report}', flush=True)
        if 'error' in summary:
            print(summary['error'], file=sys.stderr)


def check_selection(groups, key):
    seen = set()
    for group in groups:
        indices = group['indices']
        if (group[key] in seen or not indices or indices != sorted(set(indices)) or
                indices[0] < 0 or indices[-1] >= group['count']):
            raise ValueError('Invalid saved selection')
        seen.add(group[key])


def rejudge(args, system):
    started = time.perf_counter()
    stamp = utc_stamp()
    target = args.report or ROOT / 'build/test-reports' / (stamp + '-rejudge')
    target.mkdir(parents=True, exist_ok=False)
    summary = {'format_version': 3, 'operation': 'offline_rejudge', 'started_utc': stamp,
               'status': 'PREPARING', 'runner': git_state(), 'command': sys.argv,
               'rejudged_from': str(args.input.resolve()), 'selected': 0, 'received': 0,
               'passed': 0, 'failed': 0, 'unverified': 0, 'criteria': system.rejudge_criteria,
               'execution_seconds': None}
    summary.update(system.rejudge_counters())
    save_json(target / 'summary.json', summary)
    try:
        original = json.loads((args.input / 'summary.json').read_text())
        if original['execution_version'] != system.version:
            raise ValueError('Saved execution protocol is not current; collect results with the current firmware and runner')
        # A rejudged report can be rejudged again; it names the run it came from.
        operation = original.get('rejudged_operation', original.get('operation'))
        if operation != system.operation:
            raise ValueError(f'Saved report is not a {system.operation} run')
        summary['rejudged_operation'] = operation
        system.check_policy(original)
        selection = (args.input / 'selection.json').read_bytes()
        groups = json.loads(selection)
        check_selection(groups, system.group_key)
        planned = list(system.requests(groups))
        total = sum(len(indices) for _, indices in planned)
        if not total or total != original['selected']:
            raise ValueError('Selection count differs from the saved report')
        if hashlib.sha256(selection).hexdigest() != original['selection_sha256']:
            raise ValueError('Saved selection checksum differs')
        source = (getattr(args, system.source_option) or Path(original['source'])).resolve()
        capture = original.get('source_capture') or {
            key: original.get(key) for key in ('status', 'received', 'not_run', 'completed_requests',
                                              'last_acknowledged_batch', 'total_seconds', 'execution_seconds')}
        complete = (capture['status'] in ('PASS', 'FAIL') and capture['not_run'] == 0 and
                    capture['received'] == total and capture['completed_requests'] == len(planned))
        summary.update({key: original[key] for key in system.carried if key in original})
        summary.update(selected=total, source=str(source), mode=original['mode'],
                       source_capture=capture, rejudge_oracle='suite')
        (target / 'selection.json').write_bytes(selection)
        print(f'Rejudging up to {total:,} saved cases; no board connection.', flush=True)
        request_index = position = sequence = 0
        raw_hash = hashlib.sha256()
        with (args.input / 'actual.bin').open('rb') as raw, (target / 'actual.bin').open('xb') as saved, \
                (target / 'results.jsonl').open('x') as results, (target / 'failures.jsonl').open('x') as failures:
            while prefix := raw.read(8):
                if len(prefix) != 8:
                    raise ProtocolError('Truncated saved batch header')
                actual_sequence, size = struct.unpack('<II', prefix)
                if actual_sequence != sequence + 1 or not 12 <= size <= MAX_PAYLOAD:
                    raise ProtocolError('Invalid saved batch sequence or size')
                payload = raw.read(size)
                if len(payload) != size or request_index >= len(planned):
                    raise ProtocolError('Truncated or unexpected saved batch')
                group, indices = planned[request_index]
                count, rows = system.rejudge_batch(payload, request_index + 1, source, group, indices,
                                                   position, summary, original)
                saved.write(prefix + payload)
                raw_hash.update(prefix + payload)
                write_rows(rows, results, failures, None)
                position += count
                sequence += 1
                if position == len(indices):
                    request_index += 1
                    position = 0
        summary.update(actual_sha256=raw_hash.hexdigest(), saved_batches=sequence)
        if original.get('actual_sha256') not in (None, summary['actual_sha256']):
            raise ProtocolError('Saved observations differ from their recorded checksum')
        if complete and (summary['received'] != total or sequence != capture['last_acknowledged_batch']):
            raise ProtocolError('Saved data does not match the completed capture')
        if not complete or summary['received'] != total:
            summary['status'] = 'INCOMPLETE'
        elif summary['unverified']:
            summary['status'] = 'PARTIAL'
        elif summary['failed'] or summary.get('timed_out'):
            summary['status'] = 'FAIL'
        else:
            summary['status'] = 'PASS'
        return {'PASS': 0, 'FAIL': 1}.get(summary['status'], 2)
    except (OSError, ValueError, KeyError, TypeError, struct.error) as error:
        summary.update(status='ERROR', error=str(error))
        return 2
    except KeyboardInterrupt:
        summary.update(status='INTERRUPTED', error='Stopped by user')
        return 130
    finally:
        summary['not_run'] = summary['selected'] - summary['received']
        summary['total_seconds'] = summary['rejudge_seconds'] = round(time.perf_counter() - started, 3)
        save_json(target / 'summary.json', summary)
        print(f'{summary["status"]}: {system.rejudge_final(summary)}. Report: {target}', flush=True)
        if 'error' in summary:
            print(summary['error'], file=sys.stderr)


def rejudge_main(system, description, source_help):
    import argparse
    parser = argparse.ArgumentParser(description=description)
    parser.add_argument('input', type=Path, help='Current-protocol report directory')
    parser.add_argument('--' + system.source_option.replace('_', '-'), type=Path, help=source_help)
    parser.add_argument('--report', type=Path, help='New output directory; the input report is never overwritten')
    args = parser.parse_args()
    try:
        return rejudge(args, system)
    except OSError as error:
        print(f'Cannot create rejudge report: {error}', file=sys.stderr)
        return 2

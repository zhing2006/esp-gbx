#!/usr/bin/env python3
"""Run the ARM7TDMI single-step suite on the board: the interpreter, judged by the suite, and the JIT,
compared with the interpreter."""

import argparse
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import arm7_execution as codec
from arm7_suite import JUDGED, ROOT, Suite, load_cases
import baseline
from protocol import ProtocolError
from single_step import run


CRITERIA = ('SingleStepTests/ARM7TDMI README.md; R15 without its undefined low bits (TRM 2.6.1); '
            'bus records by kind, size, address, data and N/S, not Lock or cycle; unpredictable R15 writes '
            'and CP14 MRC/MCR run but are not compared, see docs/plans/gba/task-1-3.md; the JIT against the '
            'interpreter in every state word and every bus record field, Lock and cycle included')


def requests(groups):
    for group in groups:
        for indices in codec.requests(group['indices']):
            yield group, indices


class Arm7:
    """The ARM7TDMI single-step suite on both backends, for single_step."""

    operation = 'arm7_single_step'
    version = codec.VERSION
    coverage = 'failed'
    group_key = 'path'
    source_option = 'arm7_source'
    rejudge_criteria = 'Recheck interpreter actuals against the suite, then compare JIT actuals'
    carried = ('model', 'device', 'revision', 'upstream', 'manifest_sha256', 'jit',
               'selection_sha256', 'execution_version', 'actual_coverage')

    def __init__(self):
        self.loaded = None
        self.verifier = None
        self.checks = {}

    def summary(self, args):
        return {'oracle': 'suite', 'interpreter_passed': 0, 'pair_mismatches': 0, 'model': 'ARM7TDMI flat 32-bit',
                'not_compared': {'r15_write': 0, 'cp14': 0}, 'cycle_differences': 0,
                'device_time_us': dict.fromkeys(codec.TIMES, 0), 'files': {},
                'jit': {'status': 'NOT RUN', 'native': 0, 'fallback': 0, 'reasons': {}}, 'criteria': CRITERIA}

    def open_suite(self, args, summary):
        suite = Suite(args.arm7_source, args.data)
        summary.update(source=str(suite.source), data=str(suite.data), revision=suite.revision,
                       upstream=suite.upstream, manifest_sha256=suite.manifest_sha256)
        return suite

    def select(self, suite, args, ids):
        total = suite.select(args.file, ids, args.all)
        if args.list:
            print('Verifying the selected cases against cases.bin...', flush=True)
            for group in suite.groups:
                suite.verify(group)
        return total

    def describe(self, suite, total, args):
        print(f'Selected {total:,} cases in {len(suite.groups)} files; '
              f'skipped {suite.case_count - total:,}: not selected.', flush=True)
        print('JIT: one-instruction blocks, compared with the interpreter; interpreter oracle: suite.', flush=True)
        print(f'Result mode: {args.mode}.', flush=True)

    def check_device(self, info, suite):
        if info['execution_version'] != codec.VERSION:
            raise ProtocolError('Device execution protocol version differs from runner')
        if not info['board_ready'] or not info['sd_mounted'] or not info['gba_data_ready']:
            raise ValueError('Device board initialization, SD mount or ARM7TDMI pack failed')
        if info['gba_manifest_sha256'] != suite.manifest_sha256:
            raise ValueError('Device manifest differs from local data; no cases were sent')
        if info['backends'] & 12 != 12:
            raise ValueError('Device needs both ARM7TDMI interpreter and JIT backends')

    def prepare(self, summary, info, suite, args):
        summary['reference_key'] = baseline.reference_key(
            info, 'arm7_reference_sha256', suite.manifest_sha256, baseline.ARM7_FILES)
        # Each file's cases are checked against cases.bin before its first
        # request, one thread ahead of the board, so the check overlaps the run.
        self.verifier = ThreadPoolExecutor(max_workers=1)
        self.checks = {group['path']: self.verifier.submit(suite.verify, group) for group in suite.groups}

    requests = staticmethod(requests)

    def payload(self, request_id, group, indices, args):
        self.checks[group['path']].result()
        return codec.request(request_id, group['file'], indices,
                             codec.DIAGNOSTIC if args.mode == 'diagnostic' else codec.DEVICE)

    def cases(self, source, group):
        if self.loaded is None or self.loaded[0] is not group:
            self.loaded = (group, load_cases(source, group))
        return self.loaded[1]

    def account(self, summary, group, row, count=1):
        stats = summary['files'].setdefault(group['name'], {
            'received': 0, 'passed': 0, 'failed': 0, 'r15_write': 0, 'cp14': 0, 'cycle_differences': 0,
            'jit_native': 0, 'jit_fallback': 0, 'fallback_reasons': {}, 'pair_mismatches': 0})
        summary['received'] += count
        stats['received'] += count
        status = 'passed' if row is None or row['status'] == 'PASS' else 'failed'
        summary[status] += count
        stats[status] += count
        # A run of passes passed on both backends, natively.
        summary['interpreter_passed'] += count if row is None or row['interpreter_status'] == 'PASS' else 0
        mismatch = row is not None and row['pair_status'] == 'FAIL'
        summary['pair_mismatches'] += mismatch
        stats['pair_mismatches'] += mismatch
        execution = 'native' if row is None else row['jit_execution']
        summary['jit'][execution] += count
        stats[f'jit_{execution}'] += count
        if execution == 'fallback':
            reason = row['fallback_reason']
            for reasons in (summary['jit']['reasons'], stats['fallback_reasons']):
                reasons[reason] = reasons.get(reason, 0) + 1
        if row is not None and row['class'] != 'judged':
            summary['not_compared'][row['class']] += 1
            stats[row['class']] += 1

    def items(self, payload, request_id, mode, source, group, indices, position):
        """Yields (index, item) per case, with item None for a run's passes."""
        cases = self.cases(source, group)
        classes = cases.classes()
        for kind, ordinal, value in codec.decode_batch(payload, request_id, mode, len(indices), position):
            if kind == 'run':
                # Runs are judged passes: the encoding must not make them unjudged.
                if any(classes[index] != JUDGED for index in indices[ordinal:ordinal + value]):
                    raise ProtocolError('Device judged a case that is not compared')
                yield indices[ordinal:ordinal + value], None
            else:
                yield indices[ordinal], value

    def batch(self, payload, request_id, suite, group, indices, position, summary, args):
        mode = codec.DIAGNOSTIC if args.mode == 'diagnostic' else codec.DEVICE
        cases = self.cases(suite.source, group)
        rows, count = [], 0
        for index, item in self.items(payload, request_id, mode, suite.source, group, indices, position):
            if item is None:
                self.account(summary, group, None, len(index))
                count += len(index)
                continue
            row = codec.judge(cases[index], item, 'device' if mode == codec.DEVICE else 'host')
            row['id'] = f'{group["path"]}::{index}'
            rows.append(row)
            self.account(summary, group, row)
            count += 1
        return count, rows

    def done(self, payload, request_id, group, indices, summary):
        cycles, times = codec.check_done(payload, request_id, len(indices))
        summary['cycle_differences'] += cycles
        summary['files'][group['name']]['cycle_differences'] += cycles
        totals = summary['device_time_us']
        for name, value in times.items():
            totals[name] += value
        # Of the rest, what the four steps do not take is decoding, judging,
        # comparing and encoding.
        total = totals['total_us'] or 1
        steps = {'interpreter': totals['interpreter_us'], 'jit_compile': totals['compile_us'],
                 'jit_publish': totals['publish_us'], 'jit_execute': totals['jit_us']}
        rest = total - totals['wait_us'] - totals['send_us']
        parts = {'sd_read': totals['read_us'], 'waiting_for_sd': totals['wait_us'],
                 'sending': totals['send_us'], 'executing': rest, **steps, 'other': rest - sum(steps.values())}
        summary['device_time'] = {'total_seconds': round(totals['total_us'] / 1e6, 3),
                                  **{f'{name}_seconds': round(value / 1e6, 3) for name, value in parts.items()},
                                  **{f'{name}_share': round(value / total, 4) for name, value in parts.items()}}

    def progress(self, summary):
        return f'{summary["passed"]:,} passed, {summary["failed"]:,} failed'

    def conclude(self, summary):
        summary['jit']['status'] = summary['status']

    def close(self):
        if self.verifier:
            self.verifier.shutdown(cancel_futures=True)

    def final(self, summary):
        jit = summary['jit']
        reasons = ', '.join(f'{name} {count:,}' for name, count in sorted(jit['reasons'].items()))
        text = (f'JIT: {jit["native"]:,} native, {jit["fallback"]:,} fallback{f" ({reasons})" if reasons else ""}, '
                f'{summary["pair_mismatches"]:,} differ from the interpreter; interpreter: '
                f'{summary["interpreter_passed"]:,} passed; not compared with the suite: '
                f'{summary["not_compared"]["r15_write"]:,} R15 writes, {summary["not_compared"]["cp14"]:,} CP14; '
                f'{summary["cycle_differences"]:,} cycle differences')
        if 'device_time' in summary:
            time = summary['device_time']
            text += (f'; device {time["total_seconds"]:.1f}s: interpreter {time["interpreter_seconds"]:.1f}s, '
                     f'JIT compile {time["jit_compile_seconds"]:.1f}s, publish {time["jit_publish_seconds"]:.1f}s, '
                     f'execute {time["jit_execute_seconds"]:.1f}s, other {time["other_seconds"]:.1f}s, '
                     f'waiting for SD {time["waiting_for_sd_seconds"]:.1f}s '
                     f'(SD reads {time["sd_read_seconds"]:.1f}s), sending {time["sending_seconds"]:.1f}s')
        return text

    # The run's JIT counts replace these and are not counted again: whether
    # the JIT ran a case natively is an observation, and a device-mode run of
    # passes has no item to count it from.
    def rejudge_counters(self):
        return {'not_compared': {'r15_write': 0, 'cp14': 0}, 'pair_mismatches': 0,
                'jit': {'status': 'NOT RUN', 'native': 0, 'fallback': 0, 'reasons': {}}}

    def check_policy(self, original):
        if original['mode'] not in ('device', 'diagnostic'):
            raise ValueError('Invalid saved result mode')

    def rejudge_batch(self, payload, request_id, source, group, indices, position, summary, original):
        mode = codec.DEVICE if original['mode'] == 'device' else codec.DIAGNOSTIC
        cases = self.cases(source, group)
        rows, count = [], 0
        for index, item in self.items(payload, request_id, mode, source, group, indices, position):
            if item is None:
                summary['unverified'] += len(index)
                summary['received'] += len(index)
                count += len(index)
                continue
            # Re-evaluate observations, not prior verdicts. Items without
            # actuals are device-mode passes that are not compared with the
            # suite; their class follows from the case alone.
            row = codec.judge(cases[index], {**item, 'outcome': codec.UNJUDGED, 'pair_failed': False}, 'host')
            row['id'] = f'{group["path"]}::{index}'
            rows.append(row)
            summary['passed' if row['status'] == 'PASS' else 'failed'] += 1
            summary['pair_mismatches'] += row['pair_status'] == 'FAIL'
            if row['class'] != 'judged':
                summary['not_compared'][row['class']] += 1
            summary['received'] += 1
            count += 1
        return count, rows

    def rejudge_final(self, summary):
        return (f'{summary["passed"]:,} passed, {summary["failed"]:,} failed, '
                f'{summary["unverified"]:,} unverified; JIT {summary["jit"]["fallback"]:,} fallback, '
                f'{summary["pair_mismatches"]:,} differ from the interpreter')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('port', nargs='?', help='Board serial port')
    parser.add_argument('--data', type=Path, default=ROOT / 'tests/sdcard/gba',
                        help='Prepared tests/gba directory with manifest.tsv (default: tests/sdcard/gba)')
    parser.add_argument('--arm7-source', type=Path, default=ROOT / 'refs/tests/arm7tdmi')
    parser.add_argument('--file', action='append', default=[], help='Select a whole file, e.g. arm_bx')
    parser.add_argument('--case', action='append', default=[], help='Exact ID, e.g. "v1/arm_bx.json.bin::17"')
    parser.add_argument('--all', action='store_true', help='Run the complete suite')
    parser.add_argument('--rerun-failed', type=Path, help='Previous report directory')
    parser.add_argument('--list', action='store_true', help='Check the data and save the selection without connecting')
    parser.add_argument('--report', type=Path, help='New report directory (default: build/test-reports/<UTC time>)')
    parser.add_argument('--baud', type=int, default=460800)
    parser.add_argument('--timeout', type=float, default=10, help='Handshake and inter-frame deadline in seconds')
    parser.add_argument('--mode', choices=('device', 'diagnostic'), default='device',
                        help='Device judgments with failure details (default), or lossless actuals judged on host')
    parser.add_argument('--reset', action='store_true', help='Reset the board through CH340 RTS before connecting')
    args = parser.parse_args()
    if not (args.all or args.file or args.case or args.rerun_failed):
        parser.error('Choose --all, --file, --case or --rerun-failed')
    if args.rerun_failed and (args.all or args.file or args.case):
        parser.error('--rerun-failed cannot be combined with other selections')
    if args.timeout <= 0:
        parser.error('Timeout must be positive')
    return run(args, Arm7())


if __name__ == '__main__':
    raise SystemExit(main())

#!/usr/bin/env python3
"""Compare native SM83 JIT execution with a suite-validated interpreter."""

import argparse
from pathlib import Path

import baseline
import execution
from protocol import Kind, ProtocolError
from single_step import run
from sm83_suite import ROOT, Suite, load_cases


def opcode(value):
    try:
        number = int(value.replace(' ', ''), 16)
    except ValueError as error:
        raise argparse.ArgumentTypeError('Use an opcode such as 00 or "cb 7f"') from error
    if not (0 <= number <= 0xff or 0xcb00 <= number <= 0xcbff):
        raise argparse.ArgumentTypeError('Opcode is outside SM83 instruction space')
    return number


def requests(groups):
    for group in groups:
        for start in range(0, len(group['indices']), execution.MAX_SELECTED):
            yield group, group['indices'][start:start + execution.MAX_SELECTED]


class Sm83:
    """The SM83 single-step suite on both backends, for single_step."""

    operation = 'paired_sm83'
    version = execution.VERSION
    coverage = 'failed_and_timed_out'
    group_key = 'opcode'
    source_option = 'sm83_source'
    rejudge_criteria = 'Recheck interpreter against suite, then compare native JIT actuals'
    carried = ('model', 'device', 'revision', 'upstream', 'manifest_sha256', 'jit',
               'selection_sha256', 'execution_version', 'actual_coverage', 'oracle')

    def __init__(self):
        self.loaded = None

    def summary(self, args):
        return {'oracle': 'reference' if args.baseline else 'suite', 'interpreter_passed': 0,
                'pair_mismatches': 0, 'model': 'SM83 flat 64 KiB', 'sample_limit': args.sample_limit,
                'timed_out': 0, 'jit': {'status': 'NOT RUN', 'native': 0, 'fallback': 0, 'opcodes': {}},
                'criteria': 'SingleStepTests/sm83 README.MD; compare provided EI; ignore IE, IME and null bus pins'}

    def open_suite(self, args, summary):
        suite = Suite(args.sm83_source, args.data)
        summary.update(source=str(suite.source), data=str(suite.data), revision=suite.revision,
                       upstream=suite.upstream, manifest_sha256=suite.manifest_sha256)
        return suite

    def select(self, suite, args, ids):
        print(f'Verifying local SM83 source against prepared data ({suite.revision})...', flush=True)
        return suite.verify(args.opcode, ids, args.all)

    def describe(self, suite, total, args):
        print(f'Selected {total:,} cases in {len(suite.groups)} opcode groups; '
              f'skipped {suite.case_count - total:,}: not selected.', flush=True)
        print(f'JIT: native comparison; interpreter oracle: {"reference" if args.baseline else "suite"}.',
              flush=True)
        print(f'Result mode: {args.mode}.', flush=True)

    def check_device(self, info, suite):
        if info['execution_version'] != execution.VERSION:
            raise ProtocolError('Device execution protocol version differs from runner')
        if not info['board_ready'] or not info['sd_mounted'] or not info['data_ready']:
            raise ValueError('Device board initialization, SD mount or SM83 data verification failed')
        if info['manifest_sha256'] != suite.manifest_sha256:
            raise ValueError('Device manifest differs from local data; no cases were sent')
        if info['backends'] & execution.PAIRED != execution.PAIRED:
            raise ValueError('Device needs both SM83 interpreter and JIT backends')

    def prepare(self, summary, info, suite, args):
        summary['reference_key'] = baseline.reference_key(
            info, 'reference_sha256', suite.manifest_sha256, baseline.SM83_FILES, sample_limit=args.sample_limit)
        if args.baseline:
            summary['baseline'] = baseline.validate(args.baseline, summary['reference_key'], suite.groups,
                                                    self.operation, 'opcode', requests)

    requests = staticmethod(requests)

    def payload(self, request_id, group, indices, args):
        mode = execution.DIAGNOSTIC if args.mode == 'diagnostic' else execution.DEVICE
        oracle = execution.REFERENCE if args.baseline else execution.SUITE
        return Kind.RUN, execution.request(request_id, group['opcode'], indices, args.sample_limit, mode, oracle)

    def cases(self, source, group):
        if self.loaded is None or self.loaded[0] is not group:
            self.loaded = (group, load_cases(source, group))
        return self.loaded[1]

    def batch(self, payload, request_id, suite, group, indices, position, summary, args):
        mode = execution.DIAGNOSTIC if args.mode == 'diagnostic' else execution.DEVICE
        oracle = execution.REFERENCE if args.baseline else execution.SUITE
        cases = self.cases(suite.source, group)
        records = execution.decode_batch(payload, request_id, mode, group['opcode'], indices, position, cases, oracle)
        rows = []
        for offset, actual in enumerate(records):
            if (position + offset >= len(indices) or actual['index'] != indices[position + offset] or
                    actual['opcode'] != group['opcode'] or actual['backend'] != execution.PAIRED):
                raise ProtocolError('Missing, duplicate or out-of-order case result')
            case = cases[actual['index']]
            row = execution.judge_pair(case, actual, oracle)
            row.update(id=f'{group["path"]}::{case["name"]}', backend='paired',
                       judged_by='device' if mode == execution.DEVICE else 'host')
            if row['status'] != 'PASS':
                row.update(initial_pc=case['initial']['pc'], opcode=f'{group["opcode"]:04x}')
            rows.append(row)
            summary[{'PASS': 'passed', 'FAIL': 'failed', 'TIMEOUT': 'timed_out'}[row['status']]] += 1
            summary['received'] += 1
            summary['interpreter_passed'] += row['interpreter_status'] == 'PASS'
            summary['pair_mismatches'] += row['pair_status'] == 'FAIL'
            coverage = summary['jit']['opcodes'].setdefault(f'{group["opcode"]:04x}',
                {'native': 0, 'fallback': 0, 'reasons': {}})
            kind = row['jit_execution']
            summary['jit'][kind] += 1
            coverage[kind] += 1
            if kind == 'fallback':
                reason = row['fallback_reason']
                coverage['reasons'][reason] = coverage['reasons'].get(reason, 0) + 1
        return len(records), rows

    def done(self, payload, request_id, group, indices, summary):
        execution.check_done(payload, request_id, len(indices))

    def progress(self, summary):
        return (f'{summary["passed"]:,} passed, {summary["failed"]:,} failed, '
                f'{summary["timed_out"]:,} timed out')

    def conclude(self, summary):
        summary['jit']['status'] = summary['status']

    def close(self):
        pass

    def final(self, summary):
        return f'JIT: {summary["jit"]["native"]:,} native, {summary["jit"]["fallback"]:,} fallback'

    def rejudge_counters(self):
        return {'timed_out': 0}

    def check_policy(self, original):
        if original['mode'] not in ('device', 'diagnostic') or original['oracle'] not in ('suite', 'reference'):
            raise ValueError('Invalid saved result mode or oracle policy')

    def rejudge_batch(self, payload, request_id, source, group, indices, position, summary, original):
        cases = self.cases(source, group)
        if len(cases) != group['count']:
            raise ValueError('Source case count differs from the saved selection')
        actuals = execution.decode_batch(payload, request_id,
            execution.DEVICE if original['mode'] == 'device' else execution.DIAGNOSTIC,
            group['opcode'], indices, position, cases,
            execution.SUITE if original['oracle'] == 'suite' else execution.REFERENCE)
        rows = []
        for actual in actuals:
            case = cases[actual['index']]
            if 'interpreter' not in actual:
                row = {'status': 'UNVERIFIED', 'judged_by': None,
                       'reason': 'Compact device PASS has no saved actual state'}
            else:
                # Re-evaluate observations, not prior verdicts. A changed
                # judge must not inherit a former device PASS or FAIL.
                observed = dict(actual)
                for field in ('oracle_outcome', 'pair_outcome'):
                    if observed[field] != execution.LIMIT:
                        observed[field] = execution.UNJUDGED
                row = execution.judge_pair(case, observed, execution.SUITE)
                row['judged_by'] = 'host'
            row.update(id=f'{group["path"]}::{case["name"]}', backend='paired')
            rows.append(row)
            summary[{'PASS': 'passed', 'FAIL': 'failed', 'TIMEOUT': 'timed_out',
                     'UNVERIFIED': 'unverified'}[row['status']]] += 1
            summary['received'] += 1
        return len(actuals), rows

    def rejudge_final(self, summary):
        return (f'{summary["passed"]:,} passed, {summary["failed"]:,} failed, '
                f'{summary["timed_out"]:,} timed out, {summary["unverified"]:,} unverified')


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('port', nargs='?', help='Board serial port')
    parser.add_argument('--data', type=Path, default=ROOT / 'tests/sdcard/gb',
                        help='Prepared tests/gb directory with manifest.tsv (default: tests/sdcard/gb)')
    parser.add_argument('--sm83-source', type=Path, default=ROOT / 'refs/tests/sm83')
    parser.add_argument('--opcode', type=opcode, action='append', default=[], help='Select an entire opcode group')
    parser.add_argument('--case', action='append', default=[], help='Exact ID, e.g. "v1/10.json::10 0000"')
    parser.add_argument('--all', action='store_true', help='Run the complete selected source suite')
    parser.add_argument('--rerun-failed', type=Path, help='Previous report directory')
    parser.add_argument('--list', action='store_true', help='Verify and save the selection without connecting')
    parser.add_argument('--report', type=Path, help='New report directory (default: build/test-reports/<UTC time>)')
    parser.add_argument('--baud', type=int, default=460800)
    parser.add_argument('--timeout', type=float, default=10, help='Handshake and inter-frame deadline in seconds')
    parser.add_argument('--sample-limit', type=int, default=execution.MAX_SAMPLES)
    parser.add_argument('--baseline', type=Path,
                        help='Compatible completed interpreter suite-validation report; skip repeating its oracle checks')
    parser.add_argument('--mode', choices=('device', 'diagnostic'), default='device',
                        help='Device judgments with failure details (default), or lossless deltas judged on host')
    parser.add_argument('--reset', action='store_true', help='Reset the board through CH340 RTS before connecting')
    args = parser.parse_args()
    if not (args.all or args.opcode or args.case or args.rerun_failed):
        parser.error('Choose --all, --opcode, --case or --rerun-failed')
    if args.rerun_failed and (args.all or args.opcode or args.case):
        parser.error('--rerun-failed cannot be combined with other selections')
    if args.timeout <= 0 or not 0 < args.sample_limit <= execution.MAX_SAMPLES:
        parser.error('Timeout must be positive and sample limit must be in 1..64')
    return run(args, Sm83())


if __name__ == '__main__':
    raise SystemExit(main())

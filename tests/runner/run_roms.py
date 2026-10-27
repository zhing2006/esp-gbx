#!/usr/bin/env python3
"""Run supported DMG-C and CPU CGB E ROMs on both board backends, with
independent judging.

Unsupported index entries are reported without sending an execution request.
No interpreter baseline or emulator-generated reference is used. With
--states, each ROM is also saved, reset and loaded at its save points in
every backend combination, and the resumed runs must pass the suite's
criterion and end as the uninterrupted runs of the same request do.
"""
import argparse
import hashlib
from datetime import datetime, timezone
import json
from pathlib import Path
import struct
import sys
import time

from protocol import DeviceError, DeviceResetError, Kind, ProtocolError
from rom_suite import (COMBINATIONS, CONDITIONS, INDEX_VERSION, MACHINES, MODELS, REGRESSION, TICKS_PER_SECOND,
                       index, unsupported, verify_data)
from single_step import git_state, save_json
from sm83_suite import ROOT
from transport import Link
from frame_compare import compare_digits, compare_frame, save_frame

VERSION = 10
CPU_NAMES = ('pc', 'sp', 'a', 'f', 'b', 'c', 'd', 'e', 'h', 'l', 'ime', 'ei', 'mode', 'halt_bug')
REASONS = {1: 'BREAKPOINT', 2: 'TIME_LIMIT', 3: 'SUSPENDED', 4: 'UNAVAILABLE'}
MEMORIES = ('wram', 'vram', 'oam', 'hram', 'io_ie', 'cartridge_ram')
ACTIONS = {'buttons': 0, 'rtc-round-trip': 1}
BACKENDS = ('interpreter', 'jit')
STATE_STATUS = ('RESUMED', 'ENDED_BEFORE_SAVE', 'CONDITIONS_NOT_MET', 'CAPTURE_REFUSED', 'ENCODING_FAILED',
                'DECODING_REFUSED', 'RESTORE_REFUSED', 'UNAVAILABLE')
DECODE_STATUS = ('ok', 'unknown', 'version', 'system', 'damaged', 'incompatible')
# A resumed run ends as the uninterrupted run on its load backend does.
STATE_FIELDS = ('end', 'ticks', 'instructions', 'cpu', 'memory', 'pcm', 'rtc', 'serial_hex', 'serial_truncated',
                'frame_id', 'frame_sha256')


class Reader:
    def __init__(self, payload):
        self.data, self.at = payload, 0

    def unpack(self, fmt):
        fmt = '<' + fmt
        size = struct.calcsize(fmt)
        if self.at + size > len(self.data):
            raise ProtocolError('Truncated ROM result')
        values = struct.unpack_from(fmt, self.data, self.at)
        self.at += size
        return values

    def cpu(self):
        return dict(zip(CPU_NAMES, self.unpack('HH12B')))

    def digest(self):
        return bytes(self.unpack('32B')).hex()

    def end(self):
        if self.at != len(self.data):
            raise ProtocolError('Trailing ROM result data')


def result_body(r, backend):
    """A RESULT from its reason on, as a RESULT or STATE RESULT carries it."""
    reason, core, flags, ticks, wall, instructions = r.unpack('BBHQQQ')
    if backend not in (0, 1) or reason not in REASONS or flags & ~7:
        raise ProtocolError('Invalid ROM result header')
    row = dict(backend=BACKENDS[backend], end=REASONS[reason],
               core=core, ticks=ticks, wall_us=wall, instructions=instructions,
               cpu=r.cpu(), serial_truncated=bool(flags & 1), render_core=core ^ 1)
    row['jit'] = dict(zip(('compiled', 'invalidated', 'evicted', 'code_bytes'), r.unpack('4I')))
    row['jit']['fallbacks'] = dict(zip(('none', 'unavailable', 'encoding', 'capacity', 'publication'), r.unpack('5Q')))
    row['memory'] = dict(zip(MEMORIES, (r.digest() for _ in MEMORIES)))
    frames, dropped = r.unpack('QQ')
    row['pcm'] = dict(frames=frames, dropped=dropped, digest=bytes(r.unpack('16B')).hex())
    clock = r.unpack('BBBHBB5BI')
    if flags & 4:
        row['rtc'] = dict(seconds=clock[0], minutes=clock[1], hours=clock[2], days=clock[3],
                          halt=bool(clock[4]), carry=bool(clock[5]), latched=list(clock[6:11]),
                          subsecond_ticks=clock[11])
    elif any(clock):
        raise ProtocolError('RTC state without an RTC')
    if flags & 2:
        row['frame_id'], = r.unpack('Q')
        row['frame_sha256'] = r.digest()
    length, = r.unpack('H')
    if length > 1024:
        raise ProtocolError('Serial output exceeds protocol limit')
    row['serial_hex'] = bytes(r.unpack(f'{length}B')).hex()
    row['simulated_seconds'] = ticks / TICKS_PER_SECOND
    row['speed'] = row['simulated_seconds'] / (wall / 1_000_000) if wall else None
    return row


def decode(payload, request_id):
    r = Reader(payload)
    version, kind, backend, actual_id = r.unpack('HBBI')
    if version != VERSION or actual_id != request_id:
        raise ProtocolError('ROM protocol version or request mismatch')
    if kind == 0:
        row = result_body(r, backend)
        r.end()
        return 'result', row
    if kind == 4:
        point, save, status, met, decoded, met_high, zero16 = r.unpack('BBBBBBH')
        met |= met_high << 8
        if (backend not in (0, 1) or save not in (0, 1) or status >= len(STATE_STATUS) or
                met >> len(CONDITIONS) or decoded >= len(DECODE_STATUS) or zero16):
            raise ProtocolError('Invalid state result header')
        saved_at, before = r.unpack('QQ')
        encoded, capture, encode, decode_us, restore = r.unpack('5I')
        frame_us, compiled, installed = r.unpack('2I'), r.unpack('2I'), r.unpack('2I')
        row = result_body(r, backend)
        r.end()
        # The wall time counts from the restore, the ticks from power-on.
        resumed = (row['ticks'] - saved_at) / TICKS_PER_SECOND
        row['speed'] = None
        row['state'] = dict(point=point, save=BACKENDS[save], load=BACKENDS[backend], status=STATE_STATUS[status],
                            conditions_met=[name for bit, name in enumerate(CONDITIONS) if met >> bit & 1],
                            decode_status=DECODE_STATUS[decoded], save_ticks=saved_at,
                            instructions_before=before, encoded_bytes=encoded, capture_us=capture,
                            encode_us=encode, decode_us=decode_us, restore_us=restore,
                            resumed_speed=resumed / (row['wall_us'] / 1_000_000) if row['wall_us'] else None,
                            frames_after_restore=[dict(us=u, compiled=c, installed=i)
                                                  for u, c, i in zip(frame_us, compiled, installed)])
        return 'state', row
    if kind in (3, 5):
        point = save = None
        if kind == 5:
            point, save = r.unpack('BB')
            if backend not in (0, 1) or save not in (0, 1):
                raise ProtocolError('Invalid state frame backend')
        elif backend not in (0, 1, 2):
            raise ProtocolError('Invalid frame backend')
        frame_id, total, offset = r.unpack('QII')
        part = r.data[r.at:]
        r.at = len(r.data)
        if not part or offset + len(part) > total or total > 1 + 2 * 160 * 144:
            raise ProtocolError('Invalid frame part')
        value = dict(backend=backend, id=frame_id, total=total, offset=offset, part=part)
        if kind == 5:
            return 'state_frame', dict(value, point=point, save=BACKENDS[save])
        return 'frame', value
    if backend != 2:
        raise ProtocolError('Invalid diagnostic backend')
    if kind == 2:
        ordinal, = r.unpack('Q')
        r.end()
        return 'progress', ordinal
    if kind != 1:
        raise ProtocolError('Unknown ROM data type')
    ordinal, entry, last, count, difference, first, address, av, bv = r.unpack('QHHIBIHBB')
    row = dict(block=ordinal, entry_pc=entry, last_instruction_pc=last,
               instructions=count, kind=difference, first_access=None if first == 0xffffffff else first,
               memory_difference=dict(address=address, interpreter=av, jit=bv),
               interpreter_cpu=r.cpu(), jit_cpu=r.cpu())
    row['interpreter_ticks'], row['jit_ticks'] = r.unpack('QQ')
    counts = r.unpack('HH')
    if max(counts) > 128 or difference not in (1, 2, 3, 4, 5):
        raise ProtocolError('Invalid diagnostic record counts or kind')
    for name, length in zip(('interpreter_trace', 'jit_trace'), counts):
        records = []
        for _ in range(length):
            tick, pc, addr, data, operation, reserved = r.unpack('QHHBBH')
            if operation > 1 or reserved:
                raise ProtocolError('Invalid memory access record')
            records.append(dict(ticks=tick, pc=pc, address=addr, data=data,
                                operation=('read', 'write')[operation]))
        row[name] = records
    row['register_differences'] = {name: [row['interpreter_cpu'][name], row['jit_cpu'][name]]
                                   for name in CPU_NAMES if row['interpreter_cpu'][name] != row['jit_cpu'][name]}
    r.end()
    return 'difference', row


def judge(actual):
    if actual['end'] != 'BREAKPOINT':
        return actual['end']
    if any(actual['jit']['fallbacks'].values()):
        return 'FALLBACK'
    registers = actual['cpu']
    return 'PASS' if [registers[n] for n in ('b', 'c', 'd', 'e', 'h', 'l')] == [3, 5, 8, 13, 21, 34] else 'FAIL'


def compare(actuals):
    a, b = actuals['interpreter'], actuals['jit']
    return {name: [a.get(name), b.get(name)] for name in STATE_FIELDS if a.get(name) != b.get(name)}


class Parts:
    """Collects a frame's parts, which arrive in order, see FRAME in rom_test.h."""

    def __init__(self):
        self.frames = {}

    def add(self, key, value):
        """The whole encoded frame once its last part arrived, else None."""
        frame_id, data = self.frames.get(key, (value['id'], b''))
        if value['id'] != frame_id or value['offset'] != len(data):
            raise ProtocolError('Frame part out of order')
        data += value['part']
        self.frames[key] = (frame_id, data)
        return data if len(data) == value['total'] else None


def verdict(case, value, frame, directory, data):
    """The suite's own criterion for one run; `frame` is its encoded final frame."""
    fallback = any(value['jit']['fallbacks'].values())
    expected_end = 'BREAKPOINT' if case['end'] == 'ld-b-b' else 'TIME_LIMIT'
    if case['criterion'] == 'fibonacci':
        result = judge(value)
    elif value['end'] != expected_end:
        result = value['end']
    elif frame is None:
        raise ProtocolError('Successful execution has no final frame')
    elif case['criterion'] == 'gambatte-digits':
        comparison = compare_digits(frame, case['digits'], directory, case['frame_rows'])
        value['frame_comparison'] = comparison
        result = 'FALLBACK' if fallback else 'PASS' if comparison['different_pixels'] == 0 else 'FAIL'
    elif case['reference'] is None:
        # Debug-only ROMs without a suite reference: a person reads the frame.
        value['frame_image'] = save_frame(frame, directory)
        result = 'FALLBACK' if fallback else 'MANUAL'
    else:
        comparison = compare_frame(frame, data / case['reference'], directory, case['report_only'], case['colors'])
        value['frame_comparison'] = comparison
        result = ('FALLBACK' if fallback else
                  'UNJUDGEABLE_REFERENCE' if comparison['different_pixels'] is None else
                  'PASS' if comparison['different_pixels'] == 0 else 'FAIL')
    # Lost frames make the PCM depend on host timing.
    if value['pcm']['dropped'] and result in ('PASS', 'MANUAL'):
        result = 'PCM_DROPPED'
    return result


def pairs(point):
    """The (save, load) backend names of a save point's combinations."""
    return {tuple(BACKENDS[part == 'jit'] for part in name.split('-')) for name in point['combinations']}


def request(case, mode, request_id, digest, size, points=(), small_pool=False):
    """Protocol v10 request: header, ROM digest, setup, script, save points, then path."""
    path = case['path'].encode()
    criterion = 1 if case['criterion'] == 'fibonacci' else 2 if case['end'] == 'ld-b-b' else 3
    clock = case['rtc'] or dict(seconds=0, minutes=0, hours=0, days=0, halt=False, carry=False, subsecond_ticks=0)
    flags = (case['rtc'] is not None) | small_pool << 1
    setup = struct.pack('<IBBBBHBBI', case['audio_rate'], flags, clock['seconds'],
                        clock['minutes'], clock['hours'], clock['days'], clock['halt'], clock['carry'],
                        clock['subsecond_ticks'])
    script = b''.join(struct.pack('<QBBH', event['ticks'], ACTIONS[event['action']], event.get('value', 0), 0)
                      for event in case['script'])
    def mask(point):
        return sum(1 << CONDITIONS.index(name) for name in point['conditions'])
    saves = b''.join(struct.pack('<QIBBBB', point['ticks'], point['window_ticks'], mask(point) & 0xff,
                                 sum(1 << COMBINATIONS.index(name) for name in point['combinations']),
                                 mask(point) >> 8, 0)
                     for point in points)
    return struct.pack('<HBBIQIHHHBB', VERSION, mode, criterion, request_id, case['limit_ticks'], size,
                       len(path), len(case['script']), len(points), MACHINES[case['mode']], case['frame_rows']) + \
        bytes.fromhex(digest) + setup + script + saves + path


def selection(args):
    rows = index()
    if args.rerun_failed:
        previous = json.loads((args.rerun_failed / 'summary.json').read_text())
        if previous.get('protocol_version') != VERSION or previous.get('index_version') != INDEX_VERSION:
            raise ValueError('Prior report uses a different ROM protocol or index')
        # The state options carry over.
        options = previous.get('state_options', dict(states=False, combinations=list(COMBINATIONS)))
        args.states, args.combination = options['states'], options['combinations']
        ids = {r['id'] for r in previous['results'] if r['status'] not in ('PASS', 'UNSUPPORTED', 'NOT_RUN')}
        return [r for r in rows if r['id'] in ids], previous['manifest_sha256']
    named = args.all or args.regression or args.suite or args.module or args.rom
    if not (named or args.states):
        raise ValueError('Select --all, --regression, --suite, --module, --rom, --states or --rerun-failed')
    known = {r['id'] for r in rows}
    if set(args.rom) - known:
        raise ValueError('Unknown ROM IDs: ' + ', '.join(sorted(set(args.rom) - known)))
    selected = [r for r in rows if (args.all and r['in_all']) or (args.regression and r['id'] in REGRESSION) or
                r['suite'] in args.suite or set(r['modules']) & set(args.module) or r['id'] in args.rom]
    if args.model:
        selected = [r for r in selected if r['model'] in args.model]
    if args.states:
        # Alone, --states takes every ROM with save points; otherwise it
        # narrows the selection to those.
        selected = [r for r in (selected if named else rows) if r['save_points']]
    return selected, None


def coverage_and_budget(summary):
    required = {r['id'] for r in index() if not r['report_only']}
    selected = {r['id'] for r in summary['results']}
    completed = {r['id'] for r in summary['results'] if 'verdicts' in r}
    passed = {r['id'] for r in summary['results'] if r['status'] == 'PASS'}
    summary['coverage'] = dict(required_total=len(required), required_selected=len(required & selected),
                               required_completed=len(required & completed), required_passed=len(required & passed),
                               report_only_selected=len(selected - required),
                               report_only_completed=len(completed - required),
                               report_only_passed=len(passed - required))
    # Budget the whole invocation, including preparation, SD reads and any
    # report-only ROMs. Partial/diagnostic runs cannot certify a full-run budget.
    # The GBC segment's budgets (docs/roadmap.md): both models' ROMs in 500
    # seconds, the fixed regression in 60, on the GUITION JC1060P470.
    scope, limit = None, None
    if required <= completed:
        scope, limit = 'all-roms', 500
    elif selected == REGRESSION and selected <= completed:
        scope, limit = 'regression', 60
    device = summary.get('device', {})
    eligible = (not summary['diagnostics'] and summary['schedule'] == 'isolated' and
                device.get('board') == 'GUITION JC1060P470' and device.get('chip_revision') == '1.0' and
                device.get('cpu_hz') == 360_000_000 and device.get('preset') == 'jc1060p470-test-release')
    measured = summary['total_wall_seconds'] if scope and eligible else None
    summary['budget'] = dict(scope=scope, limit_seconds=limit, measured_seconds=measured,
                             status='UNMEASURED' if measured is None else 'PASS' if measured <= limit else 'FAIL')


def state_coverage(summary):
    """What the save points covered where the states were actually taken, and
    the board's timings by backend: capture, encoding, decoding and restore
    of each load, and the first two frames after the restore."""
    rows = [value for result in summary['results'] for value in result.get('states', [])]
    if not rows:
        return
    resumed = [value['state'] for value in rows if value['state']['status'] == 'RESUMED']
    short = {'interpreter': 'int', 'jit': 'jit'}
    summary['state_coverage'] = dict(
        loads=len(rows), resumed=len(resumed),
        passed=sum(value['verdict'] == 'PASS' and not value['differences'] for value in rows),
        conditions={name: sum(name in state['conditions_met'] for state in resumed) for name in CONDITIONS},
        combinations={name: sum(f'{short[state["save"]]}-{short[state["load"]]}' == name for state in resumed)
                      for name in COMBINATIONS},
        largest_encoding_bytes=max((state['encoded_bytes'] for state in resumed), default=None))

    def spread(values):
        values = list(values)
        return dict(count=len(values), mean=sum(values) / len(values), max=max(values)) if values else None
    summary['state_timing'] = {backend: dict(
        capture_us=spread(state['capture_us'] for state in resumed if state['save'] == backend),
        restore_us=spread(state['restore_us'] for state in resumed if state['load'] == backend),
        first_frame_us=spread(state['frames_after_restore'][0]['us'] for state in resumed
                              if state['load'] == backend and state['frames_after_restore'][0]['us']),
        second_frame_us=spread(state['frames_after_restore'][1]['us'] for state in resumed
                               if state['load'] == backend and state['frames_after_restore'][1]['us']),
        first_frame_compiled=spread(state['frames_after_restore'][0]['compiled'] for state in resumed
                                    if state['load'] == backend and state['frames_after_restore'][0]['us']))
        for backend in BACKENDS}
    # Encoding and decoding run on the other core, whatever the backends.
    summary['state_timing']['other_core'] = dict(
        encode_us=spread(state['encode_us'] for state in resumed),
        decode_us=spread(state['decode_us'] for state in resumed))


def run(args):
    start = time.perf_counter()
    stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')
    report = args.report or ROOT / 'build/test-reports' / (stamp + '-roms')
    report.mkdir(parents=True, exist_ok=False)
    summary = dict(format_version=1, index_version=INDEX_VERSION, protocol_version=VERSION,
                   status='PREPARING', started_utc=stamp, runner=git_state(), command=sys.argv,
                   models=args.model or list(MODELS), schedule='lockstep' if args.diagnose else args.schedule,
                   diagnostics=args.diagnose, data=str(args.data.resolve()), results=[],
                   jit_pool='16 KiB, 64 blocks' if args.small_pool else 'default',
                   timing='Cold JIT cache; execution includes compilation; excludes loading and result hashing')
    link = None
    save_json(report / 'summary.json', summary)
    try:
        selected, old_manifest = selection(args)
        if not selected:
            raise ValueError('No matching ROMs')
        manifest, verified = verify_data(args.data, selected)
        summary['manifest_sha256'] = manifest
        if old_manifest and old_manifest != manifest:
            raise ValueError('Failed-only rerun requires the same data manifest')
        summary['state_options'] = dict(states=args.states, combinations=args.combination)
        for row in selected:
            points = row['save_points'] if args.states else []
            # The save points this run sends, in the selected combinations.
            row['sent_save_points'] = [dict(point, combinations=chosen) for point in points
                                       if (chosen := [c for c in point['combinations'] if c in args.combination])]
        save_json(report / 'selection.json', selected)
        executable = []
        for row in selected:
            reason = unsupported(row)
            if reason:
                summary['results'].append(dict(id=row['id'], model=row['model'], mode=row['mode'], status='UNSUPPORTED',
                                               reason=reason, requires=row['requires'], modules=row['modules'],
                                               speed=None))
            else:
                executable.append(row)
                summary['results'].append(dict(id=row['id'], model=row['model'], mode=row['mode'], status='NOT_RUN',
                                               actual={}, report_only=row['report_only']))
        results_by_id = {r['id']: r for r in summary['results']}
        print(f'Selected {len(selected)} ROMs: {len(executable)} executable, '
              f'{len(selected) - len(executable)} unsupported (not sent)', flush=True)
        if args.list or not executable:
            for row in selected:
                print(f'{row["id"]}: {unsupported(row) or "READY"}')
                for i, point in enumerate(row['sent_save_points']):
                    print(f'  save point {i}: {point["ticks"]} ticks, {"+".join(point["conditions"]) or "no condition"}, '
                          f'{", ".join(point["combinations"])}')
            summary['status'] = 'LISTED' if args.list else 'NO EXECUTABLE ROMS'
            return 0 if args.list else 2
        if not args.port:
            raise ValueError('Serial port is required for execution')
        if args.diagnose and len(executable) != 1:
            raise ValueError('Select exactly one supported ROM for lockstep diagnosis')
        if any(row['sent_save_points'] for row in executable) and (args.diagnose or args.schedule != 'isolated'):
            raise ValueError('Save points run with --schedule isolated, without --diagnose')
        link = Link.open(args.port, args.baud, args.reset, write_timeout=min(1, args.timeout / 2))
        summary['device'] = link.connect(args.timeout)
        link.ping(timeout=args.timeout)
        if summary['device']['manifest_sha256'] != manifest:
            raise ValueError('Board data manifest does not match local data; no ROM request sent')
        if not summary['device']['board_ready'] or not summary['device']['sd_mounted']:
            raise ValueError('Board or SD card is unavailable')
        summary['status'] = 'RUNNING'
        save_json(report / 'summary.json', summary)
        request_id = 0
        with (report / 'actual.bin').open('xb') as raw, (report / 'results.jsonl').open('x') as output:
            for case in executable:
                case_started = time.perf_counter()
                row = results_by_id[case['id']]
                row['status'] = 'RUNNING'
                actuals, differences, frames = row['actual'], [], {}
                points = case['sent_save_points']
                # Keyed (save point, save backend, load backend).
                expected_states = {(i, save, load) for i, point in enumerate(points) for save, load in pairs(point)}
                states, state_frames = {}, {}
                parts = Parts()
                summary['current_rom'] = case['id']
                save_json(report / 'summary.json', summary)
                # Each save point runs to its end about twice per backend.
                deadline = time.perf_counter() + args.rom_timeout * (1 + 2 * len(points))
                modes = [3] if args.diagnose else [0] if args.schedule == 'concurrent' else [4]
                for mode in modes:
                    request_id += 1
                    digest, size = verified[case['id']]
                    link.send(Kind.ROM_TEST, request(case, mode, request_id, digest, size, points, args.small_pool))
                    received = set()
                    wanted = {'interpreter', 'jit'} if mode in (0, 3, 4) else {('interpreter', 'jit')[mode - 1]}
                    while True:
                        remaining = deadline - time.perf_counter()
                        if remaining <= 0:
                            raise TimeoutError('ROM wall-clock deadline exceeded')
                        frame = link.receive(min(args.timeout, remaining))
                        if frame.kind == Kind.DONE:
                            count = len(wanted) + len(expected_states)
                            if (frame.payload != struct.pack('<HHI', VERSION, count, request_id) or received != wanted or
                                    set(states) != expected_states):
                                raise ProtocolError('Incomplete ROM execution')
                            if case['criterion'] != 'fibonacci' and set(frames) != {n for n in wanted if 'frame_sha256' in actuals[n]}:
                                raise ProtocolError('Missing final frame')
                            break
                        if frame.kind != Kind.DATA:
                            raise ProtocolError('Unexpected ROM response')
                        kind, actual = decode(frame.payload, request_id)
                        raw.write(struct.pack('<II', frame.sequence, len(frame.payload)) + frame.payload)
                        raw.flush()
                        if kind == 'result':
                            name = actual['backend']
                            if name not in wanted or name in received:
                                raise ProtocolError('Duplicate or unexpected backend result')
                            if args.diagnose:
                                actual['instrumented_speed'] = actual['speed']
                                actual['speed'] = None
                            actuals[name] = actual
                            received.add(name)
                            save_json(report / 'summary.json', summary)
                        elif kind == 'frame':
                            encoded = parts.add(('frame', actual['backend']), actual)
                            names = wanted if actual['backend'] == 2 else {('interpreter', 'jit')[actual['backend']]}
                            for name in names if encoded is not None else ():
                                if name not in wanted or name in frames or name not in actuals:
                                    raise ProtocolError('Duplicate or unexpected final frame')
                                value = actuals[name]
                                if (actual['id'] != value.get('frame_id') or
                                        hashlib.sha256(encoded).hexdigest() != value.get('frame_sha256')):
                                    raise ProtocolError('Frame does not match result digest')
                                frames[name] = encoded
                        elif kind == 'state':
                            state = actual['state']
                            key = (state['point'], state['save'], state['load'])
                            if key not in expected_states or key in states or received != wanted:
                                raise ProtocolError('Duplicate or unexpected state result')
                            states[key] = actual
                        elif kind == 'state_frame':
                            key = (actual['point'], actual['save'], BACKENDS[actual['backend']])
                            value = states.get(key)
                            if value is None or key in state_frames:
                                raise ProtocolError('Duplicate or unexpected state frame')
                            encoded = parts.add(('state',) + key, actual)
                            if encoded is not None:
                                if (actual['id'] != value.get('frame_id') or
                                        hashlib.sha256(encoded).hexdigest() != value.get('frame_sha256')):
                                    raise ProtocolError('State frame does not match its result')
                                state_frames[key] = encoded
                        elif kind == 'difference':
                            differences.append(actual)
                            save_json(report / 'diagnosis.json', actual)
                            print(f'DIFFERENCE: block {actual["block"]}, entry 0x{actual["entry_pc"]:04x}, '
                                  f'last instruction 0x{actual["last_instruction_pc"]:04x}, '
                                  f'first access {actual["first_access"]}, registers {actual["register_differences"]}', flush=True)
                        link.acknowledge(frame)
                if link.profile:
                    # Profiling firmware: samples and JIT counters of both jobs.
                    with (report / 'profile.txt').open('a') as profile:
                        profile.write(f'rom {case["id"]}\n' + ''.join(line + '\n' for line in link.profile))
                    link.profile.clear()
                frame_root = report / 'frames' / case['id']
                verdicts = {name: verdict(case, value, frames.get(name), frame_root / name, args.data)
                            for name, value in actuals.items()}
                state_rows = []
                for key in sorted(states):
                    point, save, load = key
                    value, reference = states[key], actuals[load]
                    state = value['state']
                    name = f'point{point}-{save}-{load}'
                    # A resumed frame equal to the uninterrupted one was not sent again.
                    packed = state_frames.get(key)
                    if packed is None and value.get('frame_sha256') == reference.get('frame_sha256') and \
                            value.get('frame_id') == reference.get('frame_id'):
                        packed = frames.get(load)
                    if state['status'] != 'RESUMED':
                        result = state['status']
                    elif not set(points[point]['conditions']) <= set(state['conditions_met']):
                        result = 'CONDITIONS_NOT_MET'
                    else:
                        result = verdict(case, value, packed, frame_root / name, args.data)
                    value.update(name=name, verdict=result, differences={
                        field: [reference.get(field), value.get(field)] for field in STATE_FIELDS
                        if reference.get(field) != value.get(field)} if state['status'] == 'RESUMED' else {})
                    state_rows.append(value)
                    if result != 'PASS' or value['differences']:
                        unlike = ', '.join(value['differences'])
                        print(f'  STATE {name}: {result}' + (f'; unlike the uninterrupted run: {unlike}' if unlike else ''),
                              flush=True)
                mismatch = compare(actuals)
                every = list(verdicts.values()) + [value['verdict'] for value in state_rows]
                state_differences = any(value['differences'] for value in state_rows)
                passed = all(v == 'PASS' for v in every) and not mismatch and not differences and not state_differences
                # A result no reference can judge is neither a pass nor a failure.
                unjudged = next((v for v in ('UNJUDGEABLE_REFERENCE', 'MANUAL') if set(every) == {v}), None)
                row.update(status='PASS' if passed else unjudged if unjudged and not mismatch and not differences
                           and not state_differences else 'FAIL', verdicts=verdicts,
                           pair_differences=mismatch, diagnostic_differences=differences,
                           report_only=case['report_only'], total_wall_seconds=time.perf_counter() - case_started)
                if points:
                    row['states'] = state_rows
                    row['state_passed'] = sum(v['verdict'] == 'PASS' and not v['differences'] for v in state_rows)
                row['serial_difference'] = {name: actual['serial_hex'] for name, actual in actuals.items()} \
                    if actuals['interpreter']['serial_hex'] != actuals['jit']['serial_hex'] else {}
                output.write(json.dumps(row) + '\n'); output.flush()
                save_json(report / 'summary.json', summary)
                speeds = ', '.join(f'{name}: {value["speed"]:.3f}x ({verdicts[name]})'
                                   if value['speed'] is not None else f'{name}: unmeasured'
                                   for name, value in actuals.items())
                saved = f'; states {row["state_passed"]}/{len(state_rows)} passed' if points else ''
                print(f'{row["status"]}: {case["id"]}; {speeds}; equal={not mismatch}{saved}', flush=True)
        summary.pop('current_rom', None)
        required = [r for r in executable if not r['report_only']]
        passed = all(results_by_id[r['id']]['status'] == 'PASS' for r in required)
        observations = any(results_by_id[r['id']]['status'] != 'PASS' for r in executable if r['report_only'])
        summary['supported_status'] = ('PASS' if passed else 'FAIL') if required else 'REPORT_ONLY'
        summary['status'] = ('FAIL' if not passed else 'PARTIAL' if len(executable) != len(selected) else
                             'REPORT_ONLY' if not required else 'PASS_WITH_OBSERVATIONS' if observations else 'PASS')
        return 0 if passed else 1
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
        current = summary.get('current_rom')
        for row in summary['results']:
            if row['id'] == current and row['status'] == 'RUNNING':
                row['status'] = summary['status']
                row['error'] = summary.get('error', 'Execution did not complete')
        summary['counts'] = {status: sum(r['status'] == status for r in summary['results'])
                             for status in sorted({r['status'] for r in summary['results']})}
        summary['total_wall_seconds'] = time.perf_counter() - start
        coverage_and_budget(summary)
        state_coverage(summary)
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
    parser.add_argument('port', nargs='?')
    parser.add_argument('--data', type=Path, default=ROOT / 'tests/sdcard/gb',
                        help='Prepared tests/gb directory with manifest.tsv (default: tests/sdcard/gb)')
    parser.add_argument('--all', action='store_true',
                        help='Run every indexed ROM except debug-only ones and the CGB E Gambatte group')
    parser.add_argument('--regression', action='store_true', help='Run the fixed regression selection')
    parser.add_argument('--suite', action='append', default=[])
    parser.add_argument('--module', action='append', default=[])
    parser.add_argument('--rom', action='append', default=[])
    parser.add_argument('--model', action='append', choices=MODELS,
                        help='Only the ROMs indexed for this model; both by default')
    parser.add_argument('--rerun-failed', type=Path)
    parser.add_argument('--list', action='store_true')
    parser.add_argument('--diagnose', action='store_true')
    parser.add_argument('--states', action='store_true',
                        help='Also save, reset and load at the save points of the selected ROMs, or of all with none selected')
    parser.add_argument('--combination', action='append', choices=COMBINATIONS,
                        help='Only these save and load backends; all four by default')
    parser.add_argument('--schedule', choices=('concurrent', 'isolated'), default='isolated')
    parser.add_argument('--report', type=Path)
    parser.add_argument('--timeout', type=float, default=30, help='Transport inactivity timeout')
    parser.add_argument('--rom-timeout', type=float, default=180, help='Absolute wall-clock limit per ROM')
    parser.add_argument('--baud', type=int, default=460800)
    parser.add_argument('--reset', action='store_true')
    parser.add_argument('--small-pool', action='store_true',
                        help='Give the JIT a 16 KiB code pool of 64 blocks, so that blocks keep moving '
                             'between its two levels')
    args = parser.parse_args()
    args.combination = args.combination or list(COMBINATIONS)
    if args.timeout <= 0 or args.rom_timeout <= 0:
        parser.error('Timeout must be positive')
    return run(args)


if __name__ == '__main__':
    raise SystemExit(main())

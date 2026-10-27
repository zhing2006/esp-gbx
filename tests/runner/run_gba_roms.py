#!/usr/bin/env python3
"""Run GBA test ROMs on both board backends, judged by each suite's own
criterion, see gba_rom_suite.py. A ROM passes when every backend that ran
passes, the backends end in the same state, and the JIT ran no code on the
interpreter for any reason but the regions it does not translate (VRAM,
palette RAM, OAM), which the user accepted. Both backends make PCM at
--rate frames a second; they must make the same frames, and a run that
dropped frames fails. --mode lockstep runs both systems at once and reports
the first turn of the JIT's loop where they differ; --mode concurrent runs
the backends at once, one on each core, where paired runs them one after
the other on core 1.

With --states, each ROM is also saved, reset and loaded at its save points
(gba_rom_suite.py STATES) in every backend combination; the resumed runs
must pass the suite's criterion and end as the uninterrupted run on the
load backend does.

--game PATH measures a game's speed instead: a ROM the user keeps under the
card's roms/, at the same path under the repository, runs --frames frames
on each backend. Nothing judges its picture; the backends must still end in
the same state. Games are never part of the test data.

Unsupported index entries are listed without sending a request.
"""
import argparse
from datetime import datetime, timezone
import hashlib
import json
from pathlib import Path
import struct
import sys
import time

from gba_rom_suite import (COMBINATIONS, CONDITIONS, CYCLES_PER_SECOND, FRAME_CYCLES, INDEX_VERSION, REGRESSION,
                           index, unsupported, verify_data)
from protocol import DeviceError, DeviceResetError, Kind, ProtocolError
from single_step import git_state, save_json
from sm83_suite import ROOT
from transport import Link

VERSION = 2
MODES = dict(paired=0, interpreter=1, jit=2, lockstep=3, concurrent=4)
ENDS = dict(idle=1, frames=2, text=3)
REASONS = {1: 'IDLE_LOOP', 2: 'TIME_LIMIT', 3: 'END', 4: 'UNAVAILABLE', 5: 'LOCKSTEP_DIFF'}
BACKENDS = ('interpreter', 'jit')
MEMORIES = ('ewram', 'iwram', 'vram', 'palette', 'oam', 'io', 'backup')
REGIONS = {2: 'ewram', 3: 'iwram', 4: 'io', 5: 'palette', 6: 'vram', 7: 'oam'}
DIFF_KINDS = {1: 'cpu', 2: 'time', 3: 'memory', 4: 'turn'}
JIT = ('compiled', 'invalidated', 'evicted', 'restored', 'stored_entries', 'store_drops', 'code_bytes')
FALLBACKS = ('none', 'unavailable', 'encoding', 'capacity', 'publication', 'region')
# No-Intro "[BIOS] Nintendo Game Boy Advance Boot ROM (World)", as
# tools/prepare_gba_tests.py BIOS_SHA256.
BIOS_SHA256 = 'fd2547724b505f487e6dcb29ec2ecff3af35a841a77ab2e85fd87350abd36570'
STATE_STATUS = ('RESUMED', 'ENDED_BEFORE_SAVE', 'CONDITIONS_NOT_MET', 'CAPTURE_REFUSED', 'ENCODING_FAILED',
                'DECODING_REFUSED', 'RESTORE_REFUSED', 'UNAVAILABLE')
# gba::Snapshot::Status.
DECODE_STATUS = ('ok', 'unknown', 'system', 'version', 'damaged')
# The state both backends must end in, and a resumed run as the
# uninterrupted run on its load backend.
COMPARED = ('end', 'ticks', 'cpu', 'memory', 'pcm', 'rtc', 'text', 'bitmap', 'frame_id', 'frame_sha256')
WIDTH, HEIGHT = 240, 160


class Reader:
    def __init__(self, payload):
        self.data, self.at = payload, 0

    def unpack(self, fmt):
        fmt = '<' + fmt
        size = struct.calcsize(fmt)
        if self.at + size > len(self.data):
            raise ProtocolError('Truncated GBA ROM result')
        values = struct.unpack_from(fmt, self.data, self.at)
        self.at += size
        return values

    def bytes(self, count):
        return bytes(self.unpack(f'{count}B'))

    def end(self):
        if self.at != len(self.data):
            raise ProtocolError('Trailing GBA ROM result data')


def cpu(r):
    words = r.unpack('37I')
    pipeline = r.unpack('2I')
    sequential, halted, zero = r.unpack('BBH')
    if zero or sequential > 1 or halted > 1:
        raise ProtocolError('Invalid CPU state')
    names = [f'r{i}' for i in range(16)] + [f'r{i}_fiq' for i in range(8, 15)]
    names += [f'r{i}_{bank}' for bank in ('svc', 'abt', 'irq', 'und') for i in (13, 14)]
    names += ['cpsr'] + [f'spsr_{bank}' for bank in ('fiq', 'svc', 'abt', 'irq', 'und')]
    state = {name: f'{value:08x}' for name, value in zip(names, words)}
    state.update(pipeline=[f'{v:08x}' for v in pipeline], sequential=bool(sequential), halted=bool(halted))
    return state


def result_body(r, backend):
    """A RESULT from its reason on, as a RESULT or STATE RESULT carries it."""
    reason, core, flags, ticks, wall = r.unpack('BBHQQ')
    if backend not in (0, 1) or reason not in REASONS or flags & ~3:
        raise ProtocolError('Invalid GBA ROM result header')
    row = dict(backend=BACKENDS[backend], end=REASONS[reason], core=core, ticks=ticks, wall_us=wall, cpu=cpu(r))
    row['jit'] = dict(zip(JIT, r.unpack('7I')))
    row['jit']['fallbacks'] = dict(zip(FALLBACKS, r.unpack('6Q')))
    row['memory'] = {name: r.bytes(32).hex() for name in MEMORIES}
    misses, miss_us, ahead, halt = r.unpack('4Q')
    row['rom_cache'] = dict(misses=misses, miss_wait_us=miss_us, read_ahead=ahead)
    row['halt_cycles'] = halt
    frames, dropped = r.unpack('QQ')
    row['pcm'] = dict(frames=frames, dropped=dropped, digest=r.bytes(16).hex())
    clock = r.bytes(7)
    control, subsecond = r.unpack('BI')
    if flags & 2:
        row['rtc'] = dict(datetime=clock.hex(), control=control, subsecond_cycles=subsecond)
    elif any(clock) or control or subsecond:
        raise ProtocolError('RTC state without an RTC')
    text = r.bytes(600)
    row['text'] = [text[i * 30:(i + 1) * 30].decode('latin-1').rstrip() for i in range(20)]
    row['bitmap'] = r.bytes(1920).hex()
    if flags & 1:
        row['frame_id'], = r.unpack('Q')
        row['frame_sha256'] = r.bytes(32).hex()
    row['simulated_seconds'] = ticks / CYCLES_PER_SECOND
    row['speed'] = row['simulated_seconds'] / (wall / 1_000_000) if wall else None
    return row


def frame_part(r, backend, **extra):
    frame_id, size, offset = r.unpack('QII')
    return dict(backend=backend, frame_id=frame_id, size=size, offset=offset, part=r.data[r.at:], **extra)


def decode(payload, request_id):
    r = Reader(payload)
    version, kind, backend, actual = r.unpack('HBBI')
    if version != VERSION or actual != request_id:
        raise ProtocolError('GBA ROM protocol version or request mismatch')
    if kind == 0:
        row = result_body(r, backend)
        r.end()
        return 'result', row
    if kind == 1:
        turn, quantum, r15, cpsr, before, since, *counts = r.unpack('QQIIIQIIII')
        what, region, zero, offset, a, b, zero2, ticks_a, ticks_b = r.unpack('BBHIBBHQQ')
        if backend != 2 or what not in DIFF_KINDS or zero or zero2:
            raise ProtocolError('Invalid GBA lockstep difference')
        row = dict(turn=turn, quantum=quantum, r15=f'{r15:08x}', cpsr=f'{cpsr:08x}', before=before, since=since,
                   kind=DIFF_KINDS[what],
                   instructions=dict(interpreter=counts[0], jit=counts[1]),
                   stops=dict(interpreter=counts[2], jit=counts[3]),
                   ticks=dict(interpreter=ticks_a, jit=ticks_b))
        if what == 3:
            row.update(region=REGIONS.get(region, region), offset=f'{offset:x}', values=dict(interpreter=a, jit=b))
        states = [cpu(r), cpu(r)]
        r.end()
        row['after'] = dict(r15=states[1]['r15'], cpsr=states[1]['cpsr'])
        row['cpu'] = {k: [states[0][k], states[1][k]] for k in states[0] if states[0][k] != states[1][k]}
        return 'diff', row
    if kind == 2:
        progress, = r.unpack('Q')
        r.end()
        return 'progress', progress
    if kind == 3:
        return 'frame', frame_part(r, backend)
    if kind == 4:
        point, save, status, decoded, met, zero = r.unpack('BBBBHH')
        if (backend not in (0, 1) or save not in (0, 1) or status >= len(STATE_STATUS) or
                met >> len(CONDITIONS) or decoded >= len(DECODE_STATUS) or zero):
            raise ProtocolError('Invalid state result header')
        saved_at, = r.unpack('Q')
        encoded, capture, encode, decode_us, restore = r.unpack('5I')
        frame_us, compiled, installed = r.unpack('2I'), r.unpack('2I'), r.unpack('2I')
        row = result_body(r, backend)
        r.end()
        # The wall time counts from the restore, the ticks from power-on.
        resumed = (row['ticks'] - saved_at) / CYCLES_PER_SECOND
        row['speed'] = None
        row['state'] = dict(point=point, save=BACKENDS[save], load=BACKENDS[backend], status=STATE_STATUS[status],
                            conditions_met=[name for bit, name in enumerate(CONDITIONS) if met >> bit & 1],
                            decode_status=DECODE_STATUS[decoded], save_ticks=saved_at, encoded_bytes=encoded,
                            capture_us=capture, encode_us=encode, decode_us=decode_us, restore_us=restore,
                            resumed_speed=resumed / (row['wall_us'] / 1_000_000) if row['wall_us'] else None,
                            frames_after_restore=[dict(us=u, compiled=c, restored=i)
                                                  for u, c, i in zip(frame_us, compiled, installed)])
        return 'state', row
    if kind == 5:
        point, save = r.unpack('BB')
        if backend not in (0, 1) or save not in (0, 1):
            raise ProtocolError('Invalid state frame')
        return 'state_frame', frame_part(r, backend, point=point, save=BACKENDS[save])
    raise ProtocolError(f'Unknown GBA ROM data type {kind}')


def pairs(point):
    """The (save, load) backend names of a save point's combinations."""
    return {tuple(BACKENDS[part == 'jit'] for part in name.split('-')) for name in point['combinations']}


def request(case, mode, request_id, args, points=()):
    path = case['path'].encode()
    markers = '\n'.join(case['markers']).encode()
    flags = (args.tiny_cache | args.small_pool << 1 | (case['criterion'] in ('frame', 'manual')) << 2 |
             case.get('card_root', False) << 3 | args.linked << 4)
    keys = b''.join(struct.pack('<QHH', event['ticks'], event['keys'], 0) for event in case['keys'])
    saves = b''.join(struct.pack('<QIHBB', point['ticks'], point['window_ticks'],
                                 sum(1 << CONDITIONS.index(name) for name in point['conditions']),
                                 sum(1 << COMBINATIONS.index(name) for name in point['combinations']), 0)
                     for point in points)
    return struct.pack('<HBBIQIHHIHBB', VERSION, mode, ENDS[case['end']], request_id, case['limit_ticks'],
                       case['bytes'], len(path), len(case['keys']), case['frames'], len(markers), flags, 0) + \
        bytes.fromhex(case['sha256']) + struct.pack('<IHH', args.rate, len(points), 0) + keys + saves + markers + path


def decode_frame(encoded):
    """The BGR555 pixels of an encoded frame, see FRAME."""
    if encoded[0] == 2:
        return list(struct.unpack_from(f'<{WIDTH * HEIGHT}H', encoded, 1))
    if encoded[0] != 1:
        raise ProtocolError('Unknown frame format')
    count, bits = struct.unpack_from('<HB', encoded, 1)
    colors = struct.unpack_from(f'<{count}H', encoded, 4)
    at, per = 4 + 2 * count, 8 // bits
    pixels = []
    for i in range(WIDTH * HEIGHT):
        byte = encoded[at + i // per]
        pixels.append(colors[(byte >> (i % per * bits)) & ((1 << bits) - 1)])
    return pixels


def rgb(pixels):
    """Each 5-bit component as (c << 3) | (c >> 2), as the reference frames."""
    out = bytearray()
    for p in pixels:
        for c in (p & 31, (p >> 5) & 31, (p >> 10) & 31):
            out.append(c << 3 | c >> 2)
    return bytes(out)


def save_png(pixels, path):
    from PIL import Image
    Image.frombytes('RGB', (WIDTH, HEIGHT), rgb(pixels)).save(path)


def compare_frame(encoded, reference, directory):
    from PIL import Image
    pixels = decode_frame(encoded)
    directory.mkdir(parents=True, exist_ok=True)
    save_png(pixels, directory / 'actual.png')
    if reference is None:
        return dict(status='MANUAL', actual=str(directory / 'actual.png'))
    with Image.open(reference) as image:
        expected = image.convert('RGB').tobytes()
    actual = rgb(pixels)
    differing = sum(actual[i:i + 3] != expected[i:i + 3] for i in range(0, len(actual), 3))
    return dict(status='PASS' if not differing else 'FAIL', differing_pixels=differing,
                actual=str(directory / 'actual.png'))


def alyosha_text(bitmap):
    """'passed', 'failed' or None: where the mode 4 text at y 76 starts."""
    data = bytes.fromhex(bitmap)
    columns = [x for y in range(8) for x in range(40, 200) if data[y * 240 + x]]
    if not columns:
        return None
    return 'passed' if min(columns) < 60 else 'failed'


def verdict(case, row, frame, directory, data):
    """The suite's verdict on one backend's result."""
    if row['end'] == 'UNAVAILABLE':
        return dict(status='ERROR', detail='The board could not run or summarize the ROM')
    criterion = case['criterion']
    if criterion == 'speed':
        return dict(status='MEASURED' if row['end'] == 'END' else 'FAIL', detail=f'Ended with {row["end"]}')
    if criterion in ('frame', 'manual'):
        if row['end'] != 'END' or frame is None:
            return dict(status='FAIL', detail=f'Ended with {row["end"]} without a frame')
        reference = data / case['reference'] if case['reference'] else None
        return compare_frame(frame, reference, directory / row['backend'])
    if criterion == 'nba':
        text = row['text']
        passes = sum(1 for line in text if ': PASS' in line)
        failed = any('FAIL' in line for line in text)
        expected = case['passes']
        ok = (row['end'] in ('IDLE_LOOP', 'END') and not failed and
              (not case['summary'] or any('congratulations!' in line for line in text)) and
              (expected is None or passes >= expected))
        return dict(status='PASS' if ok else 'FAIL', detail=' | '.join(line for line in text if line))
    if row['end'] != 'IDLE_LOOP':
        return dict(status='FAIL', detail=f'Ended with {row["end"]}')
    register = int(row['cpu'][f'r{case["register"]}'], 16)
    if criterion == 'alyosha':
        text = alyosha_text(row['bitmap'])
        if text is not None:
            return dict(status='PASS' if text == 'passed' else 'FAIL', detail=f'text {text}, r12 {register:#x}')
    return dict(status='PASS' if register == 0 else 'FAIL', detail=f'r{case["register"]} {register:#x}')


def game(path, frames):
    """A speed measurement of a game at a card path under roms/."""
    if not path.startswith('roms/') or '..' in path:
        raise ValueError('A game is a path under roms/')
    payload = (ROOT / path).read_bytes()
    return dict(id='game/' + path[len('roms/'):], suite='game', path=path, card_root=True,
                sha256=hashlib.sha256(payload).hexdigest(), bytes=len(payload), requires=[], modules=['game'],
                report_only=False, end='frames', criterion='speed', register=12, frames=frames, markers=[], keys=[],
                limit_ticks=(frames + 10) * FRAME_CYCLES, reference=None)


def selection(args, rows):
    named = args.all or args.suite or args.module or args.rom or args.regression
    if args.game and not (named or args.rerun_failed or args.states):
        return []
    if args.rerun_failed:
        if named:
            raise ValueError('--rerun-failed is a selection of its own')
        previous = json.loads((args.rerun_failed / 'summary.json').read_text())
        if previous.get('protocol_version') != VERSION or previous.get('index_version') != INDEX_VERSION:
            raise ValueError('Prior report uses a different protocol or index')
        # The state options carry over.
        options = previous.get('state_options', dict(states=False, combinations=list(COMBINATIONS)))
        args.states, args.combination = options['states'], options['combinations']
        ids = {r['id'] for r in previous['results'] if r['status'] not in ('PASS', 'UNSUPPORTED', 'NOT_RUN')}
        return [r for r in rows if r['id'] in ids]
    if args.regression and (args.all or args.suite or args.module or args.rom):
        raise ValueError('--regression is a selection of its own')
    if not (named or args.states):
        raise ValueError('Select --all, --regression, --suite, --module, --rom, --states or --rerun-failed')
    known = {r['id'] for r in rows}
    if set(args.rom) - known:
        raise ValueError('Unknown ROM IDs: ' + ', '.join(sorted(set(args.rom) - known)))
    selected = [r for r in rows if not named or args.all or (args.regression and r['id'] in REGRESSION) or
                r['suite'] in args.suite or set(r['modules']) & set(args.module) or r['id'] in args.rom]
    if args.states:
        # Alone, --states takes every ROM with save points; otherwise it
        # narrows the selection to those.
        selected = [r for r in selected if r['save_points']]
    return selected


def assemble(store, key, value):
    """Adds a frame part, see FRAME in gba_rom_test.h."""
    frame = store.setdefault(key, dict(frame_id=value['frame_id'], data=bytearray(value['size'])))
    if value['frame_id'] != frame['frame_id'] or value['offset'] + len(value['part']) > len(frame['data']):
        raise ProtocolError('Frame part does not fit its frame')
    frame['data'][value['offset']:value['offset'] + len(value['part'])] = value['part']


def exchange(link, case, mode, request_id, args, points):
    link.send(Kind.GBA_ROM_TEST, request(case, mode, request_id, args, points))
    # Each save point runs to its end about three times per save backend.
    deadline = time.perf_counter() + args.rom_timeout * (1 + 3 * len(points))
    expected = {(i, save, load) for i, point in enumerate(points) for save, load in pairs(point)}
    results, frames, diff, states, state_frames = [], {}, None, {}, {}
    while True:
        remaining = deadline - time.perf_counter()
        if remaining <= 0:
            raise TimeoutError('ROM wall-clock limit exceeded')
        frame = link.receive(min(args.timeout, remaining))
        if frame.kind == Kind.DONE:
            version, count, done_id = struct.unpack('<HHI', frame.payload)
            if (version != VERSION or done_id != request_id or count != len(results) + len(states) or
                    set(states) != expected):
                raise ProtocolError('DONE does not match the results')
            return results, frames, diff, states, state_frames
        if frame.kind != Kind.DATA:
            raise ProtocolError(f'Unexpected frame {frame.kind}')
        kind, value = decode(frame.payload, request_id)
        if kind == 'result':
            results.append(value)
        elif kind == 'diff':
            diff = value
        elif kind == 'frame':
            assemble(frames, value['backend'], value)
        elif kind == 'state':
            state = value['state']
            key = (state['point'], state['save'], state['load'])
            if key not in expected or key in states:
                raise ProtocolError('Duplicate or unexpected state result')
            states[key] = value
        elif kind == 'state_frame':
            key = (value['point'], value['save'], BACKENDS[value['backend']])
            if key not in states:
                raise ProtocolError('State frame before its result')
            assemble(state_frames, key, value)
        link.acknowledge(frame)


def checked(result, verdict_row):
    """A verdict, failed when the run dropped PCM frames: those make the PCM
    depend on host timing."""
    if result['pcm']['dropped'] and verdict_row['status'] in ('PASS', 'MANUAL', 'MEASURED'):
        return dict(status='PCM_DROPPED', detail=f'{result["pcm"]["dropped"]} PCM frames dropped')
    return verdict_row


def unconfirmed_fallbacks(result):
    """The JIT's fallbacks to the interpreter that no rule allows: all but
    none and region (code in VRAM, palette RAM or OAM)."""
    if result['backend'] != 'jit':
        return {}
    return {k: v for k, v in result['jit']['fallbacks'].items() if v and k not in ('none', 'region')}


def judge(case, results, frames, directory, data, mode, diff, points=(), states=None, state_frames=None):
    row = dict(id=case['id'], report_only=case['report_only'], criterion=case['criterion'], actual={})
    status = 'PASS'
    root = directory / case['id'].replace('#', '-')
    for result in results:
        backend = BACKENDS.index(result['backend'])
        encoded = frames.get(backend, frames.get(2))
        encoded = bytes(encoded['data']) if encoded else None
        result['verdict'] = checked(result, verdict(case, result, encoded, root, data))
        row['actual'][result['backend']] = result
        if result['verdict']['status'] not in ('PASS', 'MANUAL', 'MEASURED'):
            status = 'FAIL'
        if unconfirmed := unconfirmed_fallbacks(result):
            row['fallbacks'] = unconfirmed
            status = 'FALLBACK' if status == 'PASS' else status
    if case['criterion'] in ('manual', 'speed') and status == 'PASS':
        status = 'MANUAL' if case['criterion'] == 'manual' else 'MEASURED'
    if mode in (MODES['paired'], MODES['concurrent']) and len(results) == 2:
        differing = [k for k in COMPARED if results[0].get(k) != results[1].get(k)]
        if differing:
            row['mismatch'] = differing
            status = 'MISMATCH' if status in ('PASS', 'MANUAL', 'MEASURED') else status
    if diff:
        row['diff'] = diff
        status = 'DIVERGED'
    if states:
        short = {'interpreter': 'int', 'jit': 'jit'}
        rows = []
        for key in sorted(states):
            point, save, load = key
            value, reference = states[key], row['actual'][load]
            state = value['state']
            name = f'point{point}-{short[save]}-{short[load]}'
            # A resumed frame equal to the uninterrupted one was not sent again.
            encoded = state_frames.get(key)
            if encoded is None and value.get('frame_sha256') and value.get('frame_id') == reference.get('frame_id') \
                    and value.get('frame_sha256') == reference.get('frame_sha256'):
                encoded = frames.get(BACKENDS.index(load), frames.get(2))
            encoded = bytes(encoded['data']) if encoded else None
            if state['status'] != 'RESUMED':
                result = dict(status=state['status'])
            elif not set(points[point]['conditions']) <= set(state['conditions_met']):
                result = dict(status='CONDITIONS_NOT_MET')
            else:
                result = checked(value, verdict(case, value, encoded, root / name, data))
                # Code the JIT left to the interpreter after the restore
                # would hide what the restore did to the JIT.
                if (unconfirmed := unconfirmed_fallbacks(value)) and result['status'] in ('PASS', 'MANUAL'):
                    result = dict(status='FALLBACK', detail=f'fallbacks {unconfirmed}')
            value.update(name=name, verdict=result, differences={
                field: [reference.get(field), value.get(field)] for field in COMPARED
                if reference.get(field) != value.get(field)} if state['status'] == 'RESUMED' else {})
            rows.append(value)
        row['states'] = rows
        row['state_passed'] = sum(v['verdict']['status'] in ('PASS', 'MANUAL') and not v['differences']
                                  for v in rows)
        if row['state_passed'] != len(rows) and status in ('PASS', 'MANUAL'):
            status = 'STATE_FAIL'
    row['status'] = status
    return row


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
        passed=sum(value['verdict']['status'] in ('PASS', 'MANUAL') and not value['differences'] for value in rows),
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
    stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')
    report = args.report or ROOT / 'build/test-reports' / (stamp + '-gba-roms')
    report.mkdir(parents=True, exist_ok=False)
    summary = dict(format_version=1, index_version=INDEX_VERSION, protocol_version=VERSION, status='PREPARING',
                   started_utc=stamp, runner=git_state(), command=sys.argv, mode=args.mode,
                   tiny_cache=args.tiny_cache, small_pool=args.small_pool, audio_rate=args.rate,
                   data=str(args.data.resolve()), results=[],
                   timing='Cold JIT cache; includes compilation; excludes loading, hashing and PCM draining')
    save_json(report / 'summary.json', summary)
    rows = index(args.data)
    selected = selection(args, rows)
    summary['state_options'] = dict(states=args.states, combinations=args.combination)
    summary['manifest_sha256'] = verify_data(args.data, selected)
    selected += [dict(game(path, args.frames), save_points=[]) for path in args.game]
    for r in selected:
        points = r['save_points'] if args.states else []
        # The save points this run sends, in the selected combinations.
        r['sent_save_points'] = [dict(point, combinations=chosen) for point in points
                                 if (chosen := [c for c in point['combinations'] if c in args.combination])]
    executable = [r for r in selected if not unsupported(r)]
    for r in selected:
        if unsupported(r):
            summary['results'].append(dict(id=r['id'], status='UNSUPPORTED', reason=unsupported(r)))
    print(f'Selected {len(selected)} ROMs: {len(executable)} executable', flush=True)
    if args.list:
        for r in selected:
            print(f'{r["id"]}: {unsupported(r) or "READY"} ({r["criterion"]}, {", ".join(r["modules"])})')
            for i, point in enumerate(r['sent_save_points']):
                print(f'  save point {i}: {point["ticks"]} ticks, {"+".join(point["conditions"]) or "no condition"}, '
                      f'{", ".join(point["combinations"])}')
        return 0
    if any(r['sent_save_points'] for r in executable) and args.mode != 'paired':
        raise ValueError('Save points run with --mode paired')
    link = Link.open(args.port, args.baud, args.reset)
    try:
        summary['device'] = link.connect(args.timeout)
        if summary['device']['gba_manifest_sha256'] != summary['manifest_sha256']:
            raise ValueError('The card holds other GBA test data than --data')
        if summary['device']['bios_sha256'] != BIOS_SHA256:
            raise ValueError('The card holds no valid GBA BIOS')
        summary['status'] = 'RUNNING'
        mode = MODES[args.mode]
        for number, case in enumerate(executable, 1):
            started = time.perf_counter()
            points = case['sent_save_points']
            try:
                results, frames, diff, states, state_frames = exchange(link, case, mode, number, args, points)
                row = judge(case, results, frames, report / 'frames', args.data, mode, diff, points, states,
                            state_frames)
            except (DeviceError, TimeoutError, ProtocolError) as error:
                row = dict(id=case['id'], report_only=case['report_only'], status='ERROR', detail=str(error))
                if not isinstance(error, DeviceError):
                    # What the board printed, such as a panic, then a fresh session.
                    time.sleep(1)
                    try:
                        link.decoder.feed(link.port.read(link.port.in_waiting))
                    except OSError:
                        pass
                    text = bytes(link.decoder.buffer[:6000]).decode('latin-1')
                    row['device_output'] = text
                    (report / 'crash').mkdir(exist_ok=True)
                    (report / 'crash' / (case['id'].replace('/', '_') + '.txt')).write_text(text)
                    link.close()
                    link = Link.open(args.port, args.baud, True)
                    link.connect(args.timeout)
            row['wall_seconds'] = time.perf_counter() - started
            summary['results'].append(row)
            speeds = ' '.join(f'{b}={a["speed"]:.2f}x' for b, a in row.get('actual', {}).items() if a['speed'])
            detail = '' if row['status'] == 'PASS' else ' ' + '; '.join(
                f'{b}: {a["verdict"].get("detail", "")}' for b, a in row.get('actual', {}).items()) + \
                (f' mismatch {row["mismatch"]}' if 'mismatch' in row else '') + \
                (f' fallbacks {row["fallbacks"]}' if 'fallbacks' in row else '') + row.get('detail', '')
            saved = f' states {row["state_passed"]}/{len(row["states"])} passed' if 'states' in row else ''
            print(f'[{number}/{len(executable)}] {row["status"]} {case["id"]} {speeds}{saved}{detail}', flush=True)
            for value in row.get('states', []):
                if value['verdict']['status'] not in ('PASS', 'MANUAL') or value['differences']:
                    unlike = ', '.join(value['differences'])
                    print(f'  STATE {value["name"]}: {value["verdict"]["status"]}' +
                          (f'; unlike the uninterrupted run: {unlike}' if unlike else ''), flush=True)
            if 'diff' in row:
                print('  diff: ' + json.dumps(row['diff']), flush=True)
            if link.profile:
                # Profiling test firmware: samples for tools/profile_report.py.
                with (report / 'profile.txt').open('a') as profile:
                    profile.write(f'rom {case["id"]}\n' + ''.join(line + '\n' for line in link.profile))
                link.profile.clear()
            if link.logs:
                row['device_logs'] = list(link.logs)
                print('  device: ' + ' | '.join(text.strip() for text in link.logs), flush=True)
                link.logs.clear()
            save_json(report / 'summary.json', summary)
    finally:
        link.close()
    counts = {}
    for row in summary['results']:
        key = row['status'] + (' (report only)' if row.get('report_only') and row['status'] != 'PASS' else '')
        counts[key] = counts.get(key, 0) + 1
    summary['counts'] = counts
    state_coverage(summary)
    required = [r for r in summary['results'] if not r.get('report_only') and r['status'] != 'UNSUPPORTED']
    summary['status'] = 'PASS' if all(r['status'] in ('PASS', 'MANUAL', 'MEASURED') for r in required) else 'FAIL'
    save_json(report / 'summary.json', summary)
    print(f'{summary["status"]}: {counts}; report {report}', flush=True)
    return 0 if summary['status'] == 'PASS' else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('port', nargs='?')
    parser.add_argument('--data', type=Path, default=ROOT / 'tests/sdcard/gba')
    parser.add_argument('--all', action='store_true')
    parser.add_argument('--regression', action='store_true', help='The fixed regression selection')
    parser.add_argument('--suite', action='append', default=[])
    parser.add_argument('--module', action='append', default=[])
    parser.add_argument('--rom', action='append', default=[])
    parser.add_argument('--game', action='append', default=[], help='Measure a game at this card path under roms/')
    parser.add_argument('--frames', type=int, default=600, help='Frames each --game runs')
    parser.add_argument('--rerun-failed', type=Path)
    parser.add_argument('--list', action='store_true')
    parser.add_argument('--mode', choices=MODES, default='paired')
    parser.add_argument('--rate', type=int, default=48000, help='PCM frames a second, 0 for none')
    parser.add_argument('--states', action='store_true',
                        help='Also save, reset and load at the save points of the selected ROMs, or of all with none '
                             'selected')
    parser.add_argument('--combination', action='append', choices=COMBINATIONS,
                        help='Only these save and load backends; all four by default')
    parser.add_argument('--tiny-cache', action='store_true', help='Two ROM cache pages, dropped before each run')
    parser.add_argument('--small-pool', action='store_true', help='JIT code pool of 16 KiB and 64 blocks')
    parser.add_argument('--linked', action='store_true',
                        help='Lockstep with the JIT\'s links, compared where native code returns')
    parser.add_argument('--report', type=Path)
    parser.add_argument('--timeout', type=float, default=30, help='Transport inactivity timeout')
    parser.add_argument('--rom-timeout', type=float, default=300, help='Absolute wall-clock limit per ROM')
    parser.add_argument('--baud', type=int, default=460800)
    parser.add_argument('--reset', action='store_true')
    args = parser.parse_args()
    args.combination = args.combination or list(COMBINATIONS)
    if not 0 <= args.rate <= 192000:
        parser.error('--rate is 0 to 192000')
    if not args.list and not args.port:
        parser.error('port is required')
    return run(args)


if __name__ == '__main__':
    sys.exit(main())

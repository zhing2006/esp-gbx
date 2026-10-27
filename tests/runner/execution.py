"""Execution wire codec and SM83 suite criteria; layout: ../protocol/execution.h."""

import struct

from protocol import EXECUTION_VERSION, ProtocolError


VERSION = EXECUTION_VERSION
INTERPRETER = 1
JIT = 2
PAIRED = 3
SUITE = 0
REFERENCE = 1
FALLBACKS = ('none', 'unavailable', 'encoding', 'capacity', 'publication')
MAX_SAMPLES = 64
MAX_SELECTED = 1024
DEVICE = 0
DIAGNOSTIC = 1
PASS, FAIL, LIMIT, UNJUDGED = range(4)
REQUEST = struct.Struct('<HBBIHHHBBB3x')
BATCH = struct.Struct('<HBBIHH')
SAMPLE = struct.Struct('<HBB')
DONE = struct.Struct('<HHII')
REGISTERS = ('pc', 'sp', 'a', 'b', 'c', 'd', 'e', 'f', 'h', 'l', 'ime', 'ei', 'mode')


def request(request_id, opcode, indices, limit=MAX_SAMPLES, mode=DEVICE, oracle=SUITE):
    if not 0 < len(indices) <= MAX_SELECTED or indices != sorted(set(indices)):
        raise ValueError('Selection must contain ordered, unique case indices')
    if not 0 < limit <= MAX_SAMPLES or mode not in (DEVICE, DIAGNOSTIC) or oracle not in (SUITE, REFERENCE):
        raise ValueError('Invalid observation limit or result mode')
    contiguous = indices[-1] - indices[0] + 1 == len(indices)
    selection = [indices[0]] if contiguous else indices
    return REQUEST.pack(VERSION, PAIRED, 1, request_id, opcode, limit, len(indices),
                        mode, int(contiguous), oracle) + struct.pack(f'<{len(selection)}I', *selection)


class Reader:
    def __init__(self, payload):
        self.payload = payload
        self.offset = BATCH.size

    def take(self, size):
        end = self.offset + size
        if end > len(self.payload):
            raise ProtocolError('Truncated result record')
        data = self.payload[self.offset:end]
        self.offset = end
        return data

    def integer(self, size=1):
        return int.from_bytes(self.take(size), 'little')


def decode_actual(reader, case):
    status = reader.integer()
    values = {name: case['initial'].get(name, 0) for name in REGISTERS}
    values['ime'], values['ei'] = int(bool(values['ime'])), int(bool(values['ei']))
    mask = reader.integer(2)
    if status > 1 or mask >> len(REGISTERS):
        raise ProtocolError('Invalid actual-state status or register mask')
    for bit, name in enumerate(REGISTERS):
        if mask & (1 << bit):
            values[name] = reader.integer(2 if bit < 2 else 1)
    if values['mode'] > 3 or values['ime'] > 1 or values['ei'] > 1:
        raise ProtocolError('Invalid CPU state')
    mcycles, count, ram_count = reader.integer(2), reader.integer(), reader.integer(2)
    if count > MAX_SAMPLES or ram_count > 64 + 2 * MAX_SAMPLES:
        raise ProtocolError('Invalid observation counts')
    ram = [(reader.integer(2), reader.integer()) for _ in range(ram_count)]
    if [a for a, _ in ram] != sorted({a for a, _ in ram}):
        raise ProtocolError('RAM addresses must be sorted and unique')
    cycles = [SAMPLE.unpack(reader.take(SAMPLE.size)) for _ in range(count)]
    if any(signals & ~7 for _, _, signals in cycles):
        raise ProtocolError('Invalid bus signals')
    return {'status': status, 'registers': values, 'mcycles': mcycles, 'ram': ram, 'cycles': cycles}


def decode_batch(payload, request_id, mode, opcode, indices, position, cases, oracle=SUITE):
    if len(payload) < BATCH.size:
        raise ProtocolError('Truncated result batch')
    version, actual_mode, actual_oracle, actual_id, first, count = BATCH.unpack_from(payload)
    if (version != VERSION or actual_mode != mode or actual_oracle != oracle or actual_id != request_id or
            not count or first != position or first + count > len(indices)):
        raise ProtocolError('Wrong result version, policy, request or order')
    reader, results = Reader(payload), []
    for index in indices[first:first + count]:
        flags = reader.integer()
        check, pair, fallback, has_actual = flags & 3, (flags >> 2) & 3, (flags >> 4) & 7, flags >> 7
        check_allowed = (LIMIT, UNJUDGED) if mode == DIAGNOSTIC or oracle == REFERENCE else (PASS, FAIL, LIMIT)
        pair_allowed = (LIMIT, UNJUDGED) if mode == DIAGNOSTIC else (PASS, FAIL, LIMIT)
        needed = mode == DIAGNOSTIC or (oracle == SUITE and check != PASS) or pair != PASS or fallback != 0
        if (check not in check_allowed or pair not in pair_allowed or fallback >= len(FALLBACKS) or
                has_actual != int(needed)):
            raise ProtocolError('Invalid paired outcomes or actual-state coverage')
        result = {'opcode': opcode, 'index': index, 'backend': PAIRED,
                  'oracle_outcome': check, 'pair_outcome': pair, 'fallback': fallback}
        if has_actual:
            case = cases[index]
            result['interpreter'] = decode_actual(reader, case)
            result['jit'] = decode_actual(reader, case)
            addresses = {a for a, _ in case['final']['ram']}
            for backend in ('interpreter', 'jit'):
                addresses.update(a for a, _, pins in result[backend]['cycles'] if pins & 2)
            for backend in ('interpreter', 'jit'):
                if [a for a, _ in result[backend]['ram']] != sorted(addresses):
                    raise ProtocolError('Actual RAM does not cover final RAM and all observed writes')
            if ((check == LIMIT) != bool(result['interpreter']['status']) or
                    (pair == LIMIT) != bool(result['interpreter']['status'] or result['jit']['status'])):
                raise ProtocolError('Observation limit outcomes disagree with actual states')
        results.append(result)
    if reader.offset != len(payload):
        raise ProtocolError('Trailing paired result data')
    return results


def pair_difference(case, interpreter, native):
    def diff(field, a, b, cycle=None):
        result = {'field': field, 'interpreter': a, 'jit': b}
        if cycle is not None:
            result['mcycle'] = cycle
        return result
    if interpreter['status'] or native['status']:
        return diff('execution.limit', interpreter['status'], native['status'])
    for i, (a, b) in enumerate(zip(interpreter['cycles'], native['cycles'])):
        expected = case['cycles'][i] if i < len(case['cycles']) else (None, None, None)
        if a[2] != b[2]:
            return diff(f'cycles[{i}].signals', a[2], b[2], i)
        for field, part in (('address', 0), ('data', 1)):
            if ((a[2] & 3) or expected[part] is not None) and a[part] != b[part]:
                return diff(f'cycles[{i}].{field}', a[part], b[part], i)
    if len(interpreter['cycles']) != len(native['cycles']):
        return diff('cycles.length', len(interpreter['cycles']), len(native['cycles']))
    for field in REGISTERS:
        a, b = interpreter['registers'][field], native['registers'][field]
        if a != b:
            return diff(f'final.{field}', a, b)
    for (address, a), (_, b) in zip(interpreter['ram'], native['ram']):
        if a != b:
            return diff(f'final.ram[0x{address:04X}]', a, b)
    if interpreter['mcycles'] != native['mcycles']:
        return diff('mcycles', interpreter['mcycles'], native['mcycles'])
    return None


def judge_pair(case, actual, oracle):
    reference_difference = comparison = None
    timeout = actual['oracle_outcome'] == LIMIT or actual['pair_outcome'] == LIMIT
    if 'interpreter' in actual:
        if oracle == SUITE:
            reference_difference = first_difference(case, actual['interpreter'])
        comparison = pair_difference(case, actual['interpreter'], actual['jit'])
    if actual['oracle_outcome'] == FAIL and reference_difference is None:
        reference_difference = {'field': 'device.oracle_verdict', 'expected': 'PASS', 'actual': 'FAIL'}
    if actual['pair_outcome'] == FAIL and comparison is None:
        comparison = {'field': 'device.pair_verdict', 'interpreter': 'equal', 'jit': 'different'}
    interpreter_status = 'TIMEOUT' if actual['oracle_outcome'] == LIMIT else 'FAIL' if reference_difference else 'PASS'
    pair_status = 'TIMEOUT' if timeout else 'FAIL' if comparison else 'PASS'
    status = 'TIMEOUT' if timeout else 'FAIL' if reference_difference or comparison or actual['fallback'] else 'PASS'
    row = {'status': status, 'interpreter_status': interpreter_status, 'pair_status': pair_status,
           'jit_execution': 'fallback' if actual['fallback'] else 'native',
           'fallback_reason': FALLBACKS[actual['fallback']]}
    if reference_difference:
        row['oracle_difference'] = reference_difference
    if comparison:
        row['pair_difference'] = comparison
    if status != 'PASS':
        row['first_difference'] = reference_difference or comparison or {
            'field': 'jit.execution', 'expected': 'native', 'actual': row['fallback_reason']}
        row['actual'] = actual
    return row


def check_done(payload, request_id, count):
    if len(payload) != DONE.size or DONE.unpack(payload) != (VERSION, 0, request_id, count):
        raise ProtocolError('Invalid completion record')


def first_difference(case, actual):
    """Ignore disconnected pins, IE and IME as specified by README.MD.

    The optional EI pending latch is compared whenever the case provides it.
    """
    def difference(field, expected, observed, cycle=None):
        result = {'field': field, 'expected': expected, 'actual': observed}
        if cycle is not None:
            result['mcycle'] = cycle
        return result

    if actual['status']:
        return difference('execution', 'completed', 'observation limit exceeded')
    for index, (expected, observed) in enumerate(zip(case['cycles'], actual['cycles'])):
        address, data, signals = expected
        actual_address, actual_data, bits = observed
        for field, wanted, got in (
            ('address', address, actual_address), ('data', data, actual_data),
            ('signals', signals, ''.join(c if bits & (1 << i) else '-' for i, c in enumerate('rwm'))),
        ):
            if wanted is not None and wanted != got:
                return difference(f'cycles[{index}].{field}', wanted, got, index)
    if len(case['cycles']) != len(actual['cycles']):
        return difference('cycles.length', len(case['cycles']), len(actual['cycles']),
                          min(len(case['cycles']), len(actual['cycles'])))
    for name in REGISTERS[:10] + ('ei',):
        expected = case['final'].get(name)
        if expected is not None and expected != actual['registers'][name]:
            return difference(f'final.{name}', expected, actual['registers'][name])
    memory = dict(actual['ram'])
    for address, value in case['final']['ram']:
        if address not in memory:
            return difference(f'final.ram[0x{address:04X}]', value, 'missing')
        if value is not None and value != memory[address]:
            return difference(f'final.ram[0x{address:04X}]', value, memory[address])
    return None

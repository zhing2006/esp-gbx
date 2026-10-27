"""ARM7TDMI wire codec and suite criteria; layout: ../protocol/arm7_execution.h."""

import struct

from arm7_suite import CLASSES, CPSR, JUDGED, WORD_NAMES, WORDS, classify
from protocol import EXECUTION_VERSION, Kind, ProtocolError


VERSION = EXECUTION_VERSION
PAIRED = 3
DEVICE = 0
DIAGNOSTIC = 1
PASS, FAIL, UNJUDGED = range(3)
MAX_INDICES = 2000
MAX_RECORDS = 64
# A run of consecutive cases at least this long goes out as a range.
MIN_RANGE = 64
REQUEST = struct.Struct('<HBBIHBBI')
BATCH = struct.Struct('<HBBIII')
DONE = struct.Struct('<HHIIIIIIIIIII')
RECORD = struct.Struct('<BBBBII')
TIMES = ('read_us', 'wait_us', 'send_us', 'total_us', 'interpreter_us', 'compile_us', 'publish_us', 'jit_us')
# gba::Arm7JitFallback; any but none is not a JIT pass.
FALLBACKS = ('none', 'unavailable', 'encoding', 'capacity', 'publication')
RECORD_FIELDS = ('kind', 'size', 'access', 'cycle', 'address', 'data')


def requests(indices):
    """Splits a file's ordered indices into ranges and index lists, in order."""
    result, scattered = [], []
    start = 0
    while start < len(indices):
        end = start + 1
        while end < len(indices) and indices[end] == indices[end - 1] + 1:
            end += 1
        if end - start >= MIN_RANGE:
            result.extend(scattered[i:i + MAX_INDICES] for i in range(0, len(scattered), MAX_INDICES))
            scattered = []
            result.append(indices[start:end])
        else:
            scattered.extend(indices[start:end])
        start = end
    result.extend(scattered[i:i + MAX_INDICES] for i in range(0, len(scattered), MAX_INDICES))
    return result


def request(request_id, file_index, indices, mode):
    contiguous = indices[-1] - indices[0] + 1 == len(indices)
    if not contiguous and len(indices) > MAX_INDICES:
        raise ValueError('Too many scattered indices for one request')
    selection = [indices[0]] if contiguous else indices
    return Kind.ARM7_RUN, REQUEST.pack(VERSION, PAIRED, mode, request_id, file_index, int(contiguous), 0,
                                       len(indices)) + struct.pack(f'<{len(selection)}I', *selection)


def decode_actual(payload, offset):
    mask, = struct.unpack_from('<Q', payload, offset)
    offset += 8
    if mask >> WORDS:
        raise ProtocolError('Invalid actual state mask')
    words = {}
    for bit in range(WORDS):
        if mask & (1 << bit):
            words[bit], = struct.unpack_from('<I', payload, offset)
            offset += 4
    count = payload[offset]
    offset += 1
    if count > MAX_RECORDS:
        raise ProtocolError('Invalid bus record count')
    records = [RECORD.unpack_from(payload, offset + RECORD.size * i) for i in range(count)]
    return {'words': words, 'records': records}, offset + RECORD.size * count


def decode_batch(payload, request_id, mode, count, position):
    """Items in order: ('run', ordinal, n) for n passes, or ('case', ordinal, item)."""
    if len(payload) < BATCH.size:
        raise ProtocolError('Truncated result batch')
    version, actual_mode, zero, actual_id, first, cases = BATCH.unpack_from(payload)
    if (version != VERSION or actual_mode != mode or zero or actual_id != request_id or not cases or
            first != position or first + cases > count):
        raise ProtocolError('Wrong result version, mode, request or order')
    items, offset, ordinal = [], BATCH.size, first
    try:
        while offset < len(payload) and ordinal < first + cases:
            tag = payload[offset]
            offset += 1
            if not tag & 0x80:
                if mode == DIAGNOSTIC:
                    raise ProtocolError('Diagnostic results contain a run of passes')
                items.append(('run', ordinal, (tag & 0x7f) + 1))
                ordinal += (tag & 0x7f) + 1
                continue
            outcome, kind, overflowed, attached = tag & 3, tag >> 2 & 3, tag >> 4 & 1, tag >> 5 & 1
            pair_failed, jit = tag >> 6 & 1, payload[offset]
            offset += 1
            fallback = jit & 7
            allowed = (UNJUDGED,) if mode == DIAGNOSTIC else (PASS, FAIL)
            needed = mode == DIAGNOSTIC or outcome == FAIL or pair_failed or fallback
            if (outcome not in allowed or kind >= len(CLASSES) or attached != bool(needed) or jit & 0xf0 or
                    fallback >= len(FALLBACKS) or (mode == DIAGNOSTIC and pair_failed) or
                    (mode == DEVICE and outcome == PASS and kind == JUDGED and not pair_failed and not fallback)):
                raise ProtocolError('Invalid result item')
            item = {'outcome': outcome, 'class': kind, 'overflowed': bool(overflowed), 'pair_failed': bool(pair_failed),
                    'fallback': fallback, 'jit_overflowed': bool(jit & 8)}
            if attached:
                item['interpreter'], offset = decode_actual(payload, offset)
                item['jit'], offset = decode_actual(payload, offset)
            items.append(('case', ordinal, item))
            ordinal += 1
    except (IndexError, struct.error) as error:
        raise ProtocolError('Truncated result item') from error
    if offset != len(payload) or ordinal != first + cases:
        raise ProtocolError('Result items do not match the batch')
    return items


def check_done(payload, request_id, count):
    if len(payload) != DONE.size:
        raise ProtocolError('Invalid completion record')
    version, zero, actual_id, actual_count, cycles, *times = DONE.unpack(payload)
    if (version, zero, actual_id, actual_count) != (VERSION, 0, request_id, count) or cycles > count:
        raise ProtocolError('Invalid completion record')
    return cycles, dict(zip(TIMES, times))


def final_words(case, actual):
    return [actual['words'].get(i, case['initial'][i]) for i in range(WORDS)]


def pair_difference(case, item):
    """The JIT against the interpreter: every state word and every record field."""
    def difference(field, interpreter, jit):
        return {'field': field, 'interpreter': interpreter, 'jit': jit}

    interpreter, jit = item['interpreter'], item['jit']
    if item['overflowed'] != item['jit_overflowed']:
        return difference('transactions.overflowed', item['overflowed'], item['jit_overflowed'])
    for index, (wanted, got) in enumerate(zip(interpreter['records'], jit['records'])):
        for field, a, b in zip(RECORD_FIELDS, wanted, got):
            if a != b:
                return difference(f'transactions[{index}].{field}', a, b)
    if len(interpreter['records']) != len(jit['records']):
        return difference('transactions.length', len(interpreter['records']), len(jit['records']))
    for index, (a, b) in enumerate(zip(final_words(case, interpreter), final_words(case, jit))):
        if a != b:
            return difference(f'final.{WORD_NAMES[index]}', f'{a:08x}', f'{b:08x}')
    return None


def first_difference(case, actual, overflowed):
    """The suite's criteria, as arm7_execution.h lists them."""
    def difference(field, expected, observed):
        return {'field': field, 'expected': expected, 'actual': observed}

    if overflowed:
        return difference('transactions.length', len(case['transactions']), f'more than {MAX_RECORDS}')
    for index, (wanted, got) in enumerate(zip(case['transactions'], actual['records'])):
        kind, size, address, data, _, access = wanted
        got_kind, got_size, got_access, _, got_address, got_data = got
        for field, expected, observed in (('kind', kind, got_kind), ('size', size, got_size),
                                          ('address', address, got_address), ('data', data, got_data),
                                          ('sequential', access & 1, got_access & 1)):
            if expected != observed:
                return difference(f'transactions[{index}].{field}', expected, observed)
    if len(case['transactions']) != len(actual['records']):
        return difference('transactions.length', len(case['transactions']), len(actual['records']))
    words = final_words(case, actual)
    r15 = ~1 if case['final'][CPSR] & 0x20 else ~3
    for index, (expected, observed) in enumerate(zip(case['final'], words)):
        if (expected ^ observed) & (r15 if index == 15 else ~0):
            return difference(f'final.{WORD_NAMES[index]}', f'{expected:08x}', f'{observed:08x}')
    return None



def actual_row(actual, overflowed):
    return {'words': {WORD_NAMES[i]: f'{value:08x}' for i, value in actual['words'].items()},
            'records': actual['records'], 'overflowed': overflowed}


def judge(case, item, judged_by):
    """A row for one item; host judgments of the actuals replace device verdicts."""
    kind = classify(case['opcode'], case['initial'][CPSR])
    if kind != item['class']:
        raise ProtocolError('Device class differs from the encoding')
    attached = 'interpreter' in item
    oracle = None
    if kind == JUDGED:
        oracle = first_difference(case, item['interpreter'], item['overflowed']) if attached else None
        if item['outcome'] == FAIL and oracle is None:
            oracle = {'field': 'device.verdict', 'expected': 'PASS', 'actual': 'FAIL'}
    pair = pair_difference(case, item) if attached else None
    if item['pair_failed'] and pair is None:
        pair = {'field': 'device.pair_verdict', 'interpreter': 'equal', 'jit': 'different'}
    fallback = FALLBACKS[item['fallback']]
    failed = oracle or pair or item['fallback']
    row = {'status': 'FAIL' if failed else 'PASS', 'class': CLASSES[kind], 'judged_by': judged_by,
           'interpreter_status': 'FAIL' if oracle else 'PASS', 'pair_status': 'FAIL' if pair else 'PASS',
           'jit_execution': 'fallback' if item['fallback'] else 'native', 'fallback_reason': fallback}
    if failed:
        row.update(first_difference=oracle or pair or {'field': 'jit.execution', 'expected': 'native',
                                                       'actual': fallback},
                   opcode=f'{case["opcode"]:08x}', thumb=bool(case['initial'][CPSR] & 0x20))
        if oracle:
            row['oracle_difference'] = oracle
        if pair:
            row['pair_difference'] = pair
        if attached:
            row['actual'] = {'interpreter': actual_row(item['interpreter'], item['overflowed']),
                             'jit': actual_row(item['jit'], item['jit_overflowed'])}
    return row

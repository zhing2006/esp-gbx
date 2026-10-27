#!/usr/bin/env python3
"""Run native RV32 backend checks through the shared board transport.

Expected values follow RV32I/M/C, Zcb, Zba, Zbb, Zbs and the psABI, not an
emulator-generated golden.
Includes small interpreter/native SM83 program checks and ARM and Thumb programs on
the ARM7TDMI JIT against the interpreter, not the SD single-step suites.
Wire layout and version contract: tests/protocol/jit_test.h.
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

VERSION = 22
CONTROL = struct.Struct('<HHI')
RESULT = struct.Struct('<HHII')
CONSTANTS = (0, 1, 0x7ff, 0x800, 0xfffff800, 0xfffff7ff, 0xffffffff,
             0x80000000, 0x7fffffff, 0x12345800, 0xfff, 0x1000)
# The cases in tests/device/rv32_extensions.S, then the JIT encoder cases in
# jit_test.cpp, run with a1 = X, a2 = Y and the word M at a3; 16-bit cases
# start with a0 = X. An instruction that traps reports 0xbad00000 | its
# exception code; 2 is an illegal instruction.
X, Y, M = 0x0400f0a8, 0xffffffda, 0x8001aa80
ILLEGAL = 0xbad00002
MASK = 0xffffffff


def rotate_left(value, count):
    count &= 31
    return (value << count | value >> (32 - count)) & MASK


def sign_extend(value, bits):
    value &= (1 << bits) - 1
    return value - (1 << bits) & MASK if value >> (bits - 1) else value


def signed(value):
    return value - (1 << 32) if value >> 31 else value


def bit_count(value):
    return bin(value).count('1')


EXTENSIONS = [
    ('Zba SH1ADD', 'zba', (X << 1) + Y & MASK), ('Zba SH2ADD', 'zba', (X << 2) + Y & MASK),
    ('Zba SH3ADD', 'zba', (X << 3) + Y & MASK),
    ('Zbb ANDN', 'zbb', X & ~Y & MASK), ('Zbb ORN', 'zbb', (X | ~Y) & MASK),
    ('Zbb XNOR', 'zbb', ~(X ^ Y) & MASK), ('Zbb CLZ', 'zbb', 32 - X.bit_length()),
    ('Zbb CTZ', 'zbb', (X & -X).bit_length() - 1), ('Zbb CPOP', 'zbb', bit_count(X)),
    ('Zbb MAX', 'zbb', max(signed(X), signed(Y)) & MASK), ('Zbb MAXU', 'zbb', max(X, Y)),
    ('Zbb MIN', 'zbb', min(signed(X), signed(Y)) & MASK), ('Zbb MINU', 'zbb', min(X, Y)),
    ('Zbb SEXT.B', 'zbb', sign_extend(X, 8)), ('Zbb SEXT.H', 'zbb', sign_extend(X, 16)),
    ('Zbb ZEXT.H', 'zbb', X & 0xffff), ('Zbb ROL', 'zbb', rotate_left(X, Y)),
    ('Zbb ROR', 'zbb', rotate_left(X, -Y)), ('Zbb RORI', 'zbb', rotate_left(X, -12)),
    ('Zbb ORC.B', 'zbb', sum(0xff << i for i in range(0, 32, 8) if X >> i & 0xff)),
    ('Zbb REV8', 'zbb', int.from_bytes(X.to_bytes(4, 'little'), 'big')),
    ('Zbs BCLR', 'zbs', X & ~(1 << (Y & 31))), ('Zbs BCLRI', 'zbs', X & ~(1 << 15)),
    ('Zbs BEXT', 'zbs', X >> (Y & 31) & 1), ('Zbs BEXTI', 'zbs', X >> 7 & 1),
    ('Zbs BINV', 'zbs', X ^ 1 << (Y & 31)), ('Zbs BINVI', 'zbs', X ^ 1 << 31),
    ('Zbs BSET', 'zbs', X | 1 << (Y & 31)), ('Zbs BSETI', 'zbs', X | 1 << 9),
    ('Zcb C.LBU', 'zcb', M >> 8 & 0xff), ('Zcb C.LHU', 'zcb', M >> 16),
    ('Zcb C.LH', 'zcb', sign_extend(M >> 16, 16)),
    ('Zcb C.SB', 'zcb', M & ~0xff00 | (Y & 0xff) << 8), ('Zcb C.SH', 'zcb', M & 0xffff | (Y & 0xffff) << 16),
    ('Zcb C.ZEXT.B', 'zcb', X & 0xff), ('Zcb C.SEXT.B', 'zcb', sign_extend(X, 8)),
    ('Zcb C.ZEXT.H', 'zcb', X & 0xffff), ('Zcb C.SEXT.H', 'zcb', sign_extend(X, 16)),
    ('Zcb C.NOT', 'zcb', ~X & MASK), ('Zcb C.MUL', 'zcb', X * Y & MASK),
    ('Zba SH3ADD from the JIT encoder', 'zba', (X << 3) + Y & MASK),
    ('Zbb ZEXT.H from the JIT encoder', 'zbb', X & 0xffff),
    ('Zbb SEXT.B from the JIT encoder', 'zbb', sign_extend(X, 8)),
    ('Zbs BEXTI from the JIT encoder', 'zbs', X >> 7 & 1),
    ('Zbb ANDN from the JIT encoder', 'zbb', X & ~Y & MASK),
    ('Zbb ROR from the JIT encoder', 'zbb', rotate_left(X, -Y)),
    ('Zbb RORI from the JIT encoder', 'zbb', rotate_left(X, -12)),
    ('Zbb MAXU from the JIT encoder', 'zbb', max(X, Y)),
    ('Zbb CLZ from the JIT encoder', 'zbb', 32 - X.bit_length()),
    ('Zba SH1ADD from the JIT encoder', 'zba', (X << 1) + Y & MASK),
    ('Zba SH2ADD from the JIT encoder', 'zba', (X << 2) + Y & MASK),
    ('Zbs BCLRI from the JIT encoder', 'zbs', X & ~(1 << 15)),
    ('Zbs BINVI from the JIT encoder', 'zbs', X ^ 1 << 31),
    ('Zbs BSETI from the JIT encoder', 'zbs', X | 1 << 9),
]
# Extensions the firmware of each chip major revision generates, see
# rv32::kZb: their instructions must execute. The others may also raise an
# illegal-instruction exception.
ENABLED = {1: (), 3: ('zba', 'zbb', 'zbs')}
# tests/device/arm7_blocks.cpp, each program: bits 0-3, after the whole run
# the JIT's CPU state, memory, bus records and instruction count equal the
# interpreter's; bit 4, the same after every shorter cycle budget, which
# stops blocks at each instruction boundary; bit 5, no fallback. The bits
# from 6 check the program's results as the ARM7TDMI defines them.
ARM7_PROGRAM = 0x3f
# tests/device/gba_blocks.cpp, each program on gba::System: bits 0-2, at
# the end the JIT's CPU, time and memory equal the interpreter's; bit 3,
# the same when the JIT's run stops every 7 ticks; bit 4, blocks compiled
# and no fallback. The bits from 5 check the program's results.
GBA_PROGRAM = 0x1f
# The checks of the ARM7TDMI's and the GBA's programs. A firmware without the
# GBA (GBX_SYSTEMS_GB_ONLY, components/core/Kconfig) reports no ARM7TDMI JIT
# in HELLO (tests/protocol/execution.h) and leaves them out.
GBA_CHECKS = ('ARM7TDMI JIT against the interpreter: ', 'GBA JIT against the interpreter: ')
ARM7_JIT = 8

CHECKS = [(f'Load constant 0x{value:08x}', value, 'all') for value in CONSTANTS] + [
    ('ADD wraparound', 0x80000008, 'all'), ('SUB wraparound', 0x7ffffffe, 'all'),
    ('SLL register', 0x60, 'all'), ('SLT signed', 1, 'all'), ('SLTU unsigned', 0, 'all'),
    ('XOR register', 0x80000006, 'all'), ('SRL register', 0x04000000, 'all'),
    ('SRA register', 0xfc000000, 'all'), ('OR register', 0x80000007, 'all'), ('AND register', 1, 'all'),
    ('MUL low', 0x8000000f, 'all'), ('MULH signed high', 0xfffffffd, 'all'),
    ('MULHSU mixed high', 0xfffffffd, 'all'), ('MULHU unsigned high', 2, 'all'),
    ('DIV signed', 0xe6666667, 'all'), ('DIVU unsigned', 0x1999999a, 'all'),
    ('REM signed', 0, 'all'), ('REMU unsigned', 1, 'all'),
    ('DIV by zero', 0xffffffff, 'all'), ('REM by zero', 0x80000000, 'all'),
    ('DIV signed overflow', 0x80000000, 'all'), ('REM signed overflow', 0, 'all'),
    ('BEQ outcomes', 4, 'all'), ('BNE outcomes', 3, 'all'), ('BLT outcomes', 1, 'all'),
    ('BGE outcomes', 6, 'all'), ('BLTU outcomes', 2, 'all'), ('BGEU outcomes', 5, 'all'),
    ('Backward loop and patched JAL', 55, 'all'),
    ('Compressed LI, ADDI, LUI and ADD', 0xfffff00c, 'all'),
    ('Compressed MV, SUB, XOR, OR, AND and ANDI', 8, 'all'),
    ('Compressed SRAI, SRLI and SLLI', 0x07ffff00, 'all'),
    ('Compressed stack and register-based word accesses', 28, 'all'),
    ('Compressed BEQZ and BNEZ outcomes', 22, 'all'),
    ('Compressed BNEZ loop, JAL, J, JR and JALR', 29, 'all'),
    ('Relocated JAL and 32-bit instructions at halfword offsets', 0x12345131, 'all'),
] + [(name, (expected, extension), 'extensions') for name, extension, expected in EXTENSIONS] + [
    ('LB sign extension', 0xffffff80, 'all'), ('LBU zero extension', 0x80, 'all'),
    ('LH sign extension', 0xffff8001, 'all'), ('LHU zero extension', 0x8001, 'all'),
    ('SB and SH preserve adjacent bytes', 0x8001aa44, 'all'),
    ('C++ callback exit reason', 0x81, 'all'), ('C++ callback result and context', 68, 'all'),
    ('C++ callback count', 1, 'all'), ('C++ callback stack alignment', 0, 'all'),
    ('Integer callee-saved registers and gp/tp', 1, 'all'),
    ('Published code first execution', 0x12345678, 'all'), ('Rewritten code execution', 0x89abcdef, 'all'),
    ('Code address reused after clear', 1, 'all'), ('Old block key removed', 1, 'all'),
    ('Published PSRAM code first execution', 0x12345678, 'all'),
    ('Rewritten PSRAM code execution', 0x89abcdef, 'all'),
    ('Second-level code across segments and a wrapped lap, oldest dropped first', 15, 'all'),
    ('Full code pool reported', 1, 'all'), ('Code pool reusable after clear', 42, 'all'),
    ('Full block index reported', 1, 'all'), ('Opaque 64-bit key lookup', 1, 'all'),
    ('All keys removed on clear', 1, 'all'),
    ('Interpreter loop, call, return and bus tracing', 0x3ff, 'all'),
    ('JIT loop, call, return, cache reuse and bus tracing', 0x3ff, 'all'),
    ('Interpreter EI sequence', 0xff, 'all'), ('JIT EI sequence across blocks', 0xff, 'all'),
    ('Interpreter illegal lock and wrapping fetches', 0x3fff, 'all'),
    ('JIT illegal lock and wrapping fetches', 0x3fff, 'all'),
    ('Interpreter varying PC, operands and opcodes', 0xff, 'all'),
    ('JIT instruction reuse, dynamic operands and opcode identity', 0xff, 'all'),
] + [(f'ARM7TDMI JIT against the interpreter: {name}', ARM7_PROGRAM | checks << 6, 'all') for name, checks in (
    ('ARM loop, flags, conditions, shifts and multiplies', 0b1111),
    ('ARM and Thumb switches, BL, PUSH, POP, loads and stores', 0b1111),
    ('MSR, MRS, banked registers, SWI, UND, LDM^ and STM^', 0b1111),
    ('Thumb branches, ALU, LDMIA, STMIA, SWI and BX PC', 0b1111),
    ('conditional loads, stores and multiplies, overwritten flag sources, Zba and Zbs patterns', 0b1111),
)] + [
    ('Interpreter event before, at and after an access', 7, 'memory'),
    ('JIT event inside an instruction and boundary exit', 7, 'memory'),
    ('Interpreter WRAM and echo code rewriting', 15, 'memory'),
    ('JIT WRAM and echo code invalidation', 15, 'memory'),
    ('Interpreter HRAM in-block self-modification', 15, 'memory'),
    ('JIT HRAM in-block self-modification', 15, 'memory'),
    ('Interpreter flags across instructions and access timestamps', 15, 'memory'),
    ('JIT lazy flags and unchanged traced block partition', 15, 'memory'),
    ('Interpreter opcode fetch, mapping, deadlines, refetch and suspension', 511, 'memory'),
    ('JIT opcode fetch, mapping, deadlines, refetch and suspension', 511, 'memory'),
    ('Interpreter self-modified opcode, flags, trace and repeated execution', 15, 'memory'),
    ('JIT self-modified opcode, flags, trace and retired-block reuse', 15, 'memory'),
    ('Interpreter self-modified CB byte across a page boundary', 7, 'memory'),
    ('JIT self-modified CB byte across a page boundary', 7, 'memory'),
    ('Interpreter run deadlines, trace, code rewriting and event rescheduling', 15, 'memory'),
    ('JIT batched deadlines, trace, code rewriting and event rescheduling', 15, 'memory'),
    ('Interpreter conditional loops, register state and code rewriting', 15, 'memory'),
    ('JIT linked destinations after eviction, refetch and cache clear', 15, 'memory'),
    ('Cache deletion preserves wrapped collisions, native entries and reinsertion', 1, 'memory'),
    ('Selective code replacement preserves hot blocks and lookup identity', 1, 'all'),
    ('Interpreter interrupt event, IE/IF writes, EI/RETI and service trace', 127, 'memory'),
    ('JIT interrupt event, IE/IF writes, EI/RETI and service trace', 127, 'memory'),
    ('Interpreter HALT wake, bugged fetches, EI-HALT and lock', 63, 'memory'),
    ('JIT HALT wake, bugged fetches, EI-HALT and lock', 63, 'memory'),
    ('Interpreter DMG STOP paths, selected wake and joypad edges', 63, 'memory'),
    ('JIT DMG STOP paths, selected wake and joypad edges', 63, 'memory'),
    ('Interpreter serial shifting, abort and DIV-aligned HALT transfer', 15, 'memory'),
    ('JIT serial shifting, abort and DIV-aligned HALT transfer', 15, 'memory'),
    ('Interpreter DIV/TAC overflow reload writes and window expiry', 255, 'memory'),
    ('JIT DIV/TAC overflow reload writes and window expiry', 255, 'memory'),
    ('Interpreter MBC3 bank code and instructions across ROM windows', 511, 'memory'),
    ('JIT ROM bank links, cache reuse and instructions across ROM windows', 511, 'memory'),
    ('Interpreter MBC3 RAM code, RTC isolation and failed-load preservation', 127, 'memory'),
    ('JIT cartridge RAM code, RTC isolation and failed-load preservation', 127, 'memory'),
    ('Interpreter DMA blocked writes, independent bus access and OAM restrictions', 63, 'memory'),
    ('JIT DMA blocked writes, independent bus access and OAM restrictions', 63, 'memory'),
    ('Interpreter ROM code after a full code pool and a reload', 7, 'memory'),
    ('JIT ROM blocks run from the second level after eviction and dropped on reload', 7, 'memory'),
    ('Interpreter linked ROM ring, resumed blocks and banked code', 31, 'memory'),
    ('JIT blocks moved between levels, dropped copies, links and banks', 31, 'memory'),
] + [(f'GBA JIT against the interpreter: {name}', GBA_PROGRAM | checks << 5, 'memory') for name, checks in (
    ('timer interrupts inside blocks of several instructions', 0b111),
    ('code rewritten in IWRAM and EWRAM, also through their mirrors, by the CPU, in the pipeline and by DMA',
     0b11111),
)]


def run(args):
    # Group order matches execute_checks(): mapped, deletion, peripherals, cartridge/DMA.
    # The extension checks follow the compressed-instruction checks.
    checks = [(name, expected) for name, expected, group in CHECKS
              if args.group == 'all' or group == args.group]
    groups = {'all': 0, 'memory': 1, 'extensions': 2}
    stamp = datetime.now(timezone.utc).strftime('%Y%m%dT%H%M%S.%fZ')
    report = args.report or ROOT / 'build/test-reports' / (stamp + '-jit')
    report.mkdir(parents=True, exist_ok=False)
    summary = {'format_version': 1, 'operation': 'native_jit_backend', 'status': 'PREPARING',
               'started_utc': stamp, 'runner': git_state(), 'command': sys.argv,
               'port': args.port, 'baud': args.baud, 'test_version': VERSION,
               'group': args.group, 'selected': len(checks), 'received': 0, 'passed': 0, 'failed': 0,
               'sm83_jit': 'PROGRAM CHECKS ONLY',
               'criteria': 'RV32I/M/C 20240411, Zcb 1.0, Zba/Zbb/Zbs 1.0 and RISC-V ELF psABI v1.0'}
    link = None
    save_json(report / 'summary.json', summary)
    try:
        link = Link.open(args.port, args.baud, args.reset, write_timeout=min(1, args.timeout / 2))
        summary['device'] = link.connect(args.timeout)
        link.ping(timeout=args.timeout)
        if not summary['device']['board_ready']:
            raise ValueError('Device board initialization failed')
        if not summary['device']['backends'] & ARM7_JIT:
            checks = [check for check in checks if not check[0].startswith(GBA_CHECKS)]
            summary['selected'] = len(checks)
            summary['gba'] = 'NOT IN FIRMWARE'
        summary['status'] = 'RUNNING'
        save_json(report / 'summary.json', summary)
        enabled = ENABLED.get(int(summary['device']['chip_revision'].split('.')[0]), ())
        link.send(Kind.JIT_TEST, CONTROL.pack(VERSION, groups[args.group], 1))
        with (report / 'actual.bin').open('xb') as raw, (report / 'results.jsonl').open('x') as results:
            while True:
                frame = link.receive(args.timeout)
                if frame.kind == Kind.DONE:
                    if (frame.payload != CONTROL.pack(VERSION, len(checks), 1) or
                            summary['received'] != len(checks)):
                        raise ProtocolError('Incomplete or invalid JIT completion')
                    break
                if frame.kind != Kind.DATA or len(frame.payload) != RESULT.size:
                    raise ProtocolError('Invalid JIT result frame')
                version, check_id, request, actual = RESULT.unpack(frame.payload)
                if (version != VERSION or request != 1 or check_id != summary['received'] or
                        check_id >= len(checks)):
                    raise ProtocolError('Wrong JIT test version, request or check order')
                name, expected = checks[check_id]
                outcome = ''
                if isinstance(expected, tuple):
                    expected, extension = expected
                    if actual == ILLEGAL and extension not in enabled:
                        outcome = 'illegal instruction'
                status = 'PASS' if actual == expected or outcome else 'FAIL'
                row = {'id': check_id, 'name': name, 'status': status, 'expected': expected, 'actual': actual}
                if outcome:
                    row['outcome'] = outcome
                raw.write(struct.pack('<II', frame.sequence, len(frame.payload)) + frame.payload)
                results.write(json.dumps(row) + '\n')
                raw.flush()
                results.flush()
                link.acknowledge(frame)
                summary['received'] += 1
                summary['passed' if status == 'PASS' else 'failed'] += 1
                summary['last_acknowledged_batch'] = frame.sequence
                print(f'{status}: {name}; expected=0x{expected:08x} actual=0x{actual:08x}{f" ({outcome})" if outcome else ""}', flush=True)
        summary['status'] = 'BACKEND FAIL' if summary['failed'] else 'BACKEND PASS'
        return 1 if summary['failed'] else 0
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
        summary['not_run'] = summary['selected'] - summary['received']
        if link:
            summary['device_logs'] = list(link.logs)
            link.close()
        save_json(report / 'summary.json', summary)
        gba = '; ARM7TDMI and GBA checks: NOT IN FIRMWARE' if 'gba' in summary else ''
        print(f'{summary["status"]}; group={args.group}; single-step suites: NOT RUN{gba}. Report: {report}',
              flush=True)
        if 'error' in summary:
            print(summary['error'], file=sys.stderr)
    return 2


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('port')
    parser.add_argument('--group', choices=('all', 'memory', 'extensions'), default='all',
                        help='Select all checks, mapped memory, cache deletion, peripherals and cartridge/DMA '
                             'checks, or the Zcb, Zba, Zbb and Zbs instructions')
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

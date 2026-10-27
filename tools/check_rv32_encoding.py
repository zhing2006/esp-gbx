#!/usr/bin/env python3
"""Compare the host encoding fixture with ESP-IDF's external RV32 assembler.

Run inside the IDF environment after building host target gbx_rv32_encoding.
No generated instructions execute on the host; expected bytes come from GNU as/ld.
"""

import argparse
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('fixture', type=Path, help='Host gbx_rv32_encoding executable')
    parser.add_argument('--report', type=Path, required=True, help='New artifact directory')
    parser.add_argument('--prefix', default='riscv32-esp-elf-', help='Assembler tool prefix')
    args = parser.parse_args()
    args.report.mkdir(parents=True, exist_ok=False)
    rows = subprocess.check_output([str(args.fixture.resolve())], text=True).splitlines()
    # Four hex digits mark a 16-bit (C extension) encoding. The assembler
    # compresses only those lines, whose mnemonics name the 16-bit form.
    encodings, assembly, lines = [], [], []
    for row in rows:
        word, text = row.split('\t', 1)
        size = len(word) // 2
        encodings.append(int(word, 16).to_bytes(size, 'little'))
        assembly.append(text)
        lines += ['.option rvc' if size == 2 else '.option norvc', text]
    source = args.report / 'reference.s'
    source.write_text('.option norelax\n.text\n' + '\n'.join(lines) + '\n')
    obj, elf, binary = (args.report / name for name in ('reference.o', 'reference.elf', 'reference.bin'))
    subprocess.run([args.prefix + 'as', '-march=rv32imc_zba_zbb_zbs', '-mabi=ilp32', '-mno-relax',
                    '-o', str(obj), str(source)], check=True)
    subprocess.run([args.prefix + 'ld', '-m', 'elf32lriscv', '--no-relax', '-Ttext=0x10000000',
                    '-e', '0', '-o', str(elf), str(obj)], check=True)
    subprocess.run([args.prefix + 'objcopy', '-O', 'binary', '--only-section=.text',
                    str(elf), str(binary)], check=True)
    expected = binary.read_bytes()
    actual = b''.join(encodings)
    (args.report / 'actual.bin').write_bytes(actual)
    failures, at = [], 0
    for i, (encoding, text) in enumerate(zip(encodings, assembly)):
        if expected[at:at + len(encoding)] != encoding:
            failures.append({'instruction': i, 'assembly': text, 'actual_bytes': encoding.hex(),
                             'expected_bytes': expected[at:at + len(encoding)].hex()})
        at += len(encoding)
    passed = bool(encodings) and actual == expected
    summary = {'status': 'PASS' if passed else 'FAIL', 'instructions': len(encodings),
               'expected_bytes': len(expected), 'actual_bytes': len(actual), 'failures': failures,
               'assembler': subprocess.check_output([args.prefix + 'as', '--version'], text=True),
               'isa_reference': 'riscv/riscv-isa-manual@6ee57d8997021aaa8025e208164763fe12e03c19'}
    (args.report / 'summary.json').write_text(json.dumps(summary, indent=2) + '\n')
    print(f'{summary["status"]}: {len(encodings)} RV32 encodings; report: {args.report}')
    return 0 if passed else 1


if __name__ == '__main__':
    raise SystemExit(main())

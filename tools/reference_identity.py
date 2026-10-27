#!/usr/bin/env python3
"""Fingerprint each interpreter and its shared execution environment, not JIT code.

This certifies inputs to a validation run, not correctness by itself. Runtime
board/revision and suite identity are checked separately by the test runner.
SM83 and ARM7TDMI get one identity each, so that a change to one system's
core or runner does not invalidate the other's baselines.
Always run at build time; rewrite the generated header only on content changes.
"""

import argparse
import hashlib
import json
from pathlib import Path
import subprocess


# Files of one system only, excluded from the other system's identity.
SYSTEMS = {
    'sm83': ('components/core/include/gbx/gb/', 'components/core/src/gb/',
             'tests/device/sm83_runner.', 'tests/protocol/execution.h'),
    'arm7': ('components/core/include/gbx/gba/', 'components/core/src/gba/',
             'tests/device/arm7_runner.', 'tests/protocol/arm7_execution.h'),
}


def repository_identity(directory):
    def git(*arguments):
        return subprocess.check_output(['git', '-C', str(directory), *arguments])
    untracked = {}
    for name in git('ls-files', '--others', '--exclude-standard', '-z').decode().split('\0'):
        if name:
            untracked[name] = hashlib.sha256((directory / name).read_bytes()).hexdigest()
    return {'commit': git('rev-parse', 'HEAD').decode().strip(),
            'changes': hashlib.sha256(git('diff', 'HEAD', '--binary', '--submodule=short')).hexdigest(),
            'untracked': untracked}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ('root', 'sdkconfig', 'lock', 'compiler', 'idf', 'commands', 'output'):
        parser.add_argument('--' + name, type=Path, required=True)
    parser.add_argument('--board', required=True)
    args = parser.parse_args()
    root = args.root.resolve()
    files = set()
    for folder in ('components/platform', 'components/board', f'boards/{args.board}',
                   'tests/device', 'tests/protocol'):
        files.update(path for path in (root / folder).rglob('*')
                     if path.is_file() and path.suffix in ('.h', '.cpp', '.c', '.S', '.txt', '.yml'))
    for folder, suffixes in (('include', ('.h',)), ('src', ('.h', '.cpp'))):
        files.update(path for path in (root / 'components/core' / folder).rglob('*')
                     if path.suffix in suffixes and 'jit' not in path.relative_to(root / 'components/core').parts)
    files.update(root / name for name in ('CMakeLists.txt', 'components/core/CMakeLists.txt',
                 'cmake/core_layout.cmake', 'tools/link_core_layout.py',
                 'tools/reference_identity.py'))
    # These native backend fixtures do not participate in interpreter execution.
    files.difference_update(root / name for name in ('tests/device/jit_test.cpp',
                            'tests/device/jit_abi.S', 'tests/protocol/jit_test.h',
                            'tests/device/sm83_blocks.cpp', 'tests/device/sm83_blocks.h',
                            'tests/device/arm7_blocks.cpp', 'tests/device/arm7_blocks.h'))
    hashes = {path.relative_to(root).as_posix(): hashlib.sha256(path.read_bytes()).hexdigest()
              for path in sorted(files)}
    shared = {}
    for name, path in (('sdkconfig', args.sdkconfig), ('dependencies.lock', args.lock),
                       ('compiler', args.compiler)):
        shared[name] = hashlib.sha256(path.read_bytes()).hexdigest()
    commands = {Path(entry['file']).resolve().relative_to(root).as_posix(): entry['command']
                for entry in json.loads(args.commands.read_text()) if Path(entry['file']).resolve() in files}
    # IDF puts architecture/ABI flags in response files rather than the command.
    shared['toolchain_flags'] = {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
                                 for path in sorted((args.commands.parent / 'toolchain').glob('*flags'))}
    shared['compiler_version'] = subprocess.check_output([str(args.compiler), '--version'], text=True)
    shared['idf'] = repository_identity(args.idf)
    # A top-level '-dirty' gitlink is not enough: hash each initialized nested
    # dependency's contents so a second edit cannot reuse the first edit's ID.
    submodules = subprocess.check_output(['git', '-C', str(args.idf), 'submodule',
        'foreach', '--quiet', '--recursive', 'git rev-parse --show-toplevel'], text=True).splitlines()
    shared['idf_submodules'] = {str(Path(path).relative_to(args.idf)): repository_identity(Path(path))
                                for path in submodules}
    identities = {}
    for system in SYSTEMS:
        excluded = tuple(prefix for other, prefixes in SYSTEMS.items() if other != system for prefix in prefixes)
        inputs = dict(shared)
        inputs['files'] = {name: digest for name, digest in hashes.items() if not name.startswith(excluded)}
        inputs['commands'] = {name: command for name, command in commands.items() if name in inputs['files']}
        identities[system] = {'sha256': hashlib.sha256(json.dumps(inputs, sort_keys=True).encode()).hexdigest(),
                              'inputs': inputs}
    text = '// Generated by tools/reference_identity.py. Do not edit.\n#pragma once\n#include <cstdint>\n'
    for system, constant in (('sm83', 'kSm83Identity'), ('arm7', 'kArm7Identity')):
        digest = bytes.fromhex(identities[system]['sha256'])
        text += f'inline constexpr uint8_t {constant}[32] = {{' + ','.join(f'0x{value:02x}' for value in digest) + '};\n'
    args.output.parent.mkdir(parents=True, exist_ok=True)
    if not args.output.exists() or args.output.read_text() != text:
        args.output.write_text(text)
    args.output.with_suffix('.json').write_text(json.dumps(identities, indent=2) + '\n')


if __name__ == '__main__':
    main()

#!/usr/bin/env python3
"""Place the execution core at the start of internal RAM in IDF's linker script.

    link_core_layout.py SECTIONS_LD OUTPUT FRAGMENT...

The FRAGMENTs, components/core/linker.lf and components/jit/linker.lf, map
the execution core's objects to internal RAM and keep their cold functions
in flash. ldgen lists each object's sections accordingly, in its own order
among IDF's code. This script moves those lists to the start of internal
RAM in a fixed order, so that other code does not move them.
"""
from pathlib import Path
import re
import sys

# Retain object-local function order. In internal RAM their fetches do not
# go through the L2 cache and PSRAM, which the other core's tasks share.
# First the code both systems run: block lookup and the worker queues.
# The GB's core stays in flash, see components/core/linker.lf.
SHARED = [('jit', 'cache_lookup'), ('core', 'offload')]
# Then the GBA's execution core: the interpreter and the devices, the JIT's
# runtime, then the renderer, which runs on the other core. The sound
# registers and the mixer came last, so that they moved none of the others.
# Compilation and tests must not move them; the code generator stays
# outside this group.
GBA = [('core', 'arm7'), ('core', 'arm7_mapped'), ('core', 'gba_scheduler'), ('core', 'gba_memory'),
       ('core', 'gba_io'), ('core', 'gba_irq'), ('core', 'gba_timer'), ('core', 'gba_dma'), ('core', 'gba_ppu'),
       ('core', 'arm7_jit_runtime'), ('core', 'gba_renderer'), ('core', 'gba_sound'), ('core', 'gba_mixer')]
# The part of it that runs on the other core, the renderer and the mixer.
OTHER_CORE = [('core', 'gba_renderer'), ('core', 'gba_mixer')]


def section(lines, name, needed=True):
    """The first and last line of an output section."""
    start = [i for i, line in enumerate(lines) if line.strip() == f'{name} :']
    if len(start) > 1 or (needed and not start):
        raise SystemExit(f'IDF linker script: no single {name}; review the core placement')
    if not start:
        return 0, 0
    end = next(i for i in range(start[0], len(lines)) if lines[i].lstrip().startswith('} >'))
    return start[0], end


def take(lines, name, archive, obj, needed=True):
    """Removes ldgen's input section description of an object from an output section."""
    start, end = section(lines, name, needed)
    file = f'*lib{archive}.a:{obj}.*'
    found = [i for i in range(start, end) if lines[i].strip() == file or lines[i].strip().startswith(file + '(')]
    if len(found) > 1 or (needed and not found):
        raise SystemExit(f'{name}: {len(found)} ldgen lines for {obj} in lib{archive}.a, expected one; '
                         'map it noflash in its linker.lf')
    line = lines.pop(found[0]).strip() if found else file
    # When all of an object's sections of a kind are cold, ldgen writes the
    # file alone, which would take every section left in it, debug info too.
    return [line] if line != file else []


def insert_after(lines, name, marker, new):
    start, end = section(lines, name)
    found = [i for i in range(start, end) if lines[i].strip() == marker]
    if len(found) != 1:
        raise SystemExit(f'IDF linker script: no single {marker} in {name}; review the core placement')
    lines[found[0] + 1:found[0] + 1] = new


def main():
    source, destination, *fragments = sys.argv[1:]
    lines = Path(source).read_text().split('\n')
    objects = SHARED + GBA
    # P4 before v3 splits internal RAM: the code and data IDF links stay
    # below the second-stage bootloader, which leaves too little room for
    # the GBA's core, and the rest takes the second data section and
    # the heap, which the JIT's pools already run from. There the GBA's core
    # goes into that data section, apart from the other core's part, which
    # fits below the bootloader and leaves that much more of the heap to the
    # pools. Later chips have one region.
    split = section(lines, '.dram1.data', False) != (0, 0)
    low = SHARED + (OTHER_CORE if split else GBA)
    high = [obj for obj in GBA if obj not in low]
    text = [line for archive, obj in low for line in take(lines, '.iram0.text', archive, obj)]
    gba_text = [line for archive, obj in high for line in take(lines, '.iram0.text', archive, obj)]
    # Their read-only data, such as jump tables, goes with them. Some have none.
    rodata = [line for archive, obj in low for line in take(lines, '.dram0.data', archive, obj, False)]
    gba_text += [line for archive, obj in high for line in take(lines, '.dram0.data', archive, obj, False)]
    # The second data section, where there is one, gets the same lists from
    # ldgen. The sections above take the sections; drop the copies.
    for archive, obj in objects:
        take(lines, '.dram1.data', archive, obj, False)
    # ldgen skips a cold function that no object defines, as after a rename,
    # which would leave it in internal RAM unnoticed.
    start, end = section(lines, '.flash.text')
    flash = '\n'.join(lines[start:end])
    for fragment in fragments:
        for obj, symbol in re.findall(r'^\s*(\w+):(\w+) \(default\)', Path(fragment).read_text(), re.M):
            if f'.text.{symbol})' not in flash and f'.text.{symbol} ' not in flash:
                raise SystemExit(f'{fragment}: {obj}:{symbol} is not in the object')
    insert_after(lines, '.iram0.text', '_iram_text_start = ABSOLUTE(.);',
                 ['    . = ALIGN(64);', '    _gbx_core_text_start = ABSOLUTE(.);'] +
                 [f'    {line}' for line in text] + ['    _gbx_core_text_end = ABSOLUTE(.);'])
    insert_after(lines, '.dram0.data', '*(.data .data.*)', [f' {line}' for line in rodata])
    if split:
        # Inside the data IDF reserves from the heap there (heap's
        # memory_layout.c: _data_start_high to _heap_start_high).
        insert_after(lines, '.dram1.data', '_data_start_high = ABSOLUTE(.);',
                     ['    . = ALIGN(64);', '    _gbx_gba_text_start = ABSOLUTE(.);'] +
                     [f'    {line}' for line in gba_text] + ['    _gbx_gba_text_end = ABSOLUTE(.);'])
    Path(destination).write_text('\n'.join(lines))


if __name__ == '__main__':
    main()

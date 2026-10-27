#!/usr/bin/env python3
"""Place the execution core at the start of internal RAM in IDF's linker script.

    link_core_layout.py SDKCONFIG SECTIONS_LD OUTPUT FRAGMENT...

The FRAGMENTs, components/core/linker.lf and components/jit/linker.lf, map
the execution core's objects to internal RAM and keep their cold functions
in flash. ldgen lists each object's sections accordingly, in its own order
among IDF's code. This script moves those lists to the start of internal
RAM in a fixed order, so that other code does not move them. SDKCONFIG
says which systems the build has (GBX_SYSTEMS in components/core/Kconfig)
and decides the fragments' conditions as ldgen does.
"""
from pathlib import Path
import re
import sys

# Retain object-local function order. In internal RAM their fetches do not
# go through the caches and PSRAM, which the other core's tasks share.
# First the code both systems run: block lookup and the worker queues.
# With the GBA in the build the GB's core stays in flash, see
# components/core/linker.lf.
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
GBA_OTHER_CORE = [('core', 'gba_renderer'), ('core', 'gba_mixer')]
# A build of the GB alone: its interpreter, the devices, the run loop and
# the JIT's dispatcher and runtime, then the mixer, which runs on the other
# core. The PPU's object holds its renderer, which runs there too.
GB = [('core', 'sm83'), ('core', 'scheduler'), ('core', 'memory'), ('core', 'cartridge'), ('core', 'rtc'),
      ('core', 'ppu'), ('core', 'apu'), ('core', 'system'), ('core', 'sm83_jit'), ('core', 'sm83_jit_runtime'),
      ('core', 'mixer')]
GB_OTHER_CORE = [('core', 'mixer')]


def enabled(sdkconfig):
    """The sdkconfig's options that are set to y, without CONFIG_."""
    return {m[1] for m in re.finditer(r'^CONFIG_(\w+)=y$', Path(sdkconfig).read_text(), re.M)}


def active_lines(fragment, options):
    """The fragment's lines that apply: those of the branches of its
    `if NAME = y:` and `else:` conditions that the options take."""
    lines, taken = [], []  # taken: (indent of the condition, whether its branch applies)
    for line in Path(fragment).read_text().split('\n'):
        if not line.strip() or line.lstrip().startswith('#'):
            continue
        indent = len(line) - len(line.lstrip())
        while taken and (indent < taken[-1][0] or (indent == taken[-1][0] and line.strip() != 'else:')):
            taken.pop()
        if m := re.fullmatch(r'\s*if (\w+) = y:', line):
            taken.append((indent, all(t for _, t in taken) and m[1] in options))
            continue
        if line.strip() == 'else:':
            if not taken or taken[-1][0] != indent:
                raise SystemExit(f'{fragment}: else without if')
            outer = all(t for _, t in taken[:-1])
            taken[-1] = (indent, outer and not taken[-1][1])
            continue
        if re.match(r'\s*(if|elif)\b', line):
            raise SystemExit(f'{fragment}: only `if NAME = y:` and `else:` conditions are supported')
        if all(t for _, t in taken):
            lines.append(line)
    return lines


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
    sdkconfig, source, destination, *fragments = sys.argv[1:]
    options = enabled(sdkconfig)
    lines = Path(source).read_text().split('\n')
    core, other_core = (GB, GB_OTHER_CORE) if 'GBX_SYSTEMS_GB_ONLY' in options else (GBA, GBA_OTHER_CORE)
    objects = SHARED + core
    # P4 before v3 splits internal RAM: the code and data IDF links stay
    # below the second-stage bootloader, which leaves too little room for
    # the GBA's core, and the rest takes the second data section and
    # the heap, which the JIT's pools already run from. There the GBA's core
    # goes into that data section, apart from the other core's part, which
    # fits below the bootloader and leaves that much more of the heap to the
    # pools. Later chips have one region.
    split = section(lines, '.dram1.data', False) != (0, 0)
    low = SHARED + (other_core if split else core)
    high = [obj for obj in core if obj not in low]
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
        for obj, symbol in re.findall(r'^\s*(\w+):(\w+) \(default\)', '\n'.join(active_lines(fragment, options)),
                                      re.M):
            if f'.text.{symbol})' not in flash and f'.text.{symbol} ' not in flash:
                raise SystemExit(f'{fragment}: {obj}:{symbol} is not in the object')
    insert_after(lines, '.iram0.text', '_iram_text_start = ABSOLUTE(.);',
                 ['    . = ALIGN(64);', '    _gbx_core_text_start = ABSOLUTE(.);'] +
                 [f'    {line}' for line in text] + ['    _gbx_core_text_end = ABSOLUTE(.);'])
    # The linker reaches data near the global pointer with one instruction
    # instead of two, in a window that depends on the alignment of all the
    # data. Behind the other data, how many of the core's tables lie in it
    # depends on the app's and the tests' data, and with it the code's
    # size. A build of the GB alone puts them below the window, which no
    # build relaxes.
    if 'GBX_SYSTEMS_GB_ONLY' in options:
        insert_after(lines, '.dram0.data', '*(.data1)', [f'    {line}' for line in rodata])
    else:
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

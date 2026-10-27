"""GBA ROM index: the ROMs of docs/plans/gba/task-1-1.md "用例选择" and the
self-written ones of task gba 1-2, each with its criterion, end condition,
time limit, module tags and the tasks it depends on. No expectation comes
from an emulator: registers, console text and pictures are judged by each
suite's own rules, and frames are compared only with the reference frames
in tests/gba/frames (tools/gba_reference_frames.py).

Criteria:
  register  jsmolka's m_test_eval convention, which the self-written ROMs
            follow: at the idle loop the named register is 0, otherwise the
            first failed test (jsmolka lib/macros.inc, task gba 1-2).
  alyosha   alyosha-gba-tests draw "All tests passed" or "Failed test NNN"
            in mode 4 at y = 76 (lib/macros.inc m_test_eval); the text
            starts at x = 56 for the first and at x = 60 for the second.
            Some ROMs overwrite the register the macro reports with digits
            of the text (prefetcher_boundary_*), so the drawn text decides;
            a ROM that draws none is judged by r12 as above.
  nba       nba-hw-test print "<name>: PASS <value>" for every test_expect
            and "congratulations!" when all pass (lib/source/test.c); the
            index gives the PASS lines a ROM prints. The board reads the
            text from the BG0 map, see tests/protocol/gba_rom_test.h.
  frame     the final frame equals the reference frame (frame 60 after
            power-on, see tools/gba_reference_frames.py).
  manual    the final frame is saved for a person to look at.
"""

import hashlib
from pathlib import Path

CYCLES_PER_SECOND = 1 << 24
FRAME_CYCLES = 280_896
INDEX_VERSION = 2
SUPPORTED_TASKS = {'2-1', '2-2', '2-3', '2-4', '5-2'}
KEYS = dict(a=1, b=2, select=4, start=8, right=16, left=32, up=64, down=128, r=256, l=512)

# Report-only ROMs (task gba 1-1).
REPORT_ONLY = {
    'alyosha-gba-tests/fifo_dma/fifo', 'alyosha-gba-tests/fifo_dma/fifo_dma_disable_3',
    'alyosha-gba-tests/fifo_dma/fifo_dma_disable_4', 'alyosha-gba-tests/fifo_dma/fifo_dma_disable_5',
    'alyosha-gba-tests/Interactions/Halt_DMA_IRQ_Read_OAM', 'alyosha-gba-tests/STM_ALU_IMM',
    'alyosha-gba-tests/ppu/Sprite_Last_VRAM_Access', 'alyosha-gba-tests/ppu/Sprite_Last_VRAM_Access_Free',
    'alyosha-gba-tests/ppu/Sprite_Disable_Midline', 'alyosha-gba-tests/ppu/Sprite_Disable_on_VRAM_Access',
    'alyosha-gba-tests/ppu/stripes_x_offset', 'nba-hw-test/ppu/bgpd/bgpd', 'nba-hw-test/ppu/bgx/bgx',
    'nba-hw-test/ppu/greenswap/greenswap', 'nba-hw-test/ppu/sprite-hmosaic/sprite-hmosaic',
}
# Report only as well: required by task gba 1-1, failing on both backends
# and left for milestone 6 in task gba 2-6, which gives the evidence. Each
# conflicts with ROMs that pass: the Bus three with nba-hw-test dma/latch,
# Internal_Cycle_DMA_Mul and status-irq-dma#dma with the DMA start delay
# of alyosha Halt_DMA_IRQ, Internal_Cycle_DMA_IRQ* and
# DMA_pause_timing_end_4, timer_reset with DMA_pause_timing_mid_1 and _2.
# fifo_6's interrupt handler reads the timer 4 cycles sooner than here,
# for a cause not found.
DEFERRED = {
    'alyosha-gba-tests/Bus/DMA_IWRAM_Bus', 'alyosha-gba-tests/Bus/DMA_OAM_Bus',
    'alyosha-gba-tests/Bus/LDRSH_misaligned', 'alyosha-gba-tests/Interactions/Internal_Cycle_DMA_Mul',
    'alyosha-gba-tests/fifo_dma/fifo_6', 'alyosha-gba-tests/timer/timer_reset',
    'nba-hw-test/ppu/status-irq-dma/status-irq-dma#dma',
}
# Registers other than r12 (task gba 1-1).
REGISTERS = {'jsmolka-gba-tests/thumb/thumb': 7, 'alyosha-gba-tests/ppu/start_up_vbl_irq': 7,
             'alyosha-gba-tests/ppu/start_up_vbl_irq_halt': 7}
# nba-hw-test: PASS lines, whether the summary is printed, and frames.
NBA = {
    'bus/128kb-boundary/128kb-boundary': (18, False, 300),
    'dma/burst-into-tears/burst-into-tears': (3, True, 300),
    'dma/force-nseq-access/force-nseq-access': (2, True, 300),
    'dma/latch/latch': (4, True, 300),
    'dma/start-delay/start-delay': (1, True, 300),
    'haltcnt/haltcnt': (6, True, 300),
    'irq/irq-delay/irq-delay': (3, True, 300),
    'timer/reload/reload': (7, True, 300),
    'timer/start-stop/start-stop': (2, True, 300),
    'timer/tick-before-reload/tick-before-reload': (2, True, 300),
    'ppu/vram-mirror/vram-mirror': (10, True, 600),
}
# ppu/status-irq-dma: a menu whose entries run on release of A; Down moves
# down, starting from the first entry each time. Each entry is a case: the
# menu shows by frame 30, then Down the given times, then A. The DMA entry
# prints no summary. Name, Downs, PASS lines, summary, frames.
STATUS_IRQ_DMA = [('dispstat', 0, None, True, 4000), ('vcount', 1, None, True, 1300),
                  ('irq', 2, None, True, 1300), ('dma', 3, 3, False, 300)]
# Tasks and module tags by path prefix, most specific first.
TAGS = [
    ('jsmolka-gba-tests/arm/', ('2-1',), ('cpu',)),
    ('jsmolka-gba-tests/thumb/', ('2-1',), ('cpu',)),
    ('jsmolka-gba-tests/memory/', ('2-1',), ('memory',)),
    ('jsmolka-gba-tests/bios/', ('2-1', '2-2'), ('bios', 'irq')),
    ('jsmolka-gba-tests/nes/', ('2-1',), ('cpu', 'region')),
    ('jsmolka-gba-tests/save/', ('2-4',), ('save',)),
    ('jsmolka-gba-tests/ppu/', ('2-3',), ('ppu',)),
    ('alyosha-gba-tests/arm/', ('2-1',), ('cpu',)),
    ('alyosha-gba-tests/thumb/', ('2-1',), ('cpu',)),
    ('alyosha-gba-tests/memory/', ('2-1', '2-3'), ('memory', 'ppu')),
    ('alyosha-gba-tests/STM_ALU_IMM', ('2-1',), ('cpu',)),
    ('alyosha-gba-tests/Bus/', ('2-1', '2-2'), ('memory', 'dma')),
    ('alyosha-gba-tests/DMA/', ('2-2',), ('dma',)),
    ('alyosha-gba-tests/fifo_dma/', ('2-2',), ('dma', 'sound', 'timer')),
    ('alyosha-gba-tests/Interactions/', ('2-2',), ('dma', 'irq')),
    ('alyosha-gba-tests/irq/', ('2-2',), ('irq',)),
    ('alyosha-gba-tests/prefetcher/', ('2-1', '2-2'), ('prefetch',)),
    ('alyosha-gba-tests/psr/', ('2-2',), ('cpu', 'irq')),
    ('alyosha-gba-tests/Serial/', ('2-2',), ('serial',)),
    ('alyosha-gba-tests/timer/', ('2-2',), ('timer',)),
    ('alyosha-gba-tests/timing/', ('2-2',), ('bios', 'dma', 'prefetch')),
    ('alyosha-gba-tests/ppu/start_up', ('2-2', '2-3'), ('ppu', 'irq')),
    ('alyosha-gba-tests/ppu/', ('2-3',), ('ppu',)),
    ('nba-hw-test/bus/', ('2-1', '2-2'), ('memory', 'dma')),
    ('nba-hw-test/dma/', ('2-2',), ('dma',)),
    ('nba-hw-test/haltcnt/', ('2-2',), ('irq', 'bios')),
    ('nba-hw-test/irq/', ('2-2',), ('irq',)),
    ('nba-hw-test/timer/', ('2-2',), ('timer',)),
    ('nba-hw-test/ppu/', ('2-3',), ('ppu',)),
    ('gbx/ppu/', ('2-3',), ('ppu',)),
    ('gbx/rom/rom32_cpu', ('2-1',), ('rom',)),
    ('gbx/rom/rom32_dma', ('2-2',), ('rom', 'dma')),
    ('gbx/save/', ('2-4',), ('save',)),
    ('gbx/rtc/', ('5-2',), ('rtc',)),
]
# The fixed regression selection, task gba 2-6: required ROMs with clear
# criteria from each area, under a minute on the Sila P4; the reasons are in
# docs/hardware-testing.md.
REGRESSION = {
    'jsmolka-gba-tests/arm/arm', 'jsmolka-gba-tests/thumb/thumb', 'jsmolka-gba-tests/memory/memory',
    'jsmolka-gba-tests/bios/bios',
    'alyosha-gba-tests/timing/prefetch_enable', 'alyosha-gba-tests/prefetcher/prefetcher_full_arm',
    'alyosha-gba-tests/prefetcher/prefetcher_branch_thumb_arm_3', 'nba-hw-test/bus/128kb-boundary/128kb-boundary',
    'alyosha-gba-tests/irq/IRQ_sub', 'alyosha-gba-tests/Interactions/Halt_DMA_IRQ', 'nba-hw-test/irq/irq-delay/irq-delay',
    'alyosha-gba-tests/timer/timer', 'nba-hw-test/timer/reload/reload',
    'alyosha-gba-tests/DMA/DMA_pause_timing_mid_1', 'nba-hw-test/dma/latch/latch', 'alyosha-gba-tests/fifo_dma/fifo_5',
    'gbx/ppu/bg_affine_mode2', 'gbx/ppu/obj_affine', 'gbx/ppu/window', 'gbx/ppu/blend_alpha', 'gbx/ppu/raster_dma',
    'jsmolka-gba-tests/save/sram', 'gbx/save/flash64_noid', 'gbx/save/eeprom8k_noid',
    'gbx/rom/rom32_cpu',
}


def seconds(value):
    return int(value * CYCLES_PER_SECOND)


def frames(value):
    return value * FRAME_CYCLES


# Conditions a save point waits for, in the bit order of gba_rom_test.h.
CONDITIONS = ('repeat-dma', 'halt', 'timed-writes', 'fifo', 'prefetch', 'backup-busy', 'rtc-busy')
# Save and load backends, in the bit order of gba_rom_test.h.
COMBINATIONS = ('int-int', 'int-jit', 'jit-int', 'jit-jit')


def save_point(at, conditions=(), window=FRAME_CYCLES, reason=''):
    """A state taken at the first instruction boundary at or after `at` ticks
    where all `conditions` hold, at most `window` ticks later, then loaded in
    all four backend combinations."""
    unknown = set(conditions) - set(CONDITIONS)
    if unknown or not 0 <= window <= CYCLES_PER_SECOND:
        raise ValueError(f'Invalid save point: {sorted(unknown)} {window}')
    return dict(ticks=at, window_ticks=window, conditions=list(conditions),
                combinations=list(COMBINATIONS), reason=reason)


# Instant-save points (task gba 4-1, with the clock of 5-2), by ROM ID. Each
# ROM was chosen for a state its result depends on, and each time so that
# the save falls where the ROM uses that state. The times come from
# stepping each ROM on the PC interpreter one instruction boundary at a time
# and recording when each condition held; the board waits for the
# conditions itself, so a time only needs to fall shortly before the state
# it names. Together the points cover every condition, and each runs in all
# four backend combinations. All but the clock's take a fraction of a
# second; the clock's ROM runs for 9 seconds, so it has one point.
STATES = {
    # Check 16 waits for a change of the seconds register, then measures the
    # next second with timers 2 and 3 (tests/gba/roms/rtc/rtc.c), polling
    # the clock all the time. The seconds tick at about 134.4 and 151.2
    # million cycles.
    'gbx/rtc/rtc': [
        save_point(142_000_000, ['rtc-busy'],
                   reason='A transfer within the second that check 16 times: the clock goes on from the state')],
    # The first EEPROM write keeps the chip busy from 17502 to 125855 cycles
    # while the ROM polls it.
    'gbx/save/eeprom8k_noid': [
        save_point(60_000, ['backup-busy'], reason='In the busy time of the first EEPROM write')],
    # The ROM writes the Flash byte by byte from 1184660 cycles on, each byte
    # a command sequence.
    'gbx/save/flash64_noid': [
        save_point(1_184_660, ['backup-busy'], reason='Within the command sequence of the first Flash byte write')],
    # Sound on, the FIFO filled by DMA and drained by timer 0; the ROM times
    # the DMA requests. FIFO A holds samples from 1024 to 1098 cycles.
    'alyosha-gba-tests/fifo_dma/fifo_2': [
        save_point(1_024, ['fifo'], reason='Samples in the FIFO between the DMA requests the ROM times')],
    # The CPU halts from 38810 to 40822 cycles until a DMA's IRQ wakes it,
    # which the ROM times.
    'alyosha-gba-tests/Interactions/Halt_DMA_IRQ': [
        save_point(38_810, ['halt'], reason='In the HALT the DMA\'s IRQ ends')],
    # The ROM times ROM loads with the prefetch buffer filling; it holds
    # halfwords from 549 to 598 cycles.
    'alyosha-gba-tests/prefetcher/prefetcher_full_arm': [
        save_point(549, ['prefetch'], reason='Halfwords in the prefetch buffer during the timed loads')],
    # Frame 60, drawn from about 16416790 to 16613910 cycles, is the judged
    # frame. Its HBlank DMA repeats on every line, from a source that moves.
    'gbx/ppu/raster_dma': [
        save_point(16_515_350, ['repeat-dma', 'halt'],
                   reason='Halfway down the judged frame, the HBlank DMA moved on and the CPU halted')],
    # The ROM sets up its registers while a line of an early frame is drawn,
    # from 1534627 cycles, in frame 6; the judged frame is frame 60.
    'gbx/ppu/window': [
        save_point(1_534_627, ['timed-writes'], reason='Register writes timed within the line being drawn')],
}


def references(data):
    """Reference frames by ROM path, from frames/frames.tsv."""
    table = {}
    for line in (data / 'frames/frames.tsv').read_text().splitlines():
        if line.startswith('#') or line.startswith('suite\t'):
            continue
        suite, rom = line.split('\t')[:2]
        table[f'{suite}/{rom}'] = f'frames/{suite}/{rom[:-4]}.png'
    return table


def manifest(data):
    raw = (data / 'manifest.tsv').read_bytes()
    entries = {}
    for line in raw.decode().splitlines():
        if not line or line.startswith('#') or line.startswith('sha256\t'):
            continue
        digest, size, path = line.split('\t')
        entries[path] = (digest, int(size))
    return hashlib.sha256(raw).hexdigest(), entries


def index(data):
    """Every ROM on the card, in path order, with the fields the runner uses."""
    _, entries = manifest(data)
    frames_by_rom = references(data)
    rows = []
    for path in sorted(p for p in entries if p.startswith('roms/') and p.endswith('.gba')):
        rom = path[len('roms/'):]
        name = rom[:-4]
        suite = rom.split('/')[0]
        tasks, modules = next((t, m) for prefix, t, m in TAGS if rom.startswith(prefix))
        row = dict(id=name, suite=suite, path=path, sha256=entries[path][0], bytes=entries[path][1],
                   requires=list(tasks), modules=list(modules), report_only=name in REPORT_ONLY | DEFERRED,
                   end='idle', criterion='register', register=REGISTERS.get(name, 12), frames=0, markers=[],
                   keys=[], limit_ticks=seconds(10), reference=None)
        inner = rom[len(suite) + 1:-4]
        if rom in frames_by_rom:
            row.update(end='frames', criterion='frame', frames=60, limit_ticks=frames(70),
                       reference=frames_by_rom[rom])
        elif name == 'alyosha-gba-tests/ppu/stripes_x_offset':
            row.update(end='frames', criterion='manual', frames=60, limit_ticks=frames(70))
        elif suite == 'alyosha-gba-tests':
            row['criterion'] = 'alyosha'
        elif suite == 'nba-hw-test' and inner in NBA:
            passes, summary, limit = NBA[inner]
            row.update(criterion='nba', passes=passes, summary=summary, limit_ticks=frames(limit))
        elif suite == 'nba-hw-test' and inner == 'ppu/status-irq-dma/status-irq-dma':
            for entry, downs, passes, summary, limit in STATUS_IRQ_DMA:
                keys = []
                for k in range(downs + 1):
                    at = frames(30 + 10 * k)
                    keys += [dict(ticks=at, keys=KEYS['down'] if k < downs else KEYS['a']),
                             dict(ticks=at + frames(3), keys=0)]
                rows.append(dict(row, id=f'{name}#{entry}', report_only=f'{name}#{entry}' in DEFERRED,
                                 end='text', criterion='nba', passes=passes,
                                 summary=summary, keys=keys, limit_ticks=frames(limit),
                                 markers=['congratulations!', 'total:'] if summary else ['VIDEO DMA:']))
            continue
        rows.append(row)
    for row in rows:
        row['save_points'] = STATES.get(row['id'], [])
    if set(STATES) - {row['id'] for row in rows}:
        raise ValueError('Save points for ROMs not in the index')
    return rows


def unsupported(row):
    missing = [task for task in row['requires'] if task not in SUPPORTED_TASKS]
    return f'requires task {", ".join(missing)}' if missing else None


def verify_data(data, rows):
    """Checks the selected ROMs and reference frames against the manifest that
    the board's HELLO names; returns the manifest's SHA-256."""
    digest, entries = manifest(data)
    for row in rows:
        if unsupported(row):
            continue
        for path in filter(None, (row['path'], row['reference'])):
            payload = (data / path).read_bytes()
            if (hashlib.sha256(payload).hexdigest(), len(payload)) != entries.get(path):
                raise ValueError(f'File does not match the manifest: {path}')
    return digest

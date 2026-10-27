"""DMG-C and CPU CGB E ROM index and integrity checks (no emulator-generated
expectations).

Mooneye dependency audit: refs/tests/mooneye-source, commit
8d742b9d55055f6878a2f3017e0ccf2234cd692c. common/lib/quit.s polls LY,
writes VRAM and reaches its first LD B,B before using the serial hardware.
Each task group below records execution dependencies, not just the test name.
Blargg durations/images: its distributed game-boy-test-roms-howto.md.
rtc3test and mbc3-tester: the howto files distributed with each.
Gambatte: its game-boy-test-roms-howto.md and the testrunner.cpp it cites,
see GAMBATTE_TIMING below.

A ROM may be indexed on both models. The DMG-C entries keep their IDs; the
CPU CGB E entries start with cgb-e/. A CGB runs a ROM whose CGB flag has bit 7
set in CGB mode and the others in compatibility mode (Pan Docs, 0143 - CGB
flag); verify_data() checks each entry's mode against the ROM's header.
Task IDs of the GBC segment start with gbc-.
"""

import hashlib
from pathlib import Path

TICKS_PER_SECOND = 8_388_608
FRAME_TICKS = 140_448  # 70224 dots
INDEX_VERSION = 10
SUPPORTED_TASKS = {'2-1', '2-2', '2-3', '2-4', '3-1', 'gbc-1-1', 'gbc-1-2', 'gbc-1-3'}
MODELS = ('DMG-C', 'CGB E')
# The machine byte of the request, see rom_test.h.
MACHINES = dict(dmg=0, cgb=1, compat=2)
CGB = 'cgb-e/'
# Every ROM produces sound at this rate; both backends must hash equal PCM.
AUDIO_RATE = 48_000
BUTTONS = dict(right=0x01, left=0x02, up=0x04, down=0x08, a=0x10, b=0x20, select=0x40, start=0x80)

# A short, fixed selection with external suite criteria; see hardware-testing.md.
REGRESSION = {
    'blargg/instr_timing/instr_timing',
    'mooneye-test-suite/acceptance/timer/tima_reload',
    'mooneye-test-suite/acceptance/timer/tima_write_reloading',
    'mooneye-test-suite/acceptance/interrupts/ie_push',
    'mooneye-test-suite/acceptance/halt_ime0_ei',
    'mooneye-test-suite/acceptance/oam_dma_restart',
    'mooneye-test-suite/acceptance/oam_dma/sources-GS',
    'mooneye-test-suite/acceptance/ppu/intr_2_mode0_timing_sprites',
    'mooneye-test-suite/acceptance/ppu/stat_irq_blocking',
    'dmg-acid2/dmg-acid2',
    # STAT interrupts taken from a NOP slide and from HALT; WY matching LY
    # in mode 3; the HBlank request IF shows only after it settles; a DMG
    # window at WX 166 that carries into the next line.
    'age-test-roms/stat-interrupt/stat-int-dmgC-cgbBCE',
    'age-test-roms/halt/halt-m0-interrupt-dmgC-cgbBCE',
    'gambatte/window/arg/late_wy_FFto2_ly2_1_dmg08_cgb04c_out3',
    'gambatte/m2int_m0irq/m2int_m0irq_scx3_1_dmg08_cgb04c_out0',
    'gambatte/window/on_screen/wxA6_wy01',
    # CGB E: BG attributes, both VRAM banks, palette RAM and OBJ priority;
    # palette RAM access in each mode; a general-purpose DMA that goes on
    # where the last one stopped; and an HBlank DMA started in mode 0 with
    # the LCD on, then stopped. A KEY1 speed switch each way, timed by TIMA
    # at every timer rate.
    CGB + 'cgb-acid2/cgb-acid2',
    CGB + 'same-suite/ppu/blocking_bgpi_increase',
    CGB + 'same-suite/dma/gbc_dma_cont',
    CGB + 'same-suite/dma/hdma_mode0',
    CGB + 'age-test-roms/speed-switch/spsw-tima-cgbE',
    # LCDC written in mode 3 in CGB mode; LY around line 153 on a CPU CGB E.
    CGB + 'age-test-roms/m3-bg-lcdc/m3-bg-lcdc',
    CGB + 'age-test-roms/ly/ly-cgbE',
}

# All nine bodies and the shared reporting path need only task 2-1 devices.
EARLY = {
    'bits/reg_f': ['cpu', 'flags'],
    'bits/unused_hwio-GS': ['io', 'masks'],
    'bits/mem_oam': ['memory', 'oam-storage', 'lcd-control'],
    'boot_regs-dmgABC': ['boot', 'cpu'],
    'boot_hwio-dmgABCmgb': ['boot', 'io', 'div', 'ppu-basic'],
    'boot_div-dmgABCmgb': ['boot', 'div'],
    'div_timing': ['cpu-timing', 'div'],
    'pop_timing': ['cpu-timing', 'div', 'stack'],
    'instr/daa': ['cpu', 'flags'],
}
INTERRUPTS = '''ei_sequence ei_timing if_ie_registers rapid_di_ei
reti_intr_timing intr_timing interrupts/ie_push'''.split()
HALT = '''di_timing-GS halt_ime0_ei halt_ime0_nointr_timing
halt_ime1_timing halt_ime1_timing2-GS'''.split()
TIMERS = '''div_write rapid_toggle tim00 tim01 tim10 tim11 tim00_div_trigger
tim01_div_trigger tim10_div_trigger tim11_div_trigger tima_reload
tima_write_reloading tma_write_reloading'''.split()
# These CPU tests use OAM DMA to observe fetch/stack timing. pop_timing does not.
DMA_TIMING = '''add_sp_e_timing call_cc_timing call_cc_timing2 call_timing
call_timing2 jp_cc_timing jp_timing ld_hl_sp_e_timing push_timing ret_cc_timing
ret_timing reti_timing rst_timing'''.split()
DMA = '''oam_dma/basic oam_dma/reg_read oam_dma/sources-GS
oam_dma_start oam_dma_restart oam_dma_timing'''.split()
PPU = '''hblank_ly_scx_timing-GS intr_1_2_timing-GS intr_2_0_timing
intr_2_mode0_timing intr_2_mode0_timing_sprites intr_2_mode3_timing
intr_2_oam_ok_timing lcdon_timing-GS lcdon_write_timing-GS stat_irq_blocking
stat_lyc_onoff vblank_stat_intr-GS'''.split()
MBC = {
    'mbc1': '''bits_bank1 bits_bank2 bits_mode bits_ramg multicart_rom_8Mb
ram_256kb ram_64kb rom_16Mb rom_1Mb rom_2Mb rom_4Mb rom_512kb rom_8Mb'''.split(),
    'mbc2': 'bits_ramg bits_romb bits_unused ram rom_1Mb rom_2Mb rom_512kb'.split(),
    'mbc5': 'rom_16Mb rom_1Mb rom_2Mb rom_32Mb rom_4Mb rom_512kb rom_64Mb rom_8Mb'.split(),
}


# v7.0 bundle: these are the tests with a native DMG-blob reference.
MEALYBUG = '''
m2_win_en_toggle
m3_bgp_change
m3_bgp_change_sprites
m3_lcdc_bg_en_change
m3_lcdc_bg_map_change
m3_lcdc_obj_en_change
m3_lcdc_obj_en_change_variant
m3_lcdc_obj_size_change
m3_lcdc_obj_size_change_scx
m3_lcdc_tile_sel_change
m3_lcdc_tile_sel_win_change
m3_lcdc_win_en_change_multiple
m3_lcdc_win_en_change_multiple_wx
m3_lcdc_win_map_change
m3_obp0_change
m3_scx_high_5_bits
m3_scx_low_3_bits
m3_scy_change
m3_window_timing
m3_window_timing_wx_0
m3_wx_4_change
m3_wx_4_change_sprites
m3_wx_5_change
m3_wx_6_change
'''.split()

# CPU CGB E, in compatibility mode: the mooneye tests verified on CGBs, see the
# "Verified results" of each source. In acceptance/ they are exactly the
# tests without a model suffix.
MOONEYE_CGB_MISC = ['boot_div-cgbABCDE', 'boot_hwio-C', 'boot_regs-cgb', 'bits/unused_hwio-C',
                    'ppu/vblank_stat_intr-C']
# SameSuite APU tests that work on CPU CGB E: those without a revision suffix
# (its game-boy-test-roms-howto.md) and those whose suffix names E. The ones
# that write KEY1 run at double speed (see their sources in
# refs/tests/same-suite-source), which arrives with task gbc-1-2.
SAMESUITE_APU = """
channel_1/channel_1_align channel_1/channel_1_align_cpu channel_1/channel_1_delay channel_1/channel_1_duty
channel_1/channel_1_duty_delay channel_1/channel_1_freq_change channel_1/channel_1_freq_change_timing-cgbDE
channel_1/channel_1_nrx2_glitch channel_1/channel_1_nrx2_speed_change channel_1/channel_1_restart
channel_1/channel_1_restart_nrx2_glitch channel_1/channel_1_stop_div channel_1/channel_1_stop_restart
channel_1/channel_1_sweep channel_1/channel_1_sweep_restart channel_1/channel_1_sweep_restart_2
channel_1/channel_1_volume channel_1/channel_1_volume_div
channel_2/channel_2_align channel_2/channel_2_align_cpu channel_2/channel_2_delay channel_2/channel_2_duty
channel_2/channel_2_duty_delay channel_2/channel_2_freq_change channel_2/channel_2_nrx2_glitch
channel_2/channel_2_nrx2_speed_change channel_2/channel_2_restart channel_2/channel_2_restart_nrx2_glitch
channel_2/channel_2_stop_div channel_2/channel_2_stop_restart channel_2/channel_2_volume
channel_2/channel_2_volume_div
channel_3/channel_3_and_glitch channel_3/channel_3_delay channel_3/channel_3_first_sample
channel_3/channel_3_freq_change_delay channel_3/channel_3_restart_delay channel_3/channel_3_restart_during_delay
channel_3/channel_3_restart_stop_delay channel_3/channel_3_shift_delay channel_3/channel_3_shift_skip_delay
channel_3/channel_3_stop_delay channel_3/channel_3_stop_div channel_3/channel_3_wave_ram_sync
channel_4/channel_4_align channel_4/channel_4_delay channel_4/channel_4_equivalent_frequencies
channel_4/channel_4_freq_change channel_4/channel_4_frequency_alignment channel_4/channel_4_lfsr
channel_4/channel_4_lfsr15 channel_4/channel_4_lfsr_15_7 channel_4/channel_4_lfsr_7_15
channel_4/channel_4_lfsr_restart channel_4/channel_4_lfsr_restart_fast channel_4/channel_4_volume_div
div_trigger_volume_10 div_write_trigger div_write_trigger_10 div_write_trigger_volume div_write_trigger_volume_10
""".split()
SAMESUITE_DOUBLE_SPEED = set("""
channel_1/channel_1_align channel_1/channel_1_align_cpu channel_1/channel_1_duty
channel_1/channel_1_freq_change_timing-cgbDE channel_2/channel_2_align channel_2/channel_2_align_cpu
channel_2/channel_2_duty channel_3/channel_3_and_glitch channel_3/channel_3_delay channel_3/channel_3_first_sample
channel_3/channel_3_freq_change_delay channel_3/channel_3_restart_delay channel_3/channel_3_restart_during_delay
channel_3/channel_3_restart_stop_delay channel_3/channel_3_shift_delay channel_3/channel_3_shift_skip_delay
channel_3/channel_3_stop_delay channel_3/channel_3_stop_div channel_3/channel_3_wave_ram_sync
channel_4/channel_4_align
""".split())
# Mealybug's references of a CGB are of CPU CGB C or D (game-boy-test-roms
# v7.0); the D one is taken where there is one. The suite's ROMs are
# monochrome, so a CGB runs them in compatibility mode.
MEALYBUG_CGB = dict.fromkeys("""
m2_win_en_toggle m3_bgp_change m3_bgp_change_sprites m3_lcdc_bg_en_change m3_lcdc_bg_map_change
m3_lcdc_obj_en_change m3_lcdc_obj_en_change_variant m3_lcdc_obj_size_change m3_lcdc_obj_size_change_scx
m3_lcdc_tile_sel_change m3_lcdc_tile_sel_win_change m3_lcdc_win_en_change_multiple m3_lcdc_win_map_change
m3_obp0_change m3_scx_high_5_bits m3_scx_low_3_bits m3_scy_change m3_window_timing m3_window_timing_wx_0
m3_wx_4_change_sprites
""".split(), 'd') | dict.fromkeys("""
m3_lcdc_bg_en_change2 m3_lcdc_bg_map_change2 m3_lcdc_tile_sel_change2 m3_lcdc_tile_sel_win_change2
m3_lcdc_win_map_change2 m3_scx_high_5_bits_change2 m3_scy_change2
""".split(), 'c')

# AGE (task gbc-1-3), by file: the models it runs on, as its name gives them
# (README.md: dmgC is CPU DMG-C, cgbBCE includes CGB E, ncm is a CGB's
# compatibility mode), and its module tags. The CGB mode follows the header.
AGE_TIMING = {
    'stat-interrupt/stat-int-dmgC-cgbBCE': (('DMG-C', 'CGB E'), ['ppu', 'stat', 'interrupt']),
    'stat-interrupt/stat-int-ncmBCE': (('CGB E',), ['ppu', 'stat', 'interrupt']),
    'halt/halt-m0-interrupt-dmgC-cgbBCE': (('DMG-C', 'CGB E'), ['ppu', 'stat', 'interrupt', 'halt']),
    'halt/ei-halt-dmgC-cgbBCE': (('DMG-C', 'CGB E'), ['interrupt', 'halt']),
    'halt/halt-prefetch-dmgC-cgbBCE': (('DMG-C', 'CGB E'), ['interrupt', 'halt']),
    'ly/ly-dmgC-cgbBC': (('DMG-C',), ['ppu', 'stat']),
    'ly/ly-cgbE': (('CGB E',), ['ppu', 'stat']),
    'ly/ly-ncmE': (('CGB E',), ['ppu', 'stat']),
    'lcd-align-ly/lcd-align-ly-cgbE': (('CGB E',), ['ppu', 'stat', 'lcd-control']),
}
# Report only: OAM access follows the mode timing, but is not in the task.
AGE_OAM = {
    'oam/oam-read-dmgC-cgbBC': ('DMG-C',), 'oam/oam-write-dmgC': ('DMG-C',),
    'oam/oam-read-cgbE': ('CGB E',), 'oam/oam-read-ncmE': ('CGB E',),
    'oam/oam-write-cgbBCE': ('CGB E',), 'oam/oam-write-ncmBCE': ('CGB E',),
}
# Screenshots taken at LD B,B, by ROM: the reference of each model.
AGE_MODE3 = {
    'm3-bg-bgp/m3-bg-bgp': {'DMG-C': 'dmgC', 'CGB E': 'ncmE'},
    'm3-bg-lcdc/m3-bg-lcdc': {'DMG-C': 'dmgC', 'CGB E': 'cgbBCE'},
    'm3-bg-lcdc/m3-bg-lcdc-ds': {'CGB E': 'cgbBCE'},
    'm3-bg-lcdc/m3-bg-lcdc-nocgb': {'CGB E': 'ncmBCE'},
    'm3-bg-scx/m3-bg-scx': {'DMG-C': 'dmgC', 'CGB E': 'cgbBCE'},
    'm3-bg-scx/m3-bg-scx-ds': {'CGB E': 'cgbBCE'},
    'm3-bg-scx/m3-bg-scx-nocgb': {'CGB E': 'ncmBCE'},
}

# Gambatte (task gbc-1-3). Its howto: every test runs 15 frames, 1053360
# cycles, from the state after the boot ROM; a ROM whose name has _out<hex>
# then shows the digits in the top left, which testrunner.cpp (commit
# d819bad) compares with its digit patterns, and one with a PNG beside it
# is compared with that PNG. main() in testrunner.cpp picks the models by
# name: dmg08_cgb04c_out for both, dmg08_out for the DMG and cgb04c_out or a
# bare _out for the CGB, each up to its digits; and a PNG named
# _dmg08_cgb04c, _cgb04c or _dmg08. dmg08 runs here as DMG-C and cgb04c,
# a CPU CGB C, as CGB E. AGE finds this timing different on C and E, so the
# CGB E entries only report, and --all leaves them out; select them with
# --suite gambatte --model "CGB E". _outaudio tests are not taken.
GAMBATTE_TIMING = """enable_display halt irq_precedence lcdirq_precedence ly0 lyc0int_m0irq
lyc153int_m2irq lycEnable lycint_ly lycint_lycflag lycint_lycirq lycint_m0stat lycm2int lywrite m0enable
m0int_m0irq m0int_m0stat m0int_m3stat m1 m2enable m2int_m0irq m2int_m0stat m2int_m2irq m2int_m2stat
m2int_m3stat miscmstatirq""".split()
GAMBATTE_MODE3 = ['sprites', 'window']
# Report only on both models.
GAMBATTE_REPORT = ['oam_access']
# Report only on DMG-C as well: the cases of the groups above that fail on
# both backends, nine of them where AGE and mooneye disagree with dmg08.
# docs/plans/gbc/task-1-3.md gives the causes.
GAMBATTE_DMG_REPORT = {
    'enable_display/enable_display_ly0_sprites_m0stat_2_dmg08_cgb04c_out0',
    'enable_display/frame0_m2irq_count_2_dmg08_cgb04c_out91', 'enable_display/frame1_m2irq_count_2_dmg08_cgb04c_out91',
    'enable_display/frame1_m2stat_count_1_dmg08_cgb04c_out91',
    'enable_display/ly0_late_scx7_m3stat_scx0_2_dmg08_out87_cgb04c_out84',
    'enable_display/ly0_late_scx7_m3stat_scx1_1_dmg08_cgb04c_out87',
    'enable_display/ly0_late_scx7_m3stat_scx3_1_dmg08_cgb04c_out87',
    'enable_display/ly0_m0irq_scx1_1_dmg08_cgb04c_outE0', 'halt/late_m0int_halt_m0stat_scx2_3a_dmg08_cgb04c_out0',
    'halt/late_m0int_halt_m0stat_scx3_2b_dmg08_cgb04c_out2', 'halt/late_m0int_halt_m0stat_scx3_3a_dmg08_cgb04c_out0',
    'halt/late_m0int_halt_m0stat_scx3_3b_dmg08_out0_cgb04c_out2',
    'halt/late_m0irq_halt_m0stat_scx3_2b_dmg08_cgb04c_out2',
    'irq_precedence/late_m0irq_vs_tima_scx2_1_dmg08_cgb04c_out4',
    'irq_precedence/late_m0irq_vs_tima_scx2_halt_1_dmg08_cgb04c_out4',
    'lcdirq_precedence/lcdirqprecedence_lycirq_ly90_lcdstat68_dmg08_cgb04c_out1',
    'ly0/lycint152_ly0stat_2_dmg08_cgb04c_outC0', 'ly0/lycint152_lyc0irq_late_retrigger_2_dmg08_cgb04c_outE0',
    'ly0/lycint152_lyc153irq_late_retrigger_2_dmg08_cgb04c_outE0',
    'lyc153int_m2irq/lyc153int_m2irq_late_retrigger_2_dmg08_cgb04c_out0',
    'lycEnable/early_ff45_response_1_dmg08_cgb04c_out0', 'lycEnable/ff41_disable_2_dmg08_out0_cgb04c_out2',
    'lycEnable/ff45_enable_weirdpoint_3_dmg08_out1_cgb04c_out3', 'lycEnable/lcdoff_lycirqen_1_dmg08_cgb04c_outE2',
    'lycEnable/lcdoff_lycirqen_4_dmg08_outE2_cgb04c_outE0',
    'lycEnable/lyc153_late_ff45_enable_3_dmg08_outE0_cgb04c_outE2',
    'm1/lyc143_late_m2enable_lycdisable_2_dmg08_cgb04c_out1', 'm1/lycint143_m1irq_late_retrigger_2_dmg08_cgb04c_out1',
    'm1/lycint_vblankintirq_2_dmg08_cgb04c_out0', 'm1/lycint_vblankirq_late_retrigger_2_dmg08_cgb04c_out0',
    'm1/m1irq_m0disable_2_dmg08_out3_cgb04c_out1', 'm1/m1irq_m2disable_lycdisable_3_dmg08_cgb04c_out1',
    'm1/m1irq_m2enable_lyc_1_dmg08_cgb04c_out1', 'm1/m1irq_m2enable_lyc_2_dmg08_out1_cgb04c_out3',
    'm2enable/late_enable_m0disable_2_dmg08_out0_cgb04c_out2',
    'miscmstatirq/lycstatwirq_trigger_m0_late_ly44_lyc44_08_40_4_dmg08_cgb04c_outE0',
    'sprites/10spritesPrLine_10xposA7_m0irq_2_dmg08_cgb04c_out2',
    'window/arg/late_enable_afterVblank_5_dmg08_cgb04c_out0',
    'window/arg/late_scx_late_wy_FFto4_ly4_wx00_2_dmg08_out3_cgb04c_out0',
    'window/late_disable_early_scx03_wx11_2_dmg08_cgb04c_out3',
    'window/late_disable_late_scx03_wx11_2_dmg08_out3_cgb04c_out0',
    'window/late_disable_scx5_1_dmg08_out3_cgb04c_out0', 'window/late_disable_spx10_wx0f_2_dmg08_cgb04c_out3',
    'window/late_enable_afterVblank_3_dmg08_cgb04c_out0', 'window/m2int_wxA6_spxA7_m0irq_2_dmg08_cgb04c_out2',
    'window/m2int_wxA6_spxA7_m3stat_2_dmg08_out0_cgb04c_out3', 'window/on_screen/wxA6_late_we_reenable_1',
    'window/on_screen/wxA6_late_we_reenable_2', 'window/on_screen/wxA6_late_we_reenable_3',
    'window/on_screen/wxA6_late_we_reenable_4',
}
GAMBATTE = Path(__file__).resolve().parents[2] / 'refs/tests/game-boy-test-roms/gambatte'
GAMBATTE_TICKS = 15 * FRAME_TICKS
# The digits are 8x8 tiles in the top rows; the board sends only those.
GAMBATTE_ROWS = 8
HEX = set('0123456789abcdefABCDEF')


def gambatte_outputs(stem):
    """The expected digits by model, as testrunner.cpp's main() finds them."""
    dmg = cgb = None
    if 'dmg08_cgb04c_out' in stem:
        dmg = cgb = 'dmg08_cgb04c_out'
    elif 'dmg08_out' in stem:
        dmg = 'dmg08_out'
        cgb = 'cgb04c_out' if 'cgb04c_out' in stem else None
    elif '_out' in stem:
        cgb = '_out'
    outputs = {}
    for model, marker in (('DMG-C', dmg), ('CGB E', cgb)):
        if not marker:
            continue
        rest = stem[stem.find(marker) + len(marker):]
        if rest.startswith('audio'):
            continue
        digits = ''
        for c in rest:
            if c not in HEX:
                break
            digits += c.upper()
        outputs[model] = digits
    return outputs


def gambatte_pngs(path):
    """The PNG references beside a ROM, by model, as main() opens them."""
    both = path.with_name(path.stem + '_dmg08_cgb04c.png')
    if both.is_file():
        return {'DMG-C': both, 'CGB E': both}
    return {model: png for model, suffix in (('DMG-C', '_dmg08.png'), ('CGB E', '_cgb04c.png'))
            if (png := path.with_name(path.stem + suffix)).is_file()}


# Each single runs only when selected by suite, module or ID, never by --all.
# They have no suite reference, so the runner saves the frame for a manual
# look. The combined dmg_sound runs all twelve within its 36 seconds.
SOUND_SINGLES = [
    '01-registers', '02-len ctr', '03-trigger', '04-sweep', '05-sweep details',
    '06-overflow on trigger', '07-len sweep period sync', '08-len ctr during power',
    '09-wave read while on', '10-wave trigger while on', '11-regs after power',
    '12-wave write while on',
]

# rtc3test: the buttons that select each subtest and the emulated seconds the
# howto gives for it, counted here from the last release. The ROM reads the
# buttons once per frame (16.7 ms) from 0.09 s on, at PC 051F and 052B, so a
# 0.1-second press or gap spans about six reads.
RTC3TEST = [('basic-tests', ['a'], 13), ('range-tests', ['down', 'a'], 8),
            ('sub-second-writes', ['down', 'down', 'a'], 26)]
# The clock starts at zero, running and without carry.
RTC_START = dict(seconds=0, minutes=0, hours=0, days=0, halt=False, carry=False, subsecond_ticks=0)

# Conditions a save point waits for, in the bit order of rom_test.h.
CONDITIONS = ('oam-dma', 'mode3-change', 'halt', 'ei-pending', 'channel-on', 'rtc-subsecond', 'cart-ram-written',
              'double-speed', 'hdma')
# Save and load backends, in the bit order of rom_test.h.
COMBINATIONS = ('int-int', 'int-jit', 'jit-int', 'jit-jit')


def ticks(seconds):
    return round(seconds * TICKS_PER_SECOND)


def save_point(at, conditions=(), window=FRAME_TICKS, reason=''):
    """A state taken at the first instruction boundary at or after `at` ticks
    where all `conditions` hold, at most `window` ticks later, then loaded in
    all four backend combinations."""
    unknown = set(conditions) - set(CONDITIONS)
    if unknown or not 0 <= window <= TICKS_PER_SECOND:
        raise ValueError(f'Invalid save point: {sorted(unknown)} {window}')
    return dict(ticks=at, window_ticks=window, conditions=list(conditions),
                combinations=list(COMBINATIONS), reason=reason)


# Instant-save points (task 4-1), by ROM ID. Each ROM was chosen for a state
# its result depends on, and each time so that the save falls where the ROM
# uses that state. The times come from mooneye's sources where they can, and
# otherwise from stepping the ROM on the PC interpreter, one instruction
# boundary at a time, and recording when each condition held.
# The board waits for the conditions itself, so a time only needs to fall
# shortly before the state it names. Together the points cover every
# condition, and each runs in all four backend combinations. The short ROMs
# take a fraction of a second each. dmg_sound and rtc3test take most of the
# time, so each has one point, late in dmg_sound to keep its run short.
STATES = {
    # hiram_test restarts OAM DMA after 7 NOPs and reads OAM one cycle before
    # and just after the second transfer ends (oam_dma_restart.s). DMA runs
    # from 272224 to 273576 ticks.
    'mooneye-test-suite/acceptance/oam_dma_restart': [
        save_point(272_900, ['oam-dma'], reason='Within the restarted DMA, before both timed OAM reads')],
    # Part 2 writes cartridge RAM (MBC5, 2 banks) from 0.416 s and copies it
    # to OAM by DMA (sources-GS.s); one transfer runs from 3528520 ticks.
    'mooneye-test-suite/acceptance/oam_dma/sources-GS': [
        save_point(3_528_900, ['oam-dma', 'cart-ram-written'],
                   reason='A DMA from cartridge RAM after the test wrote it')],
    # EIs in a row with a serial interrupt pending; the interrupt must push PC
    # 01A2, after the second EI (ei_sequence.s). The first EI is pending at
    # 184 ticks.
    'mooneye-test-suite/acceptance/ei_sequence': [
        save_point(0, ['ei-pending'], reason='Between the first EI and the instruction after it')],
    # Rounds 3 and 4 time HALT with IME=1 by DIV, and each round starts from
    # an EI;HALT that waits for VBlank (halt_ime1_timing2-GS.s). The EI before
    # the HALT that leads into round 3 is pending at 553152 ticks; round 3's
    # timed HALT lasts from 693704 to 833896 ticks.
    'mooneye-test-suite/acceptance/halt_ime1_timing2-GS': [
        save_point(553_000, ['ei-pending'], reason='EI pending before HALT: the wake-up must dispatch'),
        save_point(760_000, ['halt'], reason='In the HALT that round 3 times with DIV')],
    # The Mealybug tests change PPU registers during mode 3 (README); this one
    # writes BGP several times on every line. Frame 11, drawn from about
    # 1403900 to 1535224 ticks, is the frame taken at LD B,B at 1535384, so
    # a save in its middle also checks that the rows drawn before it stay.
    'mealybug-tearoom-tests/ppu/m3_bgp_change': [
        save_point(1_470_000, ['mode3-change'], reason='Mid-line BGP write in the middle of the judged frame')],
    # dmg-acid2 writes registers in mode 2 of set lines, never in mode 3, and
    # its failure images include the window's internal line counter (README).
    # Frame 11, drawn from about 2466500 to 2597856 ticks, is the one taken
    # at LD B,B; line 72 is drawn at about 2532200.
    'dmg-acid2/dmg-acid2': [
        save_point(2_532_200, window=0, reason='Halfway down the judged frame, with the window line counter in use')],
    # Basic tests: Tick times two RTC ticks with the timer, 1 ms tolerance
    # (rtc3test tests.md). The ROM writes the clock's control register for RTC
    # on at 0.29 s and for RTC off at 3.02 s, so 2.5 s falls within Tick,
    # half a second after a tick of the clock started at zero.
    'rtc3test/basic-tests': [
        save_point(ticks(2.5), ['rtc-subsecond'], window=0, reason='Halfway between the two ticks Tick times')],
    # The singles run in order within the 36 seconds. CH1 sweeps between 27.46
    # and 29.37 s, longest from 29.003 to 29.097 s. The ROM has written
    # cartridge RAM from 0.012 s on.
    'blargg/dmg_sound/dmg_sound': [
        save_point(ticks(29.05), ['channel-on', 'cart-ram-written'], window=0,
                   reason='CH1 sweeping during the sweep tests, with results in cartridge RAM')],
    # CPU CGB E (task 1-2), times from the PC interpreter at task 1-2. The
    # states add the WRAM and VRAM banks, palette RAM, VRAM DMA, the speed and
    # the color frames. interrupt_time runs at double speed from 3643738
    # ticks; its frame at the end of 2 s shows the cycle counts it measured.
    CGB + 'blargg/interrupt_time/interrupt_time': [
        save_point(4_000_000, ['double-speed'], reason='At double speed, between the timed interrupts')],
    # spsw-tima-cgbE switches to double speed at 132722 ticks; the CPU then
    # waits 0x20000 ticks in HALT, which ends at its own time.
    CGB + 'age-test-roms/speed-switch/spsw-tima-cgbE': [
        save_point(132_722, ['halt', 'double-speed'], reason='In the pause of a speed switch, before the CPU wakes')],
    # hdma_mode0 starts an HBlank DMA in mode 0, which runs from 618216 to
    # 618360 ticks, then stops it (SameSuite dma/hdma_mode0.asm).
    CGB + 'same-suite/dma/hdma_mode0': [
        save_point(618_200, ['hdma'], reason='An HBlank DMA under way, before the test stops it')],
    # cgb-acid2 reaches LD B,B at about 3313500 ticks; line 72 of the judged
    # frame is drawn about 70000 ticks before.
    CGB + 'cgb-acid2/cgb-acid2': [
        save_point(3_243_000, window=0, reason='Halfway down the judged color frame, both VRAM banks in use')],
    # dmg-acid2 in DMG compatibility mode: as on the DMG, the judged frame is
    # under way at 2532200 ticks.
    CGB + 'dmg-acid2/dmg-acid2': [
        save_point(2_532_200, window=0, reason='Compatibility mode, halfway down the judged frame')],
}


def press(names, first=0.25, hold=0.1, gap=0.1):
    """Buttons pressed one at a time, from `first` seconds on."""
    script, at = [], first
    for name in names:
        script.append(dict(ticks=ticks(at), action='buttons', value=BUTTONS[name]))
        script.append(dict(ticks=ticks(at + hold), action='buttons', value=0))
        at += hold + gap
    return script


def index():
    rows = []

    def add(suite, name, task, modules, criterion='fibonacci', seconds=120,
            report_only=False, reference=None, path=None, limit_ticks=None,
            script=(), rtc=None, debug_only=False, mode='dmg', end=None,
            frame_rows=0, colors=None, digits=None, in_all=None):
        cgb = mode != 'dmg'
        rows.append(dict(id=f'{CGB if cgb else ""}{suite}/{name}', suite=suite,
                         path=path or f'roms/{suite}/{name}.gb', model=MODELS[cgb], mode=mode,
                         modules=sorted(set(['cpu', *modules, *(['cgb'] if cgb else [])])), requires=[task],
                         end=end or ('ld-b-b' if criterion == 'fibonacci' else 'duration'),
                         criterion=criterion, limit_ticks=limit_ticks or ticks(seconds),
                         report_only=report_only, reference=reference,
                         early_finish=None, script=list(script), rtc=rtc,
                         audio_rate=AUDIO_RATE, debug_only=debug_only,
                         in_all=not debug_only if in_all is None else in_all,
                         frame_rows=frame_rows, colors=colors, digits=digits))

    def moon(name, task, modules):
        add('mooneye-test-suite', name, task, ['ppu-basic', 'memory', *modules])
        # The same test on a CGB, where it was verified; see MOONEYE_CGB_MISC.
        if name.startswith('acceptance/') and '-' not in name.rsplit('/', 1)[-1]:
            add('mooneye-test-suite', name, 'gbc-1-1', ['ppu-basic', 'memory', *modules], mode='compat')

    for name, tags in EARLY.items():
        moon('acceptance/' + name, '2-1', tags)
    for name in INTERRUPTS:
        moon('acceptance/' + name, '2-2', ['interrupt', 'div'])
    for name in HALT:
        moon('acceptance/' + name, '2-2', ['interrupt', 'halt', 'div', 'timer']
             if name == 'halt_ime1_timing' else ['interrupt', 'halt', 'div'])
    for name in TIMERS:
        moon('acceptance/timer/' + name, '2-2', ['timer', 'div', 'interrupt'])
    moon('acceptance/serial/boot_sclk_align-dmgABCmgb', '2-2', ['serial', 'interrupt', 'boot', 'div'])
    for name in DMA_TIMING + DMA:
        tags = ['oam-dma', 'cpu-timing', 'ram-code']
        if name == 'oam_dma/sources-GS':
            tags += ['mbc5', 'cart-ram']
        moon('acceptance/' + name, '2-3', tags)
    for name in PPU:
        moon('acceptance/ppu/' + name, '2-4', ['ppu', 'stat', 'interrupt', 'halt'])
    for mapper, names in MBC.items():
        for name in names:
            moon(f'emulator-only/{mapper}/{name}', '2-3', [mapper, 'cartridge'])
    for name, seconds in [('cpu_instrs/cpu_instrs', 55), ('instr_timing/instr_timing', 1),
                          ('mem_timing/mem_timing', 3), ('mem_timing-2/mem_timing', 4),
                          ('halt_bug', 2), ('oam_bug/oam_bug', 21)]:
        report_only = name.startswith('oam_bug/')
        add('blargg', name, '2-4', ['ppu', 'frame', 'timer', 'interrupt'],
            'frame', seconds, report_only,
            f'roms/blargg/{name}-' + ('dmg.png' if report_only else 'dmg-cgb.png'))
    add('dmg-acid2', 'dmg-acid2', '2-4', ['ppu', 'frame', 'stat', 'interrupt'],
        'frame', 120, reference='roms/dmg-acid2/dmg-acid2-dmg.png')
    rows[-1]['end'] = 'ld-b-b'
    # Acid2's 120-second ceiling is a runner safety bound, not a suite duration.
    add('blargg', 'dmg_sound/dmg_sound', '3-1', ['apu', 'frame', 'div', 'timer', 'interrupt'],
        'frame', 36, reference='roms/blargg/dmg_sound/dmg_sound-dmg.png')
    for name in SOUND_SINGLES:
        add('blargg', 'dmg_sound/rom_singles/' + name, '3-1', ['apu', 'frame', 'div', 'timer', 'interrupt'],
            'frame', 36, report_only=True, debug_only=True)
    # The howto checks the screen "after first 40 frames" without saying how
    # frames are counted. The ROM keeps the LCD off for part of its run, so
    # 40 frames of emulated time end before it draws the result. 60 frames
    # fall after the result is drawn and before the test starts over.
    add('mbc3-tester', 'mbc3-tester', '3-1', ['mbc3', 'mbc30', 'cartridge', 'frame'], 'frame',
        limit_ticks=60 * FRAME_TICKS, reference='roms/mbc3-tester/mbc3-tester-dmg.png')
    for name, buttons, seconds in RTC3TEST:
        script = press(buttons)
        end = script[-1]['ticks']
        if name == 'sub-second-writes':
            # Taking the clock state and writing it back, at varying
            # sub-second phases, must not change the result.
            script += [dict(ticks=at, action='rtc-round-trip') for at in
                       range(ticks(1), end + ticks(seconds), ticks(0.37))]
        add('rtc3test', name, '3-1', ['mbc3', 'rtc', 'cartridge', 'joypad', 'frame', 'timer', 'div', 'interrupt'],
            'frame', path='roms/rtc3test/rtc3test.gb', limit_ticks=end + ticks(seconds),
            script=script, rtc=RTC_START, reference=f'roms/rtc3test/rtc3test-{name}-dmg.png')
    for name in MEALYBUG:
        add('mealybug-tearoom-tests', 'ppu/' + name, '2-4', ['ppu', 'frame', 'raster'],
            'frame', 120, True, f'roms/mealybug-tearoom-tests/ppu/{name}_dmg_blob.png')
        rows[-1]['end'] = 'ld-b-b'
        rows[-1]['reference_model'] = 'DMG blob; exact CPU revision unconfirmed'
    cgb_rows(add, moon)
    timing_rows(add)
    for row in rows:
        row['save_points'] = STATES.get(row['id'], [])
    if set(STATES) - {row['id'] for row in rows}:
        raise ValueError('Save points for ROMs outside the index')
    return sorted(rows, key=lambda row: row['id'])


def cgb_rows(add, moon):
    """The CPU CGB E entries other than mooneye's acceptance tests, which
    moon() adds with their DMG-C entries."""
    for name in MOONEYE_CGB_MISC:
        tags = ['boot', 'io'] if name.startswith('boot') or name.startswith('bits') else ['ppu', 'stat', 'interrupt']
        add('mooneye-test-suite', 'misc/' + name, 'gbc-1-1', ['ppu-basic', 'memory', *tags], mode='compat')
    # Blargg on a CGB, in CGB mode: the howto's durations and references.
    # cpu_instrs switches to double speed on a CGB, and interrupt_time
    # measures at both speeds, so both wait for task gbc-1-2.
    for name, seconds, reference, task, tags in [
            ('cpu_instrs/cpu_instrs', 31, 'cpu_instrs/cpu_instrs-dmg-cgb.png', 'gbc-1-2', ['speed']),
            ('instr_timing/instr_timing', 1, 'instr_timing/instr_timing-dmg-cgb.png', 'gbc-1-1', ['timer']),
            ('mem_timing/mem_timing', 3, 'mem_timing/mem_timing-dmg-cgb.png', 'gbc-1-1', ['timer']),
            ('mem_timing-2/mem_timing', 4, 'mem_timing-2/mem_timing-dmg-cgb.png', 'gbc-1-1', ['timer']),
            ('halt_bug', 2, 'halt_bug-dmg-cgb.png', 'gbc-1-1', ['halt', 'interrupt']),
            ('interrupt_time/interrupt_time', 2, 'interrupt_time/interrupt_time-cgb.png', 'gbc-1-2',
             ['speed', 'interrupt']),
            ('cgb_sound/cgb_sound', 37, 'cgb_sound/cgb_sound-cgb.png', 'gbc-1-1', ['apu', 'div', 'timer'])]:
        add('blargg', name, task, ['ppu', 'frame', 'interrupt', *tags], 'frame', seconds,
            reference=f'roms/blargg/{reference}', mode='cgb')
    add('cgb-acid2', 'cgb-acid2', 'gbc-1-1', ['ppu', 'frame', 'palette', 'vram-bank', 'stat', 'interrupt'],
        'frame', 120, reference='roms/cgb-acid2/cgb-acid2.png', path='roms/cgb-acid2/cgb-acid2.gbc', mode='cgb',
        end='ld-b-b')
    add('dmg-acid2', 'dmg-acid2', 'gbc-1-1', ['ppu', 'frame', 'palette', 'stat', 'interrupt'],
        'frame', 120, reference='roms/dmg-acid2/dmg-acid2-cgb.png', mode='compat', end='ld-b-b')
    add('cgb-acid-hell', 'cgb-acid-hell', 'gbc-1-1', ['ppu', 'frame', 'raster', 'palette', 'vram-bank'],
        'frame', 120, True, 'roms/cgb-acid-hell/cgb-acid-hell.png', path='roms/cgb-acid-hell/cgb-acid-hell.gbc',
        mode='cgb', end='ld-b-b')
    # SameSuite: dma/ and ppu/blocking_bgpi_increase test what Pan Docs
    # documents for every CGB (LCD VRAM DMA Transfers; LCD Color Palettes);
    # their sources name no revision and use no double speed.
    for name, tags in [('dma/gbc_dma_cont', ['hdma']), ('dma/gdma_addr_mask', ['hdma']),
                       ('dma/hdma_lcd_off', ['hdma']), ('dma/hdma_mode0', ['hdma', 'ppu', 'stat', 'interrupt']),
                       ('ppu/blocking_bgpi_increase', ['ppu', 'palette', 'stat', 'interrupt'])]:
        add('same-suite', name, 'gbc-1-1', ['memory', 'vram-bank', *tags], mode='cgb')
    # AGE's speed switch tests for CPU CGB E. Those in caution/ cut short the
    # HALT after the switch, which their author found to unsettle CGB E.
    for name, tags in [('spsw-ch2-lc-delay-cgbBCE', ['apu', 'div']), ('spsw-div-cgbBCE', ['div']),
                       ('spsw-mode0-cgbBCE', ['ppu', 'stat']), ('spsw-stop-prefetch-cgbBCE', ['timer', 'div']),
                       ('spsw-tima-cgbE', ['timer', 'div', 'interrupt'])]:
        add('age-test-roms', 'speed-switch/' + name, 'gbc-1-2', ['speed', *tags], mode='cgb')
    for name in SAMESUITE_APU:
        add('same-suite', 'apu/' + name, 'gbc-1-2' if name in SAMESUITE_DOUBLE_SPEED else 'gbc-1-1',
            ['apu', 'div', 'timer', 'interrupt'], report_only=True, mode='cgb')
    for name, revision in MEALYBUG_CGB.items():
        add('mealybug-tearoom-tests', 'ppu/' + name, 'gbc-1-1', ['ppu', 'frame', 'raster', 'palette'],
            'frame', 120, True, f'roms/mealybug-tearoom-tests/ppu/{name}_cgb_{revision}.png', mode='compat',
            end='ld-b-b')


def header_mode(path):
    """The mode a CGB runs a ROM in, by its CGB flag (Pan Docs, 0143 - CGB flag)."""
    with open(path, 'rb') as rom:
        rom.seek(0x143)
        return 'cgb' if rom.read(1)[0] & 0x80 else 'compat'


def timing_rows(add):
    """Task gbc-1-3: AGE's STAT, LY and mode 3 tests and Gambatte's."""
    age = GAMBATTE.parent / 'age-test-roms'
    if not GAMBATTE.is_dir() or not age.is_dir():
        raise ValueError(f'Test ROMs missing under {GAMBATTE.parent}; run refs/fetch.sh')
    for name, (models, tags) in AGE_TIMING.items():
        for model in models:
            mode = 'dmg' if model == 'DMG-C' else header_mode(age / (name + '.gb'))
            add('age-test-roms', name, 'gbc-1-3', tags, mode=mode)
    for name, models in AGE_OAM.items():
        for model in models:
            mode = 'dmg' if model == 'DMG-C' else header_mode(age / (name + '.gb'))
            add('age-test-roms', name, 'gbc-1-3', ['ppu', 'oam-access'], report_only=True, mode=mode)
    for name, references in AGE_MODE3.items():
        for model, suffix in references.items():
            mode = 'dmg' if model == 'DMG-C' else header_mode(age / (name + '.gb'))
            add('age-test-roms', name, 'gbc-1-3', ['ppu', 'frame', 'raster', 'mode3'], 'frame',
                reference=f'roms/age-test-roms/{name}-{suffix}.png', mode=mode, end='ld-b-b')
    groups = [*((name, ['ppu', 'stat', 'interrupt', *(['halt'] if name == 'halt' else [])], False)
                for name in GAMBATTE_TIMING),
              *((name, ['ppu', 'stat', 'interrupt', 'mode3'], False) for name in GAMBATTE_MODE3),
              *((name, ['ppu', 'oam-access'], True) for name in GAMBATTE_REPORT)]
    for directory, tags, report in groups:
        for path in sorted((GAMBATTE / directory).rglob('*')):
            if path.suffix not in ('.gb', '.gbc'):
                continue
            relative = path.relative_to(GAMBATTE)
            name = relative.with_suffix('').as_posix()
            pngs = gambatte_pngs(path)
            outputs = gambatte_outputs(path.stem)
            for model in MODELS:
                cgb = model == 'CGB E'
                common = dict(report_only=report or cgb or name in GAMBATTE_DMG_REPORT,
                              path=f'roms/gambatte/{relative.as_posix()}',
                              limit_ticks=GAMBATTE_TICKS, mode=header_mode(path) if cgb else 'dmg',
                              in_all=not cgb, end='duration')
                if outputs.get(model):
                    add('gambatte', name, 'gbc-1-3', tags, 'gambatte-digits', frame_rows=GAMBATTE_ROWS,
                        digits=outputs[model], **common)
                elif model in pngs:
                    add('gambatte', name, 'gbc-1-3', tags, 'frame', colors='gambatte',
                        reference=f'roms/gambatte/{pngs[model].relative_to(GAMBATTE).as_posix()}', **common)


def unsupported(row):
    missing = sorted(set(row['requires']) - SUPPORTED_TASKS)
    return f'Executor unsupported; requires task {", ".join(missing)}' if missing else None


def verify_data(data, rows):
    """Verify selected files against the SAME manifest subsequently handshaken.

    Unsupported ROMs are indexed but never opened or sent for execution.
    """
    raw = (data / 'manifest.tsv').read_bytes()
    entries = {}
    for line in raw.decode().splitlines():
        if not line or line.startswith('#') or line.startswith('sha256\t'):
            continue
        digest, size, path = line.split('\t')
        if path in entries:
            raise ValueError(f'Duplicate manifest path: {path}')
        entries[path] = (digest, int(size))
    verified = {}
    for row in rows:
        if unsupported(row):
            continue
        path = row['path']
        if path not in entries:
            raise ValueError(f'ROM missing from manifest: {path}')
        payload = (data / path).read_bytes()
        actual = (hashlib.sha256(payload).hexdigest(), len(payload))
        if actual != entries[path]:
            raise ValueError(f'ROM does not match manifest: {path}')
        if not 32768 <= len(payload) <= 8388608 or payload[0x147] not in (
                0, 1, 2, 3, 5, 6, 8, 9, 0x0f, 0x10, 0x11, 0x12, 0x13,
                0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e):
            raise ValueError(f'ROM requires an unsupported cartridge: {path}')
        if row['mode'] != 'dmg':
            header = 'cgb' if payload[0x143] & 0x80 else 'compat'
            if row['mode'] != header:
                raise ValueError(f'A CGB runs {path} in {header} mode, not {row["mode"]}')
        if row['reference']:
            reference = row['reference']
            if reference not in entries:
                raise ValueError(f'Reference missing from manifest: {reference}')
            image = (data / reference).read_bytes()
            if (hashlib.sha256(image).hexdigest(), len(image)) != entries[reference]:
                raise ValueError(f'Reference does not match manifest: {reference}')
            from frame_compare import reference_any, reference_colors, reference_shades
            try:
                (reference_any if row['colors'] == 'gambatte' else
                 reference_shades if row['mode'] == 'dmg' else reference_colors)(data / reference)
            except ValueError as error:
                if not row['report_only']:
                    raise
                row['reference_error'] = str(error)
        verified[row['id']] = actual
    return hashlib.sha256(raw).hexdigest(), verified

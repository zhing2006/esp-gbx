# 任务 1-3：STAT 中断和 mode 3 的时序

这是[路线图](../../roadmap.md#gbc) GBC 段里程碑 1 的第三个任务。它按 Gambatte、AGE 和 Mealybug 的用例修正 PPU 的几处时序：STAT 中断、LY 和 LY=LYC 比较、模式切换的时刻，重点是第 153、0、1 行附近和 LCD 打开以后的第一帧；mode 3 因 OBJ 和窗口而延长的长度；mode 3 中途写 LCDC、SCX 和 BGP 时从哪一个 dot 开始生效。1-2 修 Shantae 时发现，第 0 行多出一次 LYC=0 的中断就会让游戏整帧不画精灵，而现有的必过用例没有发现这个问题。解释器和 JIT 同时改，GB 模式和 CGB 模式都改。这些改动只发生在每行一次的计算和有中途修改的行上，预计不影响普通的行，实际开销按下面的完成标准实测。做完这个任务，里程碑 1 就完成了。这个任务依赖 1-2。

开工时在[任务状态](status.md)里把这个任务标为“进行中”。

## 先读

- 路线图的 GBC 段，包括“未尽之事”，任务 1-2 的“Shantae 的人物和 bank 切换”一节。
- `refs/docs/pandocs/src/` 中的 `STAT.md`、`Interrupt_Sources.md`、`Interrupts.md`、`Rendering.md` 和 `LCDC.md`。
- AGE、Gambatte 和 Mealybug 的 `game-boy-test-roms-howto.md`，Mealybug 的 `README.md`。
- [真机测试计划](../../hardware-testing.md)的“选择本次运行的测试”“每个任务的双后端优化”“运行整机 ROM”和“通过判据”。
- [架构](../../architecture.md)的“时间线和事件”“SM83 JIT”（尤其是推迟派发）“PPU 渲染”和“即时存档的状态”。
- `gb/ppu.cpp` 里各个时刻的依据注释。

## 范围

Gambatte 的判据：

- 按 Gambatte 的 howto，用例运行 15 帧以后，把画面和 `testrunner.cpp` 里的单色数字图案比较。runner 加上这种判定方式。`testrunner.cpp` 按 `refs/README.md` 的规则加入参考资料，固定到 howto 引用的提交 `d819bad196`，两个 fetch 脚本同步。只使用它的数字图案和运行条件，不参考 libgambatte 的实现。`.gb` 文件在 CGB 上按什么模式运行，也按 `testrunner.cpp` 确定。
- 机型。dmg08 的期望值用于 DMG-C。howto 说 dmg08 的 CPU 版本没有确定，所以它和 mooneye、AGE 的 DMG-C 用例冲突时以后两者为准，冲突的用例列出来和用户确认。cgb04c 的期望值来自 CPU CGB C，而 AGE 的 `ly`、`lcd-align-ly`、`oam` 和 `m3-bg-bgp` 用例分出了 CGB C 和 E 的不同结果，正好是这个任务涉及的时序，所以 Gambatte 在 CGB E 上只记入报告，倍速的 `_ds_` 用例也一样。

STAT 中断和 LY：

- Gambatte 选这些目录：`enable_display`、`halt`、`irq_precedence`、`lcdirq_precedence`、`ly0`、`lyc0int_m0irq`、`lyc153int_m2irq`、`lycEnable`、`lycint_*`、`lycm2int`、`lywrite`、`m0enable`、`m0int_*`、`m1`、`m2enable`、`m2int_*` 和 `miscmstatirq`。按文件名统计，DMG 上约 700 个用例，CGB 上约 1,060 个。`audio` 结果的用例不选。
- AGE 加入测试索引，作为必过：`stat-interrupt/stat-int-dmgC-cgbBCE` 和 `halt/halt-m0-interrupt-dmgC-cgbBCE`，两种机型都运行；`stat-interrupt/stat-int-ncmBCE` 在 CGB E 的兼容模式下运行；`ly/ly-dmgC-cgbBC` 在 DMG-C 上运行；`ly/ly-cgbE` 和 `lcd-align-ly/lcd-align-ly-cgbE` 在 CGB E 上运行，`ly/ly-ncmE` 在 CGB E 的兼容模式下运行；`halt/ei-halt-dmgC-cgbBCE` 和 `halt/halt-prefetch-dmgC-cgbBCE`，两种机型都运行。
- 只记入报告：AGE `oam` 下适用于 DMG-C 和 CGB E 的用例，以及 Gambatte 的 `oam_access`。OAM 能不能访问跟着模式的时刻变，这次改动可能影响它们，但是它们不在这个任务的完成条件里。
- PPU 要改的：第 153、0、1 行附近 LY 的读出值、LY=LYC 标志和 STAT 中断线的时刻，mode 0 和 mode 2 中断沿的时刻，写 STAT、LYC 时中断线的变化，以及 LCD 打开以后第一帧的模式和中断。CGB E 第 153 行 LY 的保持时间和读 LY 时的毛刺，按 AGE `ly-cgbE` 和 `lcd-align-ly-cgbE` 确定。倍速下的时刻按同样的规则换算。

mode 3 的长度：

- CPU 所在的核按这一行的 SCX、窗口和 OBJ 算出 mode 3 的长度，每行算一次。OBJ 和窗口带来的延长按 Gambatte 的 `sprites` 和 `window` 修正，这两个目录按文件名统计在 DMG 上约 280 个用例，CGB 上约 635 个，机型的规则同上。mode 0 中断和 HBlank DMA 的时刻都跟着它变。

mode 3 中途的修改：

- 渲染核在一行中途有修改时，按修改发生的 dot 分段采样，这个做法不变。要改的是各个寄存器的修改从哪一个 dot 开始生效。1-2 结束时 Mealybug 的 CGB 用例里，失败的大多每行只差 1 到 5 个像素。
- AGE 的 `m3-bg-bgp`、`m3-bg-lcdc` 和 `m3-bg-scx` 有 CGB E 和 DMG-C 的参考图，加入测试索引作为必过，按 AGE howto 的颜色换算比较画面，包括倍速的 `-ds` 和兼容模式的 `-nocgb`、`ncmE`。
- Mealybug 的 CGB 用例仍然只记入报告，因为参考图只有 CGB C 和 D。其中 C 和 D 参考图相同的 10 个失败用例作为修正的目标，失败的原因写进报告：`m3_lcdc_bg_en_change`、`m3_lcdc_bg_map_change`、`m3_lcdc_obj_en_change`、`m3_lcdc_obj_size_change`、`m3_lcdc_obj_size_change_scx`、`m3_lcdc_tile_sel_change`、`m3_lcdc_tile_sel_win_change`、`m3_lcdc_win_map_change`、`m3_scx_high_5_bits` 和 `m3_scx_low_3_bits`。BGP、OBP0 和 SCY 的用例在 C 和 D 上结果不同，CGB E 上以 AGE 的 `m3-bg-bgp-ncmE` 为准。

现状和数据：

- 在 PC 测试程序上看过一次 AGE 的现状：`stat-int`、`halt-m0-interrupt`、`ly-cgbE`、`ly-ncmE`、`lcd-align-ly-cgbE`、`oam-read` 和 `oam-write` 不通过，`ly-dmgC-cgbBC`、`ei-halt` 和 `halt-prefetch` 通过。`m3-bg-*` 没有测过。开工时在板上按索引重新建立基线。
- 卡上的测试数据由 `tools/prepare_gb_tests.py` 选择，增减用例时同步修改脚本和测试索引。
- 各个时刻的依据写在 `gb/ppu.cpp` 和渲染代码的注释里，写明是哪个用例在哪种机型上确定的。

JIT：

- 推迟派发计算 LY 下一次等于 LYC 的时刻、STAT 中断请求的时刻和 HBlank 的时刻，都要和 PPU 的修改一致。两个后端的结果相同。

即时存档：

- PPU 状态的字段改变时，按 GB 段的规则增加状态格式的版本号。

## 不做

- 逐 dot 运行 PPU 或像素 FIFO。如果某个用例只有逐 dot 运行才能通过，列出来，不在这个任务里做。
- cgb-acid-hell，以及只有 CGB C 参考图、或者 C 和 D 结果不同的 Mealybug 用例的修正。它们的结果照常记入报告。
- OAM DMA、HDMA 的传输、定时器、串口和声音的时序。
- 把 Gambatte 和 Mealybug 的 CGB 结果作为 CGB E 的完成条件。

## 约束

- 已经通过的必过用例不能回退，GB 模式和 CGB 模式都一样。
- 设备仍然在被访问或者事件到期时才追赶到当前时刻，不逐周期推进。每条指令、每次访存的路径上不为本任务增加检查。没有中途修改的行仍然一次画完。
- 改动解释器、设备执行代码或 JIT 运行时 helper 时，按 `cmake/core_layout.cmake` 的规则放置，检查 map 中的地址。

## 由你决定

PPU 事件的划分和时刻的表示、mode 3 长度的计算方式、各寄存器生效时刻的表示、Gambatte 的判定放在板上还是电脑上、新用例在测试索引里怎么分组，以及固定回归加哪几个用例。理由写在代码注释和真机测试计划里。

## 完成标准

- 开发过程中按[真机测试计划](../../hardware-testing.md#选择本次运行的测试)选择受影响模块的 ROM，开发完就运行。
- 在 JC1060P470 上，两个后端都通过上面列出的必过 AGE 用例，以及选中目录里 Gambatte 在 DMG-C 上的用例，结果一致。做不到的，写明原因，和用户确认以后改为只记入报告。只记入报告的结果写进报告，Mealybug 那 10 个目标用例逐个写明结果。
- 任务结束时做一次两种机型的全量回归，包括新加的用例。两个后端都通过，结果一致，回退清单为空或者已经和用户确认。状态格式改了的话，`--states` 也要通过。
- 固定回归加入这类用例以后不超过 1 分钟。新用例很多，全部放进全量回归可能超过路线图的预算，这一点没有实测；超出时先和用户确认分组方式。
- 按[双后端优化要求](../../hardware-testing.md#每个任务的双后端优化)，用 1-2 “GB 模式的速度”一节的 5 个 GB ROM 和几个 CGB E 的 ROM，比较改动前后两个后端的速度，也比较渲染核上 CGB 渲染的耗时。变慢的列入回退清单。
- 用户在 JC1060P470 上复测 Shantae 出门以后的画面，以及 1-2 试玩过的几款游戏，画面和声音正常。
- 开发过程中只构建要刷进板子的固件和 PC 的 debug 版。用户确认可以提交时，全部板子的正式固件和测试固件构建一次，没有新的警告。

## 收尾

- 满足全部完成标准以后，在[任务状态](status.md)里把这个任务标为“已完成”。
- 在架构文档中更新 PPU 的时刻、mode 3 长度的计算、中途修改的生效时刻和 JIT 推迟派发的计算，在真机测试计划中写明 Gambatte 的判据、机型的适用范围、用例的分组和固定回归，在 `refs/README.md` 登记 `testrunner.cpp`。路线图“未尽之事”里这个任务做完的部分删掉，没做完的写明现状。
- 向用户汇报测试结果、耗时、速度、只记入报告的用例结果和回退清单。

## 实现与验证

都在 JC1060P470 上测量，芯片 ESP32-P4 v1.0，主频 360 MHz。改动前的固件在开工时的代码上只加了测试协议第 10 版，模拟器的代码没有改。测试 ROM 的速度取测试固件 `jc1060p470-test-release` 报告里的倍数，算法同 1-1。Shantae 的数字来自 `jc1060p470-profile` 固件回放 `recordings/shantae-intro.keys`，这个固件带 JIT 计数和采样的开销。报告都在 `build/test-reports/` 下。

### 时序

PPU 的各个时刻改成从 LY 改变的那一刻算起，以 tick 为单位，LCD 打开时第 0 行从写 LCDC 的访存时刻开始。一行开头的 LY=LYC 比较、mode 2 和 OAM 中断源的变化按 `kEdges` 表推进，OAM 中断源在下一行之前升起。原速下 HBlank 中断源和第 144 行的 mode 1 中断源升起以后有 6 tick 的结算时间，CPU 可以立刻响应，但是读 IF 要等结算时间过去才看得到，这段时间里写 STAT、LYC 和 IF 的效果也按 Gambatte 的用例处理。中断派发时，PC 高字节入栈以后才取 IE，低字节入栈的访存时刻才取 IF。mode 3 的长度在开始时按公式算一次，mode 3 中途写 LCDC、WX 或者 WY 在中途匹配时，再用渲染线程的同一份时间线修正。DMG 上 WX 为 166 的窗口延续到下一行。设计写在架构文档的“时间线和事件”和“PPU 渲染”，各个时刻的依据写在 `gb/ppu.cpp` 和 `gb/renderer.h`。即时存档的格式升到第 5 版，加上了这些新的状态。

### 用例结果

最后一次全量回归是 `gbc-1-3-opt-all`，在性能优化之后、评审修正之前。两个后端的结果完全一致，和 PC 测试程序逐个相同。评审修正以后只重跑了受影响的用例，见“测试”。

- AGE 的必过用例全部通过，DMG-C 8 个，CGB E 15 个（不算 1-2 加的 5 个倍速用例）。改动前这 23 个只通过 6 个。
- Gambatte 在 DMG-C 上的 1006 个用例通过 956 个，改动前是 663 个。没有通过的 50 个改为只记入报告，见下一节。
- Gambatte 在 CGB E 上只记入报告，不在全量回归里。性能优化之前单独运行的 `gbc-1-3-final-gambatte-cgbe` 通过 1534/1762 个，耗时 526.6 秒，两个后端一致，和 PC 测试程序逐个相同。之后没有在板上重跑这一组；PC 测试程序上，优化和评审修正以后的结果和指纹都和之前逐个相同。改动前没有在板上跑过这一组。
- 只记入报告的用例：Gambatte `oam_access` 24 个全部通过，改动前 16 个；AGE `oam` 下的 6 个都不通过，和改动前一样；Mealybug 在 DMG-C 上 24 个全部通过，在 CGB E 上 27 个通过 6 个，改动前 5 个；SameSuite APU 61 个通过 17 个，和改动前一样；cgb-acid-hell 不通过，blargg `oam_bug` 因为参考图不符合要求而无法判定。

### DMG-C 上没有通过的 Gambatte 用例

这 50 个用例在两个后端上都不通过，改为只记入报告，名单是 `rom_suite.py` 的 `GAMBATTE_DMG_REPORT`。其中 13 个改动前是通过的，列在回退清单里，下面用“（回退）”标出。

- 和 AGE、mooneye 冲突的 9 个。`halt/late_m0int_halt_m0stat_scx2_3a`、`scx3_2b`、`scx3_3a`（回退）、`scx3_3b`（回退）和 `late_m0irq_halt_m0stat_scx3_2b`（回退）要求 HBlank 中断源再早 1 tick 升起，这样改以后 AGE 的 `stat-int`、`halt-m0-interrupt` 和 mooneye 的 `intr_2_*` 不通过。`m1/m1irq_m2enable_lyc_1`、`_2`、`m1irq_m2disable_lycdisable_3` 和 `lycint143_m1irq_late_retrigger_2` 要求 OAM 和 mode 1 中断源也有结算时间，这样改以后 mooneye 的 `stat_irq_blocking` 不通过。
- 和写 STAT、LYC 的时刻有关的 8 个：`lycEnable` 下的 `early_ff45_response_1`、`ff41_disable_2`（回退）、`ff45_enable_weirdpoint_3`、`lcdoff_lycirqen_1`、`lcdoff_lycirqen_4`、`lyc153_late_ff45_enable_3`，`m2enable/late_enable_m0disable_2` 和 `miscmstatirq/lycstatwirq_trigger_m0_late_ly44_lyc44_08_40_4`。试过让 DMG 写 STAT 和 LYC 提前 4 到 6 tick 生效，`m0enable`、`lycdisable` 一类的用例能通过，但是 `m2enable`、`miscmstatirq` 和 `halt` 里同样多的用例会失败，所以没有采用。这 8 个没有逐个分析。
- 窗口的 13 个：`window/on_screen/wxA6_late_we_reenable_1` 到 `_4` 在 WX 为 166、窗口延续的那一行前后开关窗口，窗口的行计数不对；`window/m2int_wxA6_spxA7_m0irq_2` 和 `m2int_wxA6_spxA7_m3stat_2` 里 X 为 167 的 OBJ 在 dmg08 上只让 mode 3 长 2 到 3 个 dot，和现在的公式不同；`window/late_disable_early_scx03_wx11_2`（回退）、`late_disable_late_scx03_wx11_2`、`late_disable_scx5_1`、`late_disable_spx10_wx0f_2`（回退）、`window/late_enable_afterVblank_3`（回退）、`window/arg/late_enable_afterVblank_5`（回退）和 `window/arg/late_scx_late_wy_FFto4_ly4_wx00_2` 在 mode 3 中途关闭或打开窗口，原因没有查明。
- 没有逐个分析的 20 个：`enable_display` 下的 `enable_display_ly0_sprites_m0stat_2`、`frame0_m2irq_count_2`、`frame1_m2irq_count_2`、`frame1_m2stat_count_1`、`ly0_late_scx7_m3stat_scx0_2`（回退）、`ly0_late_scx7_m3stat_scx1_1`（回退）、`ly0_late_scx7_m3stat_scx3_1`（回退）和 `ly0_m0irq_scx1_1`；`m1` 下的 `lyc143_late_m2enable_lycdisable_2`、`lycint_vblankintirq_2`、`lycint_vblankirq_late_retrigger_2` 和 `m1irq_m0disable_2`（回退）；`ly0` 下的 `lycint152_ly0stat_2`、`lycint152_lyc0irq_late_retrigger_2` 和 `lycint152_lyc153irq_late_retrigger_2`；`lyc153int_m2irq/lyc153int_m2irq_late_retrigger_2`；`irq_precedence/late_m0irq_vs_tima_scx2_1` 和 `late_m0irq_vs_tima_scx2_halt_1`；`lcdirq_precedence/lcdirqprecedence_lycirq_ly90_lcdstat68`（回退）；`sprites/10spritesPrLine_10xposA7_m0irq_2`。

### Mealybug 的 10 个目标用例

`m3_lcdc_bg_en_change`、`m3_lcdc_bg_map_change`、`m3_lcdc_obj_en_change`、`m3_lcdc_obj_size_change`、`m3_lcdc_obj_size_change_scx`、`m3_lcdc_tile_sel_change`、`m3_lcdc_tile_sel_win_change`、`m3_lcdc_win_map_change`、`m3_scx_high_5_bits` 和 `m3_scx_low_3_bits` 在 CGB E 上都不通过，两个后端一致；它们在 DMG-C 上全部通过。除了 `m3_scx_low_3_bits`，其余 9 个和参考图不一致的位置，正好是 CGB C/D 参考图和 DMG 参考图不同的地方，也就是说我们在 CGB E 上画出的和 DMG 一样。LCDC 的 BG 位在 CGB E 上的生效时刻有 AGE `m3-bg-lcdc` 对照，它在两个后端上都通过；OBJ 和窗口相关的几个没有 CGB E 的用例可以对照，所以没有按 CGB C/D 的参考图改。`m3_scx_low_3_bits` 在兼容模式下画面右上角的图标是白色的，原因没有查明。

### 速度

测试 ROM 的倍数。“前”是改动前的 `gbc-1-3-before-all`，“优化前”是 1-3 第一次提交时的 `speed-opt6`，“评审后”是性能优化和评审修正以后的 `gbc-1-3-review-speed`，括号里是“评审后”和“前”相比。解释器计数优化后重测的结果见“解释器计数的优化”。

| ROM | 解释器 前 / 优化前 / 评审后 | JIT 前 / 优化前 / 评审后 |
|---|---|---|
| blargg `cpu_instrs` | 4.248 / 4.079 / 4.230（−0.4%） | 16.687 / 14.854 / 17.166（+2.9%） |
| blargg `dmg_sound` | 4.184 / 4.032 / 4.184（0.0%） | 14.174 / 12.854 / 14.451（+2.0%） |
| blargg `mem_timing-2` | 4.387 / 4.234 / 4.399（+0.3%） | 14.824 / 13.458 / 15.014（+1.3%） |
| mooneye `intr_2_mode0_timing_sprites` | 3.599 / 3.425 / 3.519（−2.2%） | 12.427 / 11.002 / 11.673（−6.1%） |
| dmg-acid2 | 6.415 / 5.956 / 6.298（−1.8%） | 10.088 / 9.366 / 9.809（−2.8%） |
| CGB E blargg `cpu_instrs` | 2.548 / 2.417 / 2.544（−0.2%） | 10.235 / 9.380 / 10.405（+1.7%） |
| CGB E cgb-acid2 | 4.931 / 4.813 / 4.883（−1.0%） | 8.248 / 7.716 / 7.876（−4.5%） |
| CGB E blargg `cgb_sound` | 4.123 / 3.952 / 4.155（+0.8%） | 12.713 / 11.626 / 12.517（−1.5%） |
| CGB E SameSuite `hdma_mode0` | 3.952 / 3.575 / 3.789（−4.1%） | 5.943 / 5.388 / 5.623（−5.4%） |
| rtc3test `basic-tests` | 11.125 / 10.629 / 11.038（−0.8%） | 13.563 / 13.026 / 13.416（−1.1%） |

同一份代码重测，多数 ROM 的变化在 ±0.5% 以内。评审修正前后，各项的变化在 −1.6% 到 +2.0% 之间。

Shantae 片头在 1X 下回放，单位是毫秒。“运行”是模拟一帧的耗时，“帧”是从开始模拟一帧到画面交给显示、声音混音完成的墙钟耗时，同调试设计里的 `FRAME`。渲染核那一列是 `tasks 5000` 里 `gbx-render` 任务占一个核的比例：

| 后端 | 运行 平均 / p95 | 帧 平均 / p95 | 渲染核 |
|---|---|---|---|
| JIT 前 | 5.078 / 6.604 | 6.886 / 8.439 | 15.1% |
| JIT 后 | 5.407 / 7.058 | 7.164 / 8.653 | 15.6% |
| 解释器 前 | 10.089 / 10.931 | 11.917 / 13.416 | 16.0% |
| 解释器 后 | 10.334 / 11.637 | 12.123 / 13.622 | 15.8% |

两段回放都没有欠载。改动后两个后端的 hash 都是 `e88ee7e93a889104`，和改动前的 `2798e425d5567f82` 不同，因为模拟的时序变了。

性能优化做到一半时（PPU 排到最后之前），又按同样的回放比较过一次。同一轮里，改动前的固件是 JIT 运行 5.183 ms、帧 7.083 ms，解释器运行 10.159 ms、帧 11.995 ms（都是平均值）；优化中的版本是 JIT 5.165 ms、6.944 ms，解释器 10.650 ms、12.630 ms。JIT 和改动前持平，解释器仍然慢约 5%。

性能优化和评审修正以后，`gbc-1-3-review-summary` 里同样的回放：JIT 运行平均 5.178 ms、p95 6.858 ms，帧平均 6.988 ms、p95 8.537 ms；解释器运行平均 10.630 ms、p95 12.115 ms，帧平均 12.615 ms、p95 14.169 ms。两段都没有欠载，hash 都是 `e88ee7e93a889104`。和同一方法下改动前的固件相比，JIT 持平，解释器慢约 5%。`tasks 5000` 里 `gbx-render` 的比例，这一轮是 JIT 26.1%、解释器 28.3%，改动前的固件在同一方法下是 27.0% 和 26.6%；它和上表的约 15% 差得多，原因没有查明，所以只在同一轮里比较。

### 性能优化

1-3 的第一版变慢，来自 PPU 每一行的开销。改动前可见行有 5 个阶段：新一行、OAM 中断沿、mode 2、mode 3 开始和 mode 0。现在 DMG 有 6 个，CGB 有 7 个，多出来的是一行开头单独的 LY=LYC 比较、下一行之前 OAM 中断源的升起，CGB 还有 OAM 中断源的落下；打开 mode 0 中断时还有一个结算阶段。每个阶段的工作也多了，比如结算时间的记录、窗口延续和 mode 3 的计时字段。JIT 下 CPU 指令跑得快，PPU 的开销占的比例大，所以 JIT 慢得更多。

第一版提交之前已经做了几项：`arm()` 一趟算出下一个要安排成事件的阶段；STAT 中断源全关时 `update_stat()` 直接让中断线保持低电平；VBlank 的几个阶段从 `advance()` 拆到 `advance_vblank()`；只在写 WY、mode 3 中途的部分写入和结算时间里的写入时才运行的函数，按 `tools/link_core_layout.py` 的名单排到 CGB 函数后面。第一版的 JIT 在 `cpu_instrs` 上原本慢 22.5%，做完这几项以后是上表“优化前”那一列。

之后又做了这些：

- 事件到来时，可见行上没有安排成事件的阶段不再逐个推进。这些阶段都不会升起 STAT 中断线，事件本身又会重新计算中断线，所以 mode 3 开始的事件只补 LY=LYC 比较，新一行开始的事件只补 mode 0 和 OAM 中断源的状态（`Ppu::skip()`）。这是原计划的第 3 项。
- 几处小改动：`arm()` 按阶段分支；`transfer_length()` 内联；mode 3 开始时不再复制选中的 OBJ，LCD 寄存器整块复制进渲染任务；CGB 读 LY 不再让 PPU 追赶，因为只有作为事件运行的阶段才会改变 LY；新一行的 LY=LYC 比较时刻用常量；mode 3 的结束时刻按 32 位偏移计算。
- PPU 和渲染器排到片内代码的最后（`tools/link_core_layout.py` 的 `LAST`），改 PPU 不再移动解释器、存储器、JIT 运行时和双速解释器。同一份代码换到这个排布，测速 ROM 在 GB 上的变化在 ±0.9% 以内，CGB 上有三项低 1.0% 到 1.7%。没有加填充，核心区反而短了 18 字节。

原计划的第 1 项是访问时按时间算出 mode、LY=LYC 标志和 OAM 中断源，这一项没有做。PC 上数过，`intr_2_mode0_timing_sprites` 平均每行只有 0.03 个阶段是在访问时补做的，开销在事件里，不在访问里。第 2 项只做了 mode 3 结束时刻这一处：对照反汇编，其余每行路径上的 64 位运算，GCC 已经只比 32 位多两条指令。

试过、没有采用的两项：

- 把访问时和事件时补做阶段的代码合成一套，每个阶段留下来的改动只写一次，访问和事件都调用它。PC 上结果和指纹都不变，但是 `cpu_instrs` 的 JIT 慢了 8.1%，解释器慢了约 2%。把 PPU 排到最后以后再测，退步不变，所以不是排布的原因。在测试固件里临时读出 L1 I-cache 的缺失计数器（`CACHE_L1_IBUS1_ACS_MISS_CNT_REG`），每行多出 160 到 200 个周期，缺失数却基本不变；PC 上数过，事件数也一样。采样分析器显示多出的时间都在新拆出来的几个函数里，平均分布在各条指令上。通用写法要把每个阶段的改动都做一遍，其中一部分马上又被 mode 3 或下一行覆盖，还要做 64 位的时刻比较、保存更多寄存器。强制内联并改用 32 位偏移以后，仍然慢 4.4%，所以保留了事件和访问各自的写法。
- 把只在 VBlank 行运行的 `advance_vblank()` 和 `begin_line()` 移到冷区：两个后端都慢了 1% 到 3%。当时 PPU 还排在 JIT 运行时前面，它后面的代码都跟着移动了。

和改动前相比仍然慢的，见上表和回退清单。`intr_2_mode0_timing_sprites` 打开了 mode 0 中断，每行多一个结算事件，这是 1-3 的时序本身要求的。两个 acid2 和 `hdma_mode0` 只模拟 0.13 到 0.4 秒，固定开销占的比例大，没有进一步分析。

### 评审修正

代码评审发现 1-3 的四处问题，都已经修正：

- JIT 直接读 IO 寄存器时，CGB 的 LY 在倍速下可能在改变前 2 tick 被直接读出旧值，而解释器按 `ly_cgb()` 返回新旧两个值相与。现在 CGB 上直接读 LY 要再留一个 M-cycle 的余量。
- 调色板 RAM 的读写只看 `mode_ == 3`，在 HBlank 中断源升起以后、mode 3 结束之前的几 tick 里被放行。现在和 STAT、VRAM 一样按 `in_transfer()` 判断。
- mode 3 中途写 WX，或者写 WY 让窗口中途匹配，都会移动 HBlank，但是 JIT 缓存的中断期限没有失效。现在写 WY、WX 也重新计算期限。
- mode 3 中途的 LCDC、WX 写入最多只记 8 次，超出的写入不再重新计时，而渲染线程仍然按全部写入画这一行。现在只记改变了值的写入，容量按 CPU 在一行里 mode 3 开始以后最多能写的次数定为 94 次。即时存档的格式因此升到第 5 版，旧版本的存档不再能读。

这些修正在 PC 测试程序上没有改变任何用例的结果和指纹。修正 PC 的警告时，CGB 渲染函数 `raster<true>` 的代码有变化（原来给不可达的代码留了栈空间），GB 和兼容模式用的 `raster<false>` 除了调用偏移完全相同。

### 解释器计数的优化

`run_block()` 原来每执行一条指令，就把指令数、M-cycle 数和最后一个 opcode 写进返回结构体。反汇编确认这些写入没有被编译器消掉。改成用局部变量累计，退出时再组装返回值以后，每条指令少了 3 次写入和 1 次读取。CPU 状态、访存顺序、事件和中断边界都没有改，JIT 的源码也没有改；JIT 回退到解释器时仍然使用这套执行循环。

用 ESP-IDF v6.1 的 GCC 15.2 重新构建 `jc1060p470-release`，原速和倍速循环各从 8,968 字节缩到 8,874 字节，静态指令数各从 2,734 条减到 2,710 条。`block0` 仍然各为 3,202 字节。循环的栈帧从 112 字节增加到 128 字节，累计值留在寄存器里。原速解释器缩短以后，后面的热代码也移动了，这也可能影响 JIT 的速度。

以下仍在 JC1060P470、ESP32-P4 v1.0、360 MHz 上测量。改动前的数字取自上面的评审后记录，本轮没有重刷改动前的固件，不是同一轮的前后对照。报告和反汇编在 `build/run-block-scalars/` 下。

Shantae 用 `jc1060p470-profile` 回放同一份 `recordings/shantae-intro.keys`，解释器 1X，COLOR FIX 打开，SCALING 为 SMOOTH。共 2,615 帧，从第 936 帧计时，计时段 1,679 帧，只回放了一次：

| 耗时，毫秒 | 评审后 平均 / p95 | 计数优化后 平均 / p95 |
|---|---|---|
| 运行 | 10.630 / 12.115 | 9.882 / 11.087 |
| 帧 | 12.615 / 14.169 | 11.765 / 13.285 |

运行平均耗时比评审后的记录低 7.0%，帧平均耗时低 6.7%。回放速度为 100.0%，没有欠载，hash 仍然是 `e88ee7e93a889104`。本轮没有重测 Shantae 的 JIT 或渲染核占用。

测试 ROM 用 `jc1060p470-test-release`。下表的“前”取自 `gbc-1-3-review-speed`，“后”取自 `run-block-scalars/affected-roms`；两个 acid2 用单独复跑的 `run-block-scalars/acid2-recheck`：

| ROM | 解释器 前 / 后 | JIT 前 / 后 |
|---|---|---|
| blargg `cpu_instrs` | 4.230 / 4.252（+0.5%） | 17.166 / 17.164（0.0%） |
| mooneye `intr_2_mode0_timing_sprites` | 3.519 / 3.519（0.0%） | 11.673 / 11.714（+0.4%） |
| dmg-acid2 | 6.298 / 6.186（−1.8%） | 9.809 / 9.786（−0.2%） |
| CGB E blargg `cpu_instrs` | 2.544 / 2.628（+3.3%） | 10.405 / 10.438（+0.3%） |
| CGB E cgb-acid2 | 4.883 / 4.814（−1.4%） | 7.876 / 7.877（0.0%） |
| CGB E SameSuite `hdma_mode0` | 3.789 / 3.802（+0.3%） | 5.623 / 5.664（+0.7%） |

第一次测两个 acid2 时，解释器分别是 6.080 倍和 4.813 倍；复跑以后仍然低于评审后的记录，不能把这项优化当作所有负载都提速。其他原有的测速 ROM 本轮没有重测。

`run-block-scalars/mapped-checks-uart` 的 39 项检查全部通过，覆盖事件发生在访问前后、执行期限、映射和代码改写、中断、HALT、STOP、定时器及 DMA。`run-block-scalars/affected-roms` 的 24 个 ROM 全部通过，包括固定回归的 22 个和 DMG-C、CGB E 上的两个 `cpu_instrs`，耗时 46.1 秒，两个后端的结果一致。两个 acid2 单独复跑也都通过，结果一致，耗时 7.1 秒。

计数优化后，四块板子的正式固件和测试固件各构建一次，JC1060P470 的 profile 固件也构建通过，都没有编译警告。构建日志在 `build/run-block-scalars/build-checks/` 下，profile 固件的日志在其构建目录下。

### 测试

- 全量回归 `gbc-1-3-opt-all`（评审修正之前）两种机型共 1355 个 ROM，耗时 488.7 秒。预算原来是 480 秒，改动前 `gbc-1-3-before-all` 的 484.4 秒和 1-3 第一次提交时的 503.1 秒都超出了；用户把预算改成了 500 秒，这次在预算以内。必过的 1161 个全部通过，DMG-C 1069 个，CGB E 92 个；只记入报告的 194 个通过 71 个。
- 评审修正以后，只重跑了受影响的用例，两个后端的结果都一致：两种机型上 AGE、Mealybug、SameSuite 和两个 acid2 的用例 `gbc-1-3-review-suites`，必过的 36 个全部通过，只记入报告的 118 个通过 47 个，和之前相同，耗时 85.3 秒；DMG-C 上和 mode 3 有关的用例 `gbc-1-3-review-mode3`，必过的 288 个全部通过，只记入报告的 14 个是 `GAMBATTE_DMG_REPORT` 里 `window` 和 `sprites` 下的，仍然不通过，耗时 77.8 秒。
- 固定回归 `gbc-1-3-review-regression` 22 个全部通过，耗时 8.1 秒。
- 即时存档 `gbc-1-3-review-states`（第 5 版）通过，必过 12 个，只记入报告 1 个，耗时 42.4 秒。
- 四块板子 JC1060P470、M5Stack Tab5、Sila P4 和 Sila P4X AIBox 的正式固件和测试固件各全新构建一次，都没有警告。
- 用户在 JC1060P470 上用 release 固件复测过。
- PC：`host/` 的两个测试通过；debug 版全量构建没有警告，修正了 MSVC 报的变量遮蔽、不可达代码和类型转换，`gnu::` 属性只对固件起作用，在 MSVC 上关掉 C5030。

回退清单：

- 速度：PPU 时序改动的回退见“速度”的前表，原因见“性能优化”。解释器计数优化后重测的 6 项见“解释器计数的优化”，其中两个 acid2 的解释器仍然比评审后的记录慢 1.8% 和 1.4%；其他原有测速 ROM 本轮没有重测。Shantae 解释器的最新单次回放是 9.882 ms，低于评审后的 10.630 ms，前值本轮没有重测。
- DMG-C 上 13 个改动前通过的 Gambatte 用例，现在只记入报告，见“DMG-C 上没有通过的 Gambatte 用例”里标“（回退）”的。

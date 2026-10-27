# 任务 1-1：CGB 整机

这是[路线图](../../roadmap.md#gbc) GBC 段里程碑 1 的第一个任务：在 GB 整机上补齐 CPU CGB E 的设备，让 CGB 模式和 DMG 兼容模式的测试 ROM 在测试固件里运行。每项设备功能都同时在解释器和 JIT 上实现，GB 模式的行为和测试结果不能回退。KEY1 倍速、从菜单启动 GBC 游戏、新的设置项、CGB 的即时存档和 2X 都在 1-2。

开工时在[任务状态](status.md)里把这个任务标为“进行中”。

## 先读

- 路线图的 GBC 段，[产品设计](../../product-design.md)的“GBC”和“双核分工”。
- `refs/docs/pandocs/src/` 中的 `CGB_Registers.md`、`Palettes.md`、`Power_Up_Sequence.md`、`Tile_Maps.md`、`OAM.md`、`LCDC.md`、`Accessing_VRAM_and_OAM.md` 和 `Audio_details.md`。
- cgb-acid2、blargg、mooneye、SameSuite 和 Mealybug 的 `game-boy-test-roms-howto.md`。
- [真机测试计划](../../hardware-testing.md)的“选择本次运行的测试”“每个任务的双后端优化”“运行整机 ROM”和“通过判据”。
- [架构](../../architecture.md)的“模拟器”全文，尤其是时间线和事件、内存映射和卡带、SM83 JIT、双核分工和 PPU 渲染。
- GB 段 [2-1](../gb/task-2-1.md) 中回退、测试时间和回传大小的规则。

## 范围

机型和内存：

- 系统按调用者给出的机型和运行模式启动：GB 模式（DMG-C）、CGB 模式或 DMG 兼容模式。CGB 标志第 7 位置位的 ROM 在 CGB 上使用 CGB 模式，其余的 ROM 使用 DMG 兼容模式。
- 不使用 boot ROM。CGB E 的开机状态按 Pan Docs “Power Up Sequence” 的 “Console state after boot ROM hand-off” 和 “Hardware registers” 两节设置。这两节没有给出 DIV、STAT 和 LY，因为它们取决于 boot ROM 的运行时长。兼容模式的 DIV 按 mooneye `misc/boot_div-cgbABCDE` 的期望值反推，其他 IO 寄存器在兼容模式下的读出值按 `misc/boot_hwio-C` 设置，注释写明这些值只对应 licensee 不是任天堂的卡头。CGB 模式的 DIV 和两种模式下 PPU 的相位都没有来源，DIV 沿用兼容模式的值，注释写明没有依据。DMG 兼容模式按 “Behavior” 一节写入 KEY0 和 OPRI，再按 “Compatibility palettes” 一节的算法选择兼容调色板。调色板表取自这一节引用的 TCRF 页面，按 `refs/README.md` 的规则加入参考资料。
- WRAM 的 8 个 bank 和 SVBK，VRAM 的 2 个 bank 和 VBK。
- FF72 至 FF75 的未公开寄存器、KEY0 在开机以后的行为，以及 CGB 模式下未使用的 IO 位。红外端口 RP 和串口一样，只实现没有对端的情况。

PPU：

- CGB 模式的 BG 属性（VRAM bank、翻转、调色板和优先级），BG 和 OBJ 的调色板 RAM，BGPI、BGPD、OBPI、OBPD 的自动递增和 mode 3 期间的访问限制，OBJ 的 VRAM bank 和调色板号，OPRI 选择的 OBJ 优先级，以及 LCDC 第 0 位在 CGB 模式下的含义。
- DMG 兼容模式下，BGP、OBP0 和 OBP1 索引到 CGB 的调色板（Pan Docs “Compatibility palettes”）。
- 渲染任务带上这一行用到的调色板 RAM 和 VRAM bank 1，以及它们在这一行期间的修改。CGB 模式和兼容模式输出 BGR555 帧。mode 3 中途修改的处理和 GB 段相同。

DMA 和声音：

- GDMA 和 HDMA，按 Pan Docs “CGB Registers” 中 “LCD VRAM DMA Transfers” 一节实现，包括传输期间 CPU 的停顿、HDMA5 的读回和中途停止，以及 LCD 关闭时的 HDMA。
- APU 在 CGB 上的不同行为，以 blargg `cgb_sound` 检查的项目为准，另外加上 PCM12 和 PCM34 寄存器。

JIT：

- SVBK 或 VBK 切换以后，快速访存和代码缓存都不能用到另一个 bank 的内容。代码缓存的标识要区分 D000–DFFF 中不同 WRAM bank 的代码。
- HDMA 和 GDMA 让 CPU 停下的时间都要计入模拟时间，JIT 在这些时刻退出块。
- JIT 访问新增的 CGB 寄存器时经过处理函数，先把相关设备推进到访问的时刻。

测试通路：

- 测试协议和 runner 按机型运行 ROM。测试索引登记路线图 1-1 完成标准中的 CGB E 用例，记录机型、运行模式、判据和参考图。同一个 ROM 可以分别登记为 DMG-C 和 CGB E 的用例，报告区分机型。
- 测试通路回传 CGB 模式和兼容模式的 BGR555 帧。runner 按套件说明用 `(X << 3) | (X >> 2)` 把每个 5 位分量换算成 8 位，再和参考图逐像素比对。GB 模式的帧仍然回传灰阶等级。
- 卡上的测试数据由 `tools/prepare_gb_tests.py` 选择，已经包括路线图列出的 CGB E 用例和只记入报告的用例。增减用例时同步修改脚本和测试索引。
- SameSuite 的源码按 refs 的规则加入，固定到 game-boy-test-roms v7.0 的 CHANGELOG 记录的 `f71b4b3c37`。接入测试索引之前，用源码确认 `dma` 和 `ppu/blocking_bgpi_increase` 适用于 CGB E；确认不了的用例先和用户商量。
- 固定回归加入 CGB 的用例，覆盖 HDMA 和 CGB 的 PPU。名单和选择理由写进真机测试计划。
- PC 版能用命令行启动 CGB 模式的 ROM，用来调试画面。

## 不做

- KEY1 和 STOP 的速度切换、JIT 在倍速下的时间换算，以及要用到倍速的 blargg `cpu_instrs`、`interrupt_time` 和 AGE `speed-switch` 用例，这些在 1-2。KEY1 在 1-1 里只按原速读出。
- 从菜单启动 GBC 游戏、运行模式和颜色校正的设置、CGB 的即时存档和 2X，这些在 1-2。
- 颜色校正。测试比对的帧始终不经过校正。
- 联机和红外通信，CGB 0、PGB 模式和 AGB 的行为。
- 把只记入报告的用例作为完成条件。

## 约束

- GB 模式的行为、测试结果和速度不能因为本任务变差。机型的判断放在 bank 切换、寄存器访问和事件处理这些不频繁的地方，不加到每条指令、每次访存的路径上。
- 设备仍然在被访问或者事件到期时才追赶到当前时刻，不逐周期推进。渲染核仍然只使用任务里的数据，不读取实时状态。
- WRAM 和 VRAM 变大以后放在哪种内存，按实测决定。L2 cache 的大小和 JIT 代码池保持不变。
- 兼容调色板和颜色换算按套件的说明和参考图判定，不把模拟器自己的输出存成标准答案。
- 改动解释器、设备执行代码或 JIT 运行时 helper 时，按 `cmake/core_layout.cmake` 的规则放置，检查 map 中的地址。

## 由你决定

机型在系统接口里的表示、CGB 设备状态的组织、渲染任务里调色板和 VRAM bank 1 修改的格式、HDMA 在调度器中的实现方式、JIT 处理 bank 的方式，以及 CGB 帧的回传格式。选择的理由写在代码注释里。

## 完成标准

- 按[双后端优化要求](../../hardware-testing.md#每个任务的双后端优化)，先用同一组 GB ROM 比较改动前后两个后端的速度，确认 GB 模式没有变慢。再用模拟时长较长的 CGB 模式 ROM，比如 blargg `cgb_sound`，分析和优化 CGB 模式给两个后端增加的开销，提交两边各自的优化前后数据和剩余瓶颈。HDMA 的开销在 1-2 用游戏分析。清理临时计时以后，再做最终验证。
- 开发过程中按[真机测试计划](../../hardware-testing.md#选择本次运行的测试)选择受影响模块的 ROM，开发完就运行。
- 在 JC1060P470 上，两个后端都通过路线图 1-1 完成标准中的 CGB E 用例，结果一致，回退清单为空或者已经和用户确认。后面的改动影响到已经通过的用例时，重新运行这些用例。GB 模式只运行受影响模块的 ROM 和固定回归，两种机型的全量回归在 1-2 做。卡上的清单已经更新过。只记入报告的用例结果写进报告。
- 固定回归加入 CGB 用例以后不超过 1 分钟，超出时先和用户确认优化方案。
- 开发过程中只构建要刷进板子的固件和 PC 的 debug 版。用户确认可以提交时，全部板子的正式固件和测试固件构建一次，没有新的警告。

## 收尾

- 满足全部完成标准以后，在[任务状态](status.md)里把这个任务标为“已完成”。
- 在架构文档中说明机型和运行模式、CGB 的内存映射和设备、渲染任务的变化，以及 JIT 对 bank 的处理。在真机测试计划中写明 CGB 用例的选择、帧比对和固定回归。验证结论保存在运行报告中。
- 向用户汇报测试结果、耗时、速度、只记入报告的用例结果和回退清单。

## 实现与验证

都在 JC1060P470 上测量，芯片 ESP32-P4 v1.0，主频 360 MHz。速度取测试固件 `jc1060p470-test-release` 报告里的倍数，也就是模拟时长除以运行时间，JIT 从空的代码缓存开始，包括编译。报告都在 `build/test-reports/` 下。

### GB 模式的速度

DMG-C，同一组 ROM，改动前是 `gbc-1-1-gb-speed-before2`，改动后是 `gbc-1-1-gb-speed-final3`。

| ROM | 解释器 前 / 后 | JIT 前 / 后 |
|---|---|---|
| blargg `cpu_instrs` | 4.17 / 4.29（+2.9%） | 16.29 / 16.26（−0.1%） |
| blargg `dmg_sound` | 4.09 / 4.19（+2.3%） | 13.94 / 13.93（−0.1%） |
| blargg `mem_timing-2` | 4.29 / 4.41（+2.9%） | 14.53 / 14.70（+1.2%） |
| mooneye `intr_2_mode0_timing_sprites` | 3.60 / 3.65（+1.5%） | 12.67 / 12.50（−1.3%） |
| dmg-acid2 | 6.39 / 6.43（+0.6%） | 10.41 / 10.33（−0.8%） |

JIT 在两个 PPU 用例上慢了 0.8% 和 1.3%。这两个 ROM 的 PPU 事件多，用 `minstret` 计数看到 `Ppu::begin_transfer()` 每行多执行约 7 条指令，用来判断 HDMA 和 VRAM bank。剩下的差距没有拆清楚，可能和热代码地址的变化有关。不再追查。

### CGB 模式的开销

同一个 ROM 在 DMG-C 和 CGB E 上的速度。DMG-C 的数字来自 `gbc-1-1-all-after-speed`，CGB E 的来自最终固件的 `gbc-1-1-cgb-final`。

| ROM | CGB E 上的模式 | 解释器 DMG-C / CGB E | JIT DMG-C / CGB E |
|---|---|---|---|
| blargg `mem_timing-2` | CGB | 4.40 / 4.36 | 14.76 / 11.27 |
| blargg `mem_timing` | CGB | 4.19 / 4.11 | 13.69 / 11.00 |
| blargg `halt_bug` | CGB | 4.63 / 4.51 | 13.84 / 11.64 |
| mooneye `intr_2_mode0_timing_sprites` | 兼容 | 3.66 / 3.60 | 12.52 / 10.48 |
| dmg-acid2 | 兼容 | 6.47 / 5.59 | 10.33 / 8.45 |

解释器在 CGB 模式下慢 1% 到 3%。用 `jc1060p470-test-profile` 采样 `mem_timing-2`，core 1 有 81% 的时间在解释器里，CGB 增加的设备开销不是瓶颈，所以没有为解释器做优化。dmg-acid2 在兼容模式下慢了 14%，这个用例只运行约 0.06 秒，原因没有分析。

JIT 优化前，CGB 模式比 GB 模式慢约 30%。采样显示 core 1 有 26.8% 的时间在等渲染核，core 0 有 62.9% 的时间在 `segment_cgb()`，其中逐像素查颜色和 tile 解码各占四成左右。优化做了两处：

- 按 BG 调色板预先算好两个像素的颜色组合，调色板改变时只重算变了的调色板。tile 解码一次查出两个像素的颜色，不再逐像素查颜色。只有这一行有 OBJ 时，才另外保留每个像素的颜色号、调色板和优先级。
- LCD 关闭时，彩色帧先刷白一行，再复制到其余各行，不再逐像素写入。这一处只影响频繁开关 LCD 的 ROM。

CGB E 上优化前后的速度，优化前是 `gbc-1-1-cgb-after-review`，优化后是 `gbc-1-1-cgb-final`：

| ROM | 解释器 前 / 后 | JIT 前 / 后 |
|---|---|---|
| blargg `cgb_sound` | 4.12 / 4.13 | 10.57 / 12.61（+19.3%） |
| blargg `mem_timing-2` | 4.36 / 4.36 | 9.63 / 11.27（+17.0%） |
| blargg `halt_bug` | 4.46 / 4.51 | 9.92 / 11.64（+17.3%） |
| cgb-acid2 | 4.83 / 5.00 | 8.12 / 8.46（+4.2%） |
| mooneye `intr_2_mode0_timing_sprites`（兼容模式） | 3.60 / 3.60 | 10.03 / 10.48（+4.5%） |

剩余的瓶颈：

- CGB 模式的 JIT 仍然比 GB 模式慢 16% 到 24%。采样 `mem_timing-2`，core 1 等渲染核的时间从 26.8% 降到了 12.9%。core 0 上 tile 解码占 40.6%，每个 tile 要读属性、选 bank、处理翻转和调色板，另外 `memcpy` 占 13.1%。
- 兼容模式的 JIT 比 GB 模式慢 16% 到 18%。采样 `intr_2_mode0_timing_sprites`，core 0 上 `segment()` 的样本是 GB 模式的 3 倍。多出来的有把灰阶换成颜色和空白行刷白，tile 解码的样本也比 GB 模式多，原因没有查明。试过按调色板查表转换兼容模式的颜色，速度没有提高，没有保留。
- cgb-acid-hell 的 JIT 是 2.06 倍，慢于解释器的 2.52 倍。这个 ROM 每行都在 mode 3 中途写寄存器，原因没有分析，它是只记入报告的用例。
- HDMA 的开销按计划在 1-2 用游戏分析。

### 片内 RAM

`idf.py size` 的 DIRAM 静态部分：正式固件 `jc1060p470-release` 从 140,656 B 增加到 151,836 B，多了 11,180 B，其中 IRAM 代码 10,516 B，`.data` 664 B。测试固件从 135,978 B 增加到 147,394 B，多了 11,416 B。增加的是执行核心里 CGB 的设备和渲染代码。兼容模式和 GB 模式共用渲染路径，渲染器里的排序也改成了插入排序，在这之前测试固件的增量是 22.1 KB。

CGB 的 WRAM bank 2 至 7、VRAM bank 1 和调色板约 32 KB，只在 CGB E 上分配，放在 PSRAM。它们放在片内时，测试固件的诊断模式和并发调度下，JIT 的代码池分不到片内内存。和放在片内时相比，CGB E 用例的速度变化在 ±1.6% 以内。

### 测试

- 最终固件运行 CGB E 的全部用例（`--all --model "CGB E"`），报告 `gbc-1-1-cgb-final`，耗时 79.7 秒。必过的 72 个用例中 70 个通过，两个后端结果一致；blargg `cpu_instrs` 和 `interrupt_time` 要用倍速，留到 1-2。和优化前的 `gbc-1-1-cgb-after-review` 相比，139 个用例在两个后端上的状态和帧摘要完全相同。
- 只记入报告的 89 个用例：Mealybug 的 27 个中 5 个通过；SameSuite APU 的 61 个中 6 个通过、35 个失败，20 个要用倍速，留到 1-2；cgb-acid-hell 失败。
- DMG-C 的全部用例在审查修改以前运行过一次，报告 `gbc-1-1-all-after-speed`，130 个用例的结果和改动前相同：129 个通过，只记入报告的 blargg `oam_bug` 因为参考图不符合要求而无法判定。审查修改以后运行了 stat 和 interrupt 模块的 ROM（`gbc-1-1-dmg-after-review`），全部通过。回退清单为空。
- 固定回归 14 个用例全部通过，耗时 10.6 秒，报告 `gbc-1-1-regression-final`。
- 诊断模式逐步比对两个后端的总线访问：CGB E 的 SameSuite `gbc_dma_cont`、`hdma_mode0`、`gdma_addr_mask`、`hdma_lcd_off` 和 mooneye `stat_irq_blocking`，DMG-C 的 mooneye `oam_dma/basic`、`stat_irq_blocking` 和 `ie_push`，全部通过。并发调度下运行 CGB E 的 `oam_dma/basic` 和 cgb-acid2，通过。

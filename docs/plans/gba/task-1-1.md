# 任务 1-1：第三方测试数据

这是[路线图](../../roadmap.md#gba) GBA 段里程碑 1 的第一个任务：把第三方的 GBA 套件整理成 SD 卡上的测试数据，选出要用的 ROM，定下每个 ROM 的判据，生成数据清单。1-2 在这之上加入自写的测试 ROM 和参考帧。两个任务做完以后，卡上的 GBA 测试数据不再改动，所以后面各任务会用到的第三方数据要在这里一次选全。这个任务不写模拟器代码。

开工时在[任务状态](status.md)里把这个任务标为“进行中”。

## 先读

- 路线图 GBA 段开头的几段和里程碑 1、2 全文，[产品设计](../../product-design.md)的“SD 卡目录”和“正确性验证”。
- [真机测试计划](../../hardware-testing.md)的“SD 卡和数据目录”“选择本次运行的测试”和“通过判据”。
- `tools/prepare_gb_tests.py` 和 [`tools/README.md`](../../../tools/README.md)：GB 用例包的格式，以及清单和来源元数据的规则。
- [`refs/README.md`](../../../refs/README.md) 的“测试”一节，`refs/tests/arm7tdmi/README.md` 和 `v1/transcode_json.py`，以及 jsmolka、alyosha 和 nba-hw-test 的 README 和测试源码。

## 范围

用例包：

- 卡上的 GBA 测试数据放在 `tests/gba/` 下，和 `tests/gb/` 并列。目录的具体布局由你决定，写进真机测试计划“SD 卡和数据目录”一节。
- ARM7TDMI 单步用例的 45 个 `.json.bin` 文件转换成板子可以直接读取的用例包，按文件和用例建立索引，板子只读取选中的部分，不把整个用例包载入内存。用例包完整保留初始状态、预期结果和全部总线访问。`cycle` 字段不参与判定，但是诊断时要能显示，所以也保留。转换以后逐条和来源核对。
- 按 README 和 `transcode_json.py` 弄清每个字段的含义，写进生成脚本开头的说明，1-3 和 1-4 照它实现：`kind` 的取值、`access` 的各个位、`pipeline` 和状态里的 `access`，以及取指的返回规则（取 `base_addr` 时返回 `opcode`，其他取指返回请求的地址本身）。README 的 “Known Issues” 和 “Disclaimers” 两节也要记下来：`r15` 同时作为写回寄存器和目标寄存器的几种非法组合可能不准，用例不检查读写的对齐，周期数没有正确记录，访问的先后顺序是对的。
- 生成脚本沿用 `prepare_gb_tests.py` 的规则：原始套件的版本由 `refs/fetch.sh` 和 `refs/fetch.ps1` 固定，没有来源元数据的目录拒绝打包，输出目录必须尚不存在，清单记录来源版本、相对路径、文件大小和 SHA-256。1-2 要往同一份数据里加入自写的 ROM 和参考帧，脚本的结构要留出这一步。
- 报告用例包的大小，并估算板上读取全量要多久，写明所用读取速度的来源，比如 GB 段已有报告中的读卡耗时。

测试 ROM 的选择：

- jsmolka 的 `arm`、`thumb`、`memory`、`bios`、`nes` 和 `save`，运行到 idle 循环时检查报告结果的寄存器。jsmolka 和 alyosha 都用 `lib/macros.inc` 的 `m_test_eval` 显示结果，它先保存 `r0` 至 `r12`，显示完再恢复，所以到 idle 循环时结果在传给它的那个寄存器里，不一定是 `r12`：jsmolka 的 `thumb` 传的是 `r7`。`ppu` 只能目测，放到卡上，由 1-2 生成参考帧。`unsafe` 不用。
- alyosha 的 `Bus`、`DMA`、`Interactions`、`Serial`、`fifo_dma`、`irq`、`prefetcher`、`psr`、`timer` 和 `timing`。`LDM` 和 `prefetcher/bounday_test_1.gba` 不用，理由见 `refs/README.md`。
- alyosha 的 `ppu` 目录不是 jsmolka 的旧副本，它比 jsmolka 多出 9 个测试。`Sprite_Last_VRAM_Access`、`Sprite_Last_VRAM_Access_Free`、`start_up` 和 `start_up_vbl` 在 idle 循环检查 `r12`；`start_up_vbl_irq` 和 `start_up_vbl_irq_halt` 把结果传给 `m_test_eval` 的是 `r7`，`r12` 一直是 0，所以检查 `r7`；`Sprite_Disable_Midline` 和 `Sprite_Disable_on_VRAM_Access` 一直跳回 `t001`，不会到达 idle 循环，和 `stripes_x_offset` 一样只能看画面，由 1-2 生成参考帧。逐个目录和 jsmolka 比较，确认 alyosha 其余目录是不是确实只是旧副本，改正 `refs/README.md` 的说法，包括 jsmolka `thumb` 用 `r7` 报告结果这一点。
- nba-hw-test 的 `bus`、`dma`、`haltcnt`、`irq`、`timer` 下的 10 个 ROM，以及 `ppu` 下的 `status-irq-dma` 和 `vram-mirror`，它们在屏幕上逐项打印 PASS 或 FAIL。`ppu` 下附 `expected.png` 的 5 个测试放到卡上，由 1-2 生成参考帧。`ram-access-timing` 只打印测量值，没有预期结果，不用。

判据：

- alyosha 的每个 ROM 读源码，记下它测的行为、判据和作者说明的验证条件，按路线图 GBA 段的精度目标分成必过和只记入报告。游戏会依赖的行为，比如等待周期、N/S 访问、预取、DMA、中断和定时器的时序，列为必过；源码说明在实机上结果不稳定、只在原机上的特殊条件下才出现，或者测的是游戏不太可能依赖的行为的，只记入报告，并写明理由。分不清的项列出来和用户商量。
- nba-hw-test 用 libgba 的 `printf` 在屏幕上打印结果，全部通过时打印 `congratulations!`，见 `lib/source/test.c`。定下 runner 怎样认出这些文字，以及每个 ROM 的结束条件和模拟时间上限。这些 ROM 测的是总线、DMA、中断和定时器，所以判定办法不能依赖 PPU 合成画面的正确性，比如可以在结束时从 VRAM 读出 BG 的图块表和字形。实现放在 2-1。
- 每个 ROM 的结束条件和判据按它自己的源码确定：jsmolka 和 alyosha 运行到 idle 循环，也就是跳转到自身的那条指令时停止，再检查这个 ROM 传给 `m_test_eval` 的寄存器。逐个确认 ROM 能到达 idle 循环、报告结果用的是哪个寄存器，不能统一按 `r12` 判定；到不了 idle 循环的 ROM 改用画面判定，交给 1-2 生成参考帧。
- 选择结果写进本文末尾的“用例选择”一节：每个 ROM 的路径、判据（包括报告结果的寄存器）、结束条件、必过还是只记入报告，以及理由。2-1 建立测试索引时照这份名单登记，路线图里程碑 2 的完成标准也引用它。

BIOS：

- BIOS 是用户从原机转储的文件，不放进用例包，也不进入数据清单。找一个有出处的校验值，比如 No-Intro 的 BIOS 条目，用来核对用户的转储；校验值和出处写进生成脚本的说明，2-1 的测试固件和 3-1 的 app 用同一个值。BIOS 在卡上的路径见产品设计“SD 卡目录”。

## 不做

- 自写的测试 ROM 和参考帧，这是 1-2 的内容。
- 测试协议、测试固件和 runner 的执行，这些从 1-4 和 2-1 开始。
- 测试索引的代码，这在 2-1。本任务只给出名单。

## 约束

- 测试套件和生成的数据留在本地，不提交到仓库，也不重新分发。
- 用例包格式要覆盖 1-4 和里程碑 2 的全部需要，因为 1-2 以后不再改动卡上的数据。格式写在生成脚本开头，和 `prepare_gb_tests.py` 一样。
- 判据只来自套件的 README 和源码，不能按我们的模拟器会得出的结果来选，这时也还没有模拟器。
- 转换不能丢弃或改写任何字段。来源有错的用例不在用例包里修改，列出来交给 1-3 和 1-4 处理。

## 由你决定

用例包的布局和索引、是否压缩、生成脚本的名字和命令行，以及 `tests/gba/` 下的目录结构。选择的理由写在脚本的说明里。

## 完成标准

- 生成脚本从 `refs/tests` 生成 `tests/gba/` 和数据清单，ARM7TDMI 单步用例逐条核对通过。
- 数据复制到卡上，读回以后清单校验一致。
- 本文的“用例选择”一节列出全部 ROM 的路径、判据、结束条件、分类和理由，用户已经确认 alyosha 的分类。
- nba-hw-test 的判定办法已经定下来，写在“用例选择”一节。
- BIOS 的校验值和出处已经写进脚本的说明。
- `refs/README.md` 中 alyosha 的说法已经改正。`tools/README.md` 写明新脚本的用途，真机测试计划的“SD 卡和数据目录”写明 GBA 数据的来源和生成命令。
- 这个任务不改固件，不需要上板运行，也不需要构建固件。

## 收尾

- 满足全部完成标准以后，在[任务状态](status.md)里把这个任务标为“已完成”。
- 向用户汇报用例包的大小、估算的读取时间、选出的 ROM 数量和分类，以及发现的来源问题。

## 用例选择

卡上的布局和生成命令见真机测试计划“SD 卡和数据目录”，用例包的格式、字段含义和 BIOS 的校验值写在 `tools/prepare_gba_tests.py` 开头。下文的 ROM 路径相对于各套件在 `refs/tests/` 下的目录。

### 测试数据

ARM7TDMI 单步用例的 45 个文件每个有 50000 条，共 2,250,000 条，全部放进用例包。用例包有 496,981,292 字节，来源的 `.json.bin` 共 933,766,929 字节。用例包比来源小，是因为最终状态只存和初始状态不同的字，总线访问的各个字段按实际的取值范围存储，没有丢弃字段。

板上读完用例包要多久还没有实测，GB 段的报告也没有把读卡耗时单独记下来。第三方在一块 ESP32-P4 板子上用 4 线 SDMMC、20MHz 时钟经 FATFS 读文件，缓冲按 128 字节对齐时实测约 7.4MB/s（[MicroPython 的 PR #75](https://github.com/PyDevices/micropython-pydevices/pull/75)），照这个速度读完约 67 秒。本项目的 SD 时钟是 40MHz，4 线总线的理论上限是 20MB/s，对应约 25 秒。1-4 实测以后以实测为准。

卡上共有 137 个 ROM：jsmolka 12 个，alyosha 108 个，nba-hw-test 17 个。

### jsmolka 和 alyosha 的判据

两个套件都用 `lib/macros.inc` 的 `m_test_eval` 显示结果，它先保存 `r0` 至 `r12`，显示完再恢复（61–127 行），所以运行到 idle 循环时，结果在传给它的那个寄存器里。下表写明每个 ROM 用的寄存器。0 表示通过；非 0 时，jsmolka 是第一个失败的测试编号，alyosha 的很多 ROM 报告的是测到的计数值或地址。

- 结束条件是执行到跳转到自身的指令，不按地址判断，因为 `nes` 把代码复制到 VRAM 里运行（`nes/nes.asm` 15–26 行）。
- 失败时显示结果要调用 BIOS 的 Div（`lib/macros.inc` 95、99 行），用 HALT 等中断的 ROM 收不到中断时会一直等下去，所以失败的 ROM 可能到不了 idle 循环。每个 ROM 的模拟时间上限定为 10 秒，到上限判为失败。套件没有规定上限，这个值只用来结束卡住的 ROM，2-1 实测以后可以调整。
- 存档的 4 个 ROM 要从全新的存档开始，存档区读出 0xFF，存档类型按 ROM 里的 ID 字符串判断。这两个套件所有 ROM 的游戏代码都是 `1337`（`lib/header.asm` 24 行），不能按游戏代码查表。
- `bios/bios.gba` 检查开机后、SWI 以后和 IRQ 里外读 BIOS 区得到的值（`bios.asm` 21–103 行），第 1 项要求开机后读出 0xE129F000，所以直接开机时要像 BIOS 的开机流程结束时一样设置 BIOS 区的读出值。用中断的 ROM 都经过 BIOS 的 IRQ 分派，有的还弹出 BIOS 的 IRQ 栈帧，或者比较 BIOS 里的返回地址。
- 说明栏标“开机相位”的 30 个 ROM，结果取决于进入 ROM 时 PPU 在一行里的位置、定时器预分频的相位和开机留下的 TIM0 计数，开机以后没有重新同步。本项目执行正版 BIOS 的代码，只跳过开机流程，所以直接开机设置的状态要和原机 BIOS 交出控制时相同，包括这些相位。`ppu/start_up` 系列测的就是这个状态，比如进入 ROM 时 VCOUNT 是 0x7F（`ppu/start_up.asm` 71–74 行）。用户确认这些 ROM 和其他 ROM 一样按测的行为分类。
- 有几种错误也会让结果为 0。Internal_Cycle 系列里用中断的 11 个和 `irq/BL_IRQ` 在完全收不到中断时通过，`irq/BL_IRQ_2` 的第 4 项也一样；`DMA_pause_timing_end_1`、`end_3` 和 `mid_1` 在 HBlank DMA 没有运行时通过；报告测量值的 ROM，测到的值恰好是 0 时也通过。这些 ROM 通过不能单独证明对应的功能正确，要和同组的其他用例一起看。

### jsmolka

| ROM | 判据 | 分类 | 说明 |
|---|---|---|---|
| `arm/arm.gba` | `r12` | 必过 | |
| `thumb/thumb.gba` | `r7` | 必过 | `thumb.asm` 6、23、44 行；它从不写 `r12`，读 `r12` 可能误判为通过 |
| `memory/memory.gba` | `r12` | 必过 | |
| `bios/bios.gba` | `r12` | 必过 | 需要正版 BIOS 和开机后的 BIOS 读出值 |
| `nes/nes.gba` | `r12` | 必过 | idle 循环在 VRAM 里 |
| `save/none.gba` | `r12` | 必过 | 没有存档芯片，0x0E000000 和 0x0F000000 读出 0xFF |
| `save/sram.gba`、`save/flash64.gba`、`save/flash128.gba` | `r12` | 必过 | 全新存档，类型按 ID 字符串判断 |
| `ppu/hello.gba`、`ppu/shades.gba`、`ppu/stripes.gba` | 画面，和 1-2 的参考帧比较 | 必过 | 没有 `m_test_eval`，画完一幅画面就进入 idle 循环，只能看画面 |

`unsafe` 不用，作者说明它在实机上不通过，可能是烧录卡的原因。

### alyosha

没有特别说明的 ROM 都运行到 idle 循环，检查 `r12`。

| ROM | 测的行为 | 分类 | 说明 |
|---|---|---|---|
| `Bus/` 下的 6 个 | DMA 和 CPU 访问未映射区、未对齐的 LDRSH 时 open bus 的值 | 必过 | 用户确认 |
| `DMA/DMA_Mode_Change.gba` | 已经打开的 HBlank DMA 改成立即启动，不开始传输 | 必过 | |
| `DMA/DMA_ROM_Fixed.gba` | 源地址固定在 0x0801FFFC 的立即 DMA，128KiB 边界上的 N/S 时序 | 必过 | |
| `DMA/DMA_pause_timing_` 的 `end_1` 至 `end_4`、`mid_1`、`mid_2`、`ROM_to_IWRAM` | HBlank DMA 打断立即 DMA 的时刻，以及从 ROM 读的 DMA 恢复时用 S 访问 | 必过 | 开机相位 |
| `fifo_dma/fifo.gba` | 声音总开关关闭时 SOUNDCNT 的读出值、FIFO 的写入和清空，FIFO DMA 的时序 | 只记入报告 | 作者注明 t002 和 t004 在实机上结果异常（`fifo.asm` 81–83、224 行）；ROM 和源码不一致 |
| `fifo_dma/fifo_2.gba` 至 `fifo_6.gba` | FIFO DMA 在不同定时器相位、预先装了数据和打开 DMA 中断时让 CPU 停下的时间 | 必过 | |
| `fifo_dma/fifo_dma_disable_3.gba`、`_4.gba`、`_5.gba` | FIFO DMA 将要发生时清除 DMA1CNT | 只记入报告 | `_3` 和 `_5` 没有任何检查，跑到 idle 循环就算通过；相邻的设定值在 `unsafe/` 里会让实机死机 |
| `Interactions/Halt_IRQ.gba`、`Halt_DMA_IRQ.gba` | HALT 时 VBlank 中断唤醒的时刻和压栈的 PC，后者叠加重复的 HBlank DMA | 必过 | 开机相位 |
| `Interactions/Halt_DMA_IRQ_Read_OAM.gba` | 同上，DMA 写 OAM，显示期间 CPU 也写 OAM | 只记入报告 | 取决于 PPU 读 OAM 的时序，PPU 按扫描线渲染时做不到 |
| `Interactions/Internal_Cycle_` 开头的 13 个 | DMA 把 CPU 停在内部周期上时，中断落在哪一条指令上，以及 MUL 的内部周期 | 必过 | 开机相位；`Internal_Cycle_DMA_IRQ_Br_pre_tim.gba` 和 `Internal_Cycle_DMA_TIM_Br_pre.gba` 完全相同 |
| `irq/` 下的 16 个 | Thumb BL 前后的中断、HALT 的 PC、IE 和 IF 的时序、BIOS 返回时待处理的中断 | 必过 | `IRQ_sub_2.gba`、`IRQ_sub_2_slow.gba` 是开机相位 |
| `prefetcher/` 下除 `bounday_test_1.gba` 以外的 17 个 | 预取缓冲在 128KiB 边界、分支、BX、DMA 和缓冲满时的时序 | 必过 | `bounday_test_1.gba` 没有源码，不用 |
| `psr/psr.gba` | SYS 模式的 SPSR、模式位 4、TEQ 写 PC 恢复 CPSR 和中断的时序 | 必过 | |
| `Serial/` 下的 9 个 | 没有对端时内部时钟传输完成的时刻、启动位清零的时刻、SIODATA8 读出 0xFF | 必过 | 用户确认 |
| `timer/timer.gba` | 开机以后 F/64 预分频的相位 | 必过 | 开机相位；作者说明它依赖开机的准确时刻，在烧录卡上不能用 |
| `timer/timer_disable.gba` | 计数为 0xFFFF 时打开定时器仍然请求中断 | 必过 | |
| `timer/timer_reset.gba` | 打开定时器后马上读到的是原来的计数 | 必过 | 开机相位，第 1 项读的是开机留下的 TIM0 |
| `timing/` 下的 5 个 | BIOS CpuSet 的耗时、写 DMA 控制寄存器到开始的延迟、64K 字的 DMA、打开预取时写 WAITCNT | 必过 | 需要正版 BIOS |
| `arm/Coprocessor_14.gba` | MRC p14 读到 open bus 的值和它的时序，MRC 写 PC 时设置标志位 | 必过 | 用户确认 |
| `thumb/blx.gba`、`cpy.gba`、`Pop_no_regs.gba`、`Push_no_regs.gba` | H1 位置位的 BX、两个低寄存器的 hi 寄存器 MOV、空列表的 POP 和 PUSH | 必过 | 用户确认 |
| `memory/PAL_8_bit_writes.gba`、`VRAM_8_bit_writes.gba` | 按字节写调色板和 VRAM 时，字节复制到整个半字 | 必过 | 用户确认 |
| `STM_ALU_IMM.gba` | FIQ 模式下 STM^ 存的是用户模式的 r8 | 只记入报告 | 结果在 `r0`，到 idle 循环时和 `r12` 相同；它引用的 `../lib/` 不在仓库里，像是从 `LDM/` 移出来的，`LDM/` 的作者只在 Game Boy Player 上测过 |
| `ppu/start_up.gba`、`start_up_vbl.gba` | 进入 ROM 时的 VCOUNT 和换行时刻、到第一次 VBlank 的时间 | 必过 | 开机相位 |
| `ppu/start_up_vbl_irq.gba`、`start_up_vbl_irq_halt.gba` | 第一次 VBlank 中断的时刻，后者用 HALT 等待 | 必过 | 开机相位；结果在 `r7`（`start_up_vbl_irq.asm` 148 行），`r12` 一直是 0 |
| `ppu/Sprite_Last_VRAM_Access.gba`、`_Free.gba` | OBJ 取图期间 CPU 读 OBJ 的 VRAM 的时刻 | 只记入报告 | 取决于 PPU 访问 VRAM 的时序，PPU 按扫描线渲染时做不到 |
| `ppu/Sprite_Disable_Midline.gba`、`Sprite_Disable_on_VRAM_Access.gba` | 一行中途关闭 OBJ | 只记入报告 | 一直跳回 `t001`，到不了 idle 循环，只能看画面，和 1-2 的参考帧比较；源码没有描述期望的画面，用户确认用 NanoBoyAdvance 的画面作参考帧；两个文件完全相同 |
| `ppu/stripes_x_offset.gba` | 一行中途修改 BG0HOFS | 只记入报告 | 没有 `m_test_eval`，只能看画面。它轮询 VCOUNT 来卡写入的时机，在 NanoBoyAdvance 上每一帧的画面都不同，所以没有参考帧，保存实际帧记为 `MANUAL`，由人查看 |

`arm`、`bios`、`memory`、`nes`、`save`、`thumb`、`ppu` 和 `unsafe` 里和 jsmolka 同名的文件，除 `arm/arm.gba` 以外都和固定版本的 jsmolka 相同，所以只用 jsmolka 的。`arm/arm.gba` 是旧版本，缺少 jsmolka 在 test 254 前后切换 FIQ 模式的修复。`LDM/` 不用，理由见 `refs/README.md`；`unsafe/` 新增的 3 个会让实机死机，也不用。

### nba-hw-test 的判据

结果用 libgba 的控制台打印在 BG0 上。图块表在 0x06002000，30 列 × 20 行，每项是 `0xF000 | 字符`，图块号就是 ASCII 码，字形在 0x06000000 开始的图块里（libgba 的 `console.c`，已经在 `128kb-boundary.gba` 的反汇编里核对过）。runner 在结束时从 VRAM 读出图块表还原文字，不依赖 PPU 合成的画面。

- 通过条件：源码里每个 `test_expect` 的名字都以 `<名字>: PASS <值>` 出现，没有 `FAIL`；打印总结的 ROM 还要有 `congratulations!`（`lib/source/test.c` 61–67 行）。失败的行会占两行，多了会把上面的行滚出屏幕，但是总结在最下面。
- 结束条件：除 `ppu/status-irq-dma` 以外，打印完都进入 `while (1) {}`，执行到跳转到自身的指令时停止。
- 期望的周期数包含 BIOS 代码的耗时，所以要用正版 BIOS：控制台的清屏和滚屏调用 `CpuFastSet`，`haltcnt` 调用 `CpuSet`，中断都经过 BIOS 的分派。这些 ROM 不写 WAITCNT，按开机后的默认值运行。
- 模拟时间上限按源码估算，没有实测；一帧是 280,896 个周期。

| ROM | 测的行为 | 判据和上限 | 分类 |
|---|---|---|---|
| `bus/128kb-boundary/128kb-boundary.gba` | LDM 和 DMA 跨过 128KiB 边界和 OAM 到 ROM 的边界时的耗时 | 18 行全部 PASS；不打印总结（`main.c` 103–104 行）；300 帧 | 必过 |
| `dma/burst-into-tears/burst-into-tears.gba` | 从 0x07FFFFFE 开始的 16 位 DMA 读到的值和耗时 | 3 行和总结；300 帧 | 必过 |
| `dma/force-nseq-access/force-nseq-access.gba` | ROM 里的 STR 启动的立即 DMA 的耗时 | 2 行和总结；300 帧 | 必过 |
| `dma/latch/latch.gba` | DMA 的数据 latch 和 open bus | 4 行和总结；300 帧 | 必过 |
| `dma/start-delay/start-delay.gba` | 立即 DMA 的启动延迟 | 1 行和总结；300 帧 | 必过 |
| `haltcnt/haltcnt.gba` | 直接写和经 BIOS CpuSet、DMA 写 HALTCNT 的效果，POSTFLG，有待处理中断时进入 HALT 的耗时 | 6 行和总结；300 帧 | 必过 |
| `irq/irq-delay/irq-delay.gba` | 从 IWRAM、EWRAM 和 ROM 打开 IME 到进入中断处理的周期数 | 3 行和总结；300 帧 | 必过 |
| `timer/reload/reload.gba` | 16 位和 32 位写入重载值的各个时刻 | 7 行和总结；300 帧 | 必过 |
| `timer/start-stop/start-stop.gba` | 启动和停止以后读到的计数 | 2 行和总结；300 帧 | 必过 |
| `timer/tick-before-reload/tick-before-reload.gba` | 重载前一个周期的计数和溢出 | 2 行和总结；300 帧 | 必过 |
| `ppu/vram-mirror/vram-mirror.gba` | VRAM 镜像在图块模式和位图模式下的读写 | 10 行和总结；600 帧，CRC 计算约要 80 帧 | 必过 |
| `ppu/status-irq-dma/status-irq-dma.gba` | DISPSTAT、VCOUNT、IF 的各个时刻，HBlank、VBlank 和视频捕获 DMA | 见下文 | 必过 |
| `ppu/dispcnt-latch/dispcnt-latch.gba` | 逐行修改 DISPCNT 的 BG、OBJ、窗口开关和强制空白 | 和 1-2 的参考帧比较画面 | 必过 |
| `ppu/` 下的 `bgpd`、`bgx`、`greenswap`、`sprite-hmosaic` | BG2PD、BG3PD 和 BG2X、BG3X 写入以后何时生效，一行之中 HBlank DMA 写 GREENSWAP 的效果，OBJ 的横向 mosaic | 和 1-2 的参考帧比较画面 | 只记入报告 |

`ppu/status-irq-dma` 是菜单，按键松开时动作（`lib/source/ui.c` 56–97 行），每次显示菜单都从第一项开始。DISPSTAT、VCOUNT、IRQ (IF) 和 DMA 四个子测试分别登记成一个用例：复位以后，等菜单出现，用按键脚本按 0 至 3 次下，再按 A。打印完结果以后 ROM 停在等 B 的循环里，不是跳转到自身，所以结果的最后一行出现并且 2 帧不变时停止。DMA 子测试不打印总结，按 3 行全部 PASS 判定。上限是 DISPSTAT 4000 帧、VCOUNT 1300 帧、IRQ (IF) 1300 帧、DMA 60 帧。

`ppu/ram-access-timing` 只打印测量值，没有期望值，不用。`archive/` 是作者淘汰的旧测试，不用。

### 来源问题

- ARM7TDMI 的 README 一处说每个文件 20,000 条，另一处说 50000 条，实际是 50000 条。README 说取指返回请求的地址本身，实际返回的是按访问宽度截断的地址，半字取指只返回低 16 位。这两点都写在了生成脚本的说明里。现在还没有模拟器，有没有预期结果错误的用例由 1-3 和 1-4 核对。
- alyosha 的 `fifo_dma/fifo.gba` 和 `fifo.asm` 不一致：源码里 t002 期望 0x0080008B（142–143 行），ROM 里对应的位置是 0x00800098。
- alyosha 的 `prefetcher/bounday_test_1.gba` 没有源码。
- alyosha 有两对完全相同的 ROM，见上表。
- nba-hw-test 的 `bus/128kb-boundary` 不打印 `congratulations!`，`ppu/status-irq-dma` 要用按键选择子测试，`refs/README.md` 原来的说法不准确。

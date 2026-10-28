# 真机测试计划

测试数据常驻 SD 卡，电脑选择本次要跑的用例。SM83 的日常测试和里程碑验收都在板上验证。出现失败、需要进一步排查时，才对出错用例使用诊断模式，将压缩后的实际结果发回电脑判定。日常按实际改动选择模块和相关回归，里程碑验收才默认运行全量。

本文说明测试数据准备、用例选择、执行方式和判据，适用于 SM83 和 ARM7TDMI 单步、JIT 后端检查、显示图案检查、音频水位检查，以及 DMG-C 和 CPU CGB E 的整机 ROM 测试。测试结果、性能数据和构建记录保存在运行报告中。各脚本的选项见 `--help`。

## SD 卡和数据目录

测试卡采用 MBR 分区表和一个 FAT32 分区。ESP-IDF 的 SD 挂载方式见[官方文档](https://docs.espressif.com/projects/esp-idf/en/v6.1/esp32p4/api-reference/storage/fatfs.html)。

测试数据放在卡根目录的 `tests/` 下面，套件内部保留原有目录和文件名。电脑上的副本放在仓库的 `tests/sdcard/`，`gb/` 和 `gba/` 对应卡上的 `tests/gb/` 和 `tests/gba/`。打包脚本默认输出到这里，runner 的 `--data` 默认也读这里。卡上其他目录的用途见[产品设计](product-design.md#sd-卡目录)。

| 卡上路径 | 电脑上的来源 | 内容 |
|---|---|---|
| `tests/gb/sm83/cases.bin` | `refs/tests/sm83/` | 按操作码建立索引的单步二进制用例包，同目录保留说明和许可 |
| `tests/gb/roms/` | `refs/tests/game-boy-test-roms/` | 选中的 GB 和 GBC 测试，包括对应参考图和套件判定说明 |
| `tests/gb/manifest.tsv` | 从以上文件生成 | 来源版本、相对路径、文件大小和 SHA-256 |
| `tests/gba/arm7tdmi/cases.bin` | `refs/tests/arm7tdmi/` | 按文件和用例建立索引的 ARM7TDMI 单步用例包，同目录保留说明和许可 |
| `tests/gba/roms/` | `refs/tests/` 下的 `jsmolka-gba-tests/`、`alyosha-gba-tests/` 和 `nba-hw-test/` | 选中的 GBA 测试 ROM，按套件保留原有路径，附说明和许可；nba-hw-test 的 PPU 测试附带的 `expected.png` 只供目测 |
| `tests/gba/roms/gbx/` | `tests/gba/roms/`，由 `tools/gba_test_roms.py` 构建 | 自写的测试 ROM：PPU 场景、EEPROM、SRAM、Flash、RTC 和 32MiB ROM 的读取 |
| `tests/gba/frames/` | 仓库的 `tests/gba/frames/` | GBA 参考帧：`frames.tsv`，以及按套件和 ROM 路径存放的 PNG |
| `tests/gba/manifest.tsv` | 从以上文件生成 | 来源版本、工具链、参考模拟器的版本、相对路径、文件大小和 SHA-256 |

原始套件的来源版本由 `refs/fetch.sh` 和 `refs/fetch.ps1` 固定，修订副本以自身 `revision.json` 为准。没有来源元数据的自定义目录会被拒绝打包。更新数据后重新生成清单，复制到卡上并读回校验。板子开机时只计算清单的 SHA-256，不读取用例包核对，所以复制以后的读回校验不能省。套件、修订副本和 `tests/sdcard/` 都留在本地，不提交到仓库，也不重新分发；GBA 的参考帧例外，提交在 `tests/gba/frames/`。

先用 `tools/fix_sm83_stop.py` 生成修订副本 `sm83-stop-pc-v1`，再用 `tools/prepare_gb_tests.py` 生成卡上的测试数据，两个脚本的用途见[工具说明](../tools/README.md)。修订副本的输出目录必须尚不存在；`prepare_gb_tests.py` 默认输出到 `tests/sdcard/gb`，全部成功以后才替换原来的目录：

```sh
python3 tools/fix_sm83_stop.py build/sm83-stop-pc-v1
python3 tools/prepare_gb_tests.py --sm83-source build/sm83-stop-pc-v1
```

用例包完整保留初始状态、预期结果和总线采样，转换后逐条与来源核对。报告必须区分原始套件与修订套件。

GBA 的测试数据由 `tools/prepare_gba_tests.py` 生成，规则和 GB 相同，默认输出到 `tests/sdcard/gba`。自写的测试 ROM 要用 Arm GNU Toolchain 15.3.Rel2 构建，`--toolchain` 指定解压后的目录，下载的文件名见 `tools/gba_test_roms.py` 开头：

```sh
python3 tools/prepare_gba_tests.py --toolchain <工具链目录>
```

打包前先核对参考帧：自写 PPU 场景的参考帧要和 `tests/gba/roms/ppu/model.py` 按 GBATEK 算出的画面相同。打包时每个有参考帧的 ROM 还要和 `frames.tsv` 记录的 SHA-256 相同，不同说明参考帧取自另一个 ROM，要用 `tools/gba_reference_frames.py` 重新生成。

ARM7TDMI 用例包的格式和字段含义、选中的 ROM 写在脚本开头，各 ROM 的判据和分类见 GBA [任务 1-1](plans/gba/task-1-1.md#用例选择)，自写 ROM 和参考帧的判据见[任务 1-2](plans/gba/task-1-2.md#核对结果)。GBA 的 BIOS 是用户从原机转储的文件，放在卡上的 `bios/gba_bios.bin`，不属于测试数据，也不进入清单。`--check-bios <文件>` 按 No-Intro 的校验值核对转储。

板子需要启用 FatFs 长文件名支持，并保留文件名中的空格。挂载路径为 `/sdcard`。测试固件挂载失败时报告错误，不自动格式化卡。

## 共用的二进制传输

所有板上测试共用一套二进制传输，不逐条打印文本回传结果。帧格式、大小限制和 CRC 定义在 `tests/protocol/`，板上的收发在 `tests/device/transport.cpp`，电脑端在 `tests/runner/protocol.py` 和 `transport.py`。串口连接依赖 pyserial，IDF 的 Python 环境已经包含它。

握手报告板型、固件、可用后端和数据清单，电脑确认本地数据与板上清单一致后才能执行。结果按批次传输，带有会话标识、顺序和 CRC 校验；电脑保存结果后才确认接收，缓存满时由流量控制暂停发送。运行日志也通过协议传输。缺少结果、批次或完成消息时不能报告通过。重新连接会中止旧传输。不实现自动重传和断点续跑，中断后由开发者重新选择需要运行的用例。

只维护最新协议，runner 和测试固件必须使用相同版本；在线执行和离线复判都不兼容旧格式。更新协议不要求改动 SD 用例包。

`python tests/runner/transport.py PORT` 用于检查握手和连接，不执行模拟器测试。UART 使用 460800 baud，Sila P4 通过 CH340 连接，Tab5 使用 USB-Serial/JTAG。

## 选择本次运行的测试

模块分类放在电脑上的测试索引里，不按模块搬动或复制 ROM。一个测试可以同时覆盖 timer、interrupt 和 HALT，因此索引允许多个模块标签。每项还记录套件、稳定的用例 ID、卡上路径、适用机型、结束条件、判定方式和运行上限。

开发者明确选择模块、套件、操作码或单个用例，不自动根据 Git diff 推测测试范围。runner 在执行前列出选中的用例数和跳过原因。机型不适用、缺少判据或执行器尚未支持的用例不能计为通过，单凭 `.gb` 或 `.gbc` 扩展名也不能确定适用机型。

| 改动范围 | 日常运行范围 |
|---|---|
| 某类 SM83 指令 | 对应操作码的全部单步用例，相关 CPU ROM，以及固定回归 |
| timer、interrupt、HALT | 对应模块的 ROM，以及它们之间的交互测试 |
| PPU、OAM DMA | 相关画面和时序测试，包括与 interrupt、总线访问的交互 |
| APU | 适用于所选机型的声音寄存器、时序和输出测试 |
| MBC、RTC、ROM 访问 | 对应控制器和存储访问测试，加上使用这些路径的整机 ROM |
| 总线、调度器、公共 JIT 后端 | 扩大到所有受影响模块；影响难以界定时跑当前系统的全量测试 |

普通任务的开发和收尾都按上表选择测试，不因任务结束就重跑全部 SM83 单步用例或全部 ROM。只有实际改动影响 CPU 执行时才运行相应单步用例；改动涉及 CPU、公共总线或 JIT 的通用路径，无法界定受影响的操作码范围时，才扩大到全量。需要全量时先完成针对性验证、清理临时计时和故障注入，再运行一次，不在每轮修改后重复。

固定回归由 `tests/runner/rom_suite.py` 的 `REGRESSION` 定义，使用 `run_roms.py --regression` 运行，判据和结束条件与各套件相同。前 15 项在 DMG-C 上运行，后 7 项在 CPU CGB E 上运行。改动 JIT 的代码缓存时，再加 `--small-pool` 跑一次：代码池改得很小，块会不断在代码池和第二级之间搬动，判据不变。

| 用例 | 选择理由 |
|---|---|
| blargg `instr_timing/instr_timing` | 用定时器检查各条受测指令的周期数 |
| mooneye `timer/tima_reload` | 检查溢出后 TIMA 读到 00、随后重载 TMA 的时序 |
| mooneye `timer/tima_write_reloading` | 检查重载期间写 TIMA 的行为 |
| mooneye `interrupts/ie_push` | 检查中断分派时压栈写 IE 对服务的影响 |
| mooneye `halt_ime0_ei` | 检查 HALT 与延迟 EI 的交互 |
| mooneye `oam_dma_restart` | 检查 DMA 重启的时序 |
| mooneye `oam_dma/sources-GS` | 检查不同 DMA 来源区域，覆盖卡带 RAM 和 MBC5 |
| mooneye `ppu/intr_2_mode0_timing_sprites` | 检查 OBJ 对 mode 3 长度和 STAT 中断的影响 |
| mooneye `ppu/stat_irq_blocking` | 检查多个 STAT 条件共用中断线的行为 |
| `dmg-acid2/dmg-acid2` | 比对 BG、窗口和 OBJ 合成的画面 |
| AGE `stat-interrupt/stat-int-dmgC-cgbBCE` | 从一串 NOP 里响应 mode 0、1、2 和 LY=LYC 的 STAT 中断，检查各个中断源升起的时刻 |
| AGE `halt/halt-m0-interrupt-dmgC-cgbBCE` | 用 HALT 等 mode 0 中断，检查 HALT 唤醒比 NOP 里响应晚的那段时间 |
| Gambatte `window/arg/late_wy_FFto2_ly2_1` | mode 3 中途把 WY 写成当前的 LY，窗口在这一行就出现，mode 3 跟着变长 |
| Gambatte `m2int_m0irq/m2int_m0irq_scx3_1` | 在 mode 0 可读之前读 IF，还看不到 HBlank 中断的请求 |
| Gambatte `window/on_screen/wxA6_wy01` | DMG 的 WX 为 166 时，窗口延续到下一行，比较画面和窗口的行计数 |
| `cgb-acid2/cgb-acid2` | 比对 CGB 模式下 BG 属性、两个 VRAM bank、调色板 RAM 和 OBJ 优先级合成的画面 |
| SameSuite `ppu/blocking_bgpi_increase` | 检查各个 PPU 模式下访问调色板 RAM 的结果和索引的自动递增 |
| SameSuite `dma/gbc_dma_cont` | 检查 GDMA 从上一次停下的地址接着传输 |
| SameSuite `dma/hdma_mode0` | 检查 LCD 打开时在 mode 0 启动 HBlank DMA，再中途停止 |
| AGE `speed-switch/spsw-tima-cgbE` | 用四种频率的 TIMA 检查 KEY1 往返切换速度时 DIV 复位和 CPU 停顿的时刻，以及倍速下的定时器和指令周期 |
| AGE `m3-bg-lcdc/m3-bg-lcdc` | CGB 模式下 mode 3 中途切换 BG 的图块数据和图块表，比较画面 |
| AGE `ly/ly-cgbE` | CPU CGB E 在第 153 行和各行开始前后读 LY 的结果 |

这些用例在解释器和 JIT 上都运行，固定回归不能代替完整的操作码测试或里程碑验收。

GBA 的固定回归由 `tests/runner/gba_rom_suite.py` 的 `REGRESSION` 定义，用 `run_gba_roms.py --regression` 运行，判据和结束条件与全量相同。改动 JIT 的代码缓存时再加 `--small-pool` 跑一次，改动 ROM 的分页缓存时再加 `--tiny-cache` 跑一次。

| 用例 | 选择理由 |
|---|---|
| jsmolka `arm/arm` | ARM 指令的结果、标志位和各模式的分组寄存器 |
| jsmolka `thumb/thumb` | Thumb 指令的结果和标志位 |
| jsmolka `memory/memory` | 各区域的镜像、总线宽度和未对齐访问 |
| jsmolka `bios/bios` | BIOS 区的读保护，开机后、SWI 以后和 IRQ 里外读 BIOS 区得到的值 |
| alyosha `timing/prefetch_enable` | 写 WAITCNT 打开预取以后，ROM 里代码的耗时 |
| alyosha `prefetcher/prefetcher_full_arm` | 预取缓冲存满以后停下，CPU 取空以后用 N 访问重新开始 |
| alyosha `prefetcher/prefetcher_branch_thumb_arm_3` | ARM 取指等过预取缓冲以后，分支到正在取的下一个字 |
| nba-hw-test `bus/128kb-boundary` | LDM 和 DMA 跨过 128KiB 边界时的耗时 |
| alyosha `irq/IRQ_sub` | 中断处理返回时马上又有中断，以及中断落在 Thumb 代码里的时刻 |
| alyosha `Interactions/Halt_DMA_IRQ` | HALT 时 VBlank 中断唤醒的时刻和压栈的 PC，期间有重复的 HBlank DMA |
| nba-hw-test `irq/irq-delay` | 从 IWRAM、EWRAM 和 ROM 打开 IME 到进入中断处理的周期数 |
| alyosha `timer/timer` | 开机以后 F/64 预分频的相位 |
| nba-hw-test `timer/reload` | 16 位和 32 位写入重载值的各个时刻 |
| alyosha `DMA/DMA_pause_timing_mid_1` | HBlank DMA 在立即 DMA 的读和写之间接管 |
| nba-hw-test `dma/latch` | DMA 的数据 latch 和 open bus |
| alyosha `fifo_dma/fifo_5` | FIFO 写入 8 个字以后的 DMA 请求，以及 FIFO DMA 让 CPU 停下的时间 |
| gbx `ppu/bg_affine_mode2` | 仿射背景的旋转缩放、参考点和回绕，比对画面 |
| gbx `ppu/obj_affine` | 仿射 OBJ 和 double size，比对画面 |
| gbx `ppu/window` | 两个窗口、OBJ 窗口和窗口外，比对画面 |
| gbx `ppu/blend_alpha` | alpha 混合、半透明 OBJ 和系数饱和，比对画面 |
| gbx `ppu/raster_dma` | 用 HBlank DMA 逐行修改滚动、仿射参数、BLDALPHA 和 WIN0H，比对画面 |
| jsmolka `save/sram` | 按 ID 字符串判断的 SRAM |
| gbx `save/flash64_noid` | 没有 ID 字符串时按第一次访问判断的 64KiB Flash，包括 ID、擦除和写入 |
| gbx `save/eeprom8k_noid` | 没有 ID 字符串时按第一次位流判断的 8KiB EEPROM |
| gbx `rom/rom32_cpu` | 32MiB ROM 经过分页缓存在三个镜像区的读取，以及从 16MiB 以上取指 |

里程碑验收的全量运行覆盖当前机型、当前实现范围内的全部适用用例，并单独列出未支持和需人工检查的部分。不能把只跑了相关模块的结果报告成整套通过。

### 每个任务的双后端优化

每个任务完成前，解释器和 JIT 都要完成针对本任务相关路径的性能分析、优化和复测。不能只优化其中一个后端，也不能用两个后端结果一致代替性能分析。共用总线、调度器或设备代码的优化，要核对它对两个后端的影响，不能用一边的提速掩盖另一边的性能回退。

功能通过针对性检查后，选少量能覆盖本次改动的代表性 ROM，记录两个后端各自的优化前数据，再根据瓶颈修改实现并复测。比较时保持板型、芯片修订、主频、构建配置、ROM、输入、模拟时长和核安排一致。JIT 的编译、代码发布与缓存命中后的执行开销分开分析，最终速度仍按完整运行计算。代码体积和内存占用也要记录，不能用未说明的资源增长换取速度。

只保留正确且有实测收益的改动。没有收益的尝试撤销，并在报告里说明；某个后端没有可保留的优化时，也要给出分析和实测依据，不能只写“无需优化”。不通过缩短规定的模拟时长、跳过设备行为或改用未实现功能的 ROM 提高测速结果。

完成标准包括两个后端各自的优化前后数据、瓶颈和剩余限制。临时计时、实验分支和故障注入在最终验证前清理。性能优化不额外触发全量测试，验证范围仍按本节的规则确定。

### 性能分析版固件

`<板子>-test-profile` preset 编出打开 `GBX_PROFILER` 的 release 测试固件，只在做性能分析时构建，不属于日常构建、测试和验收。它的耗时包含采样本身的开销，不能当作测速结果。其他构建不包含任何分析代码。

运行整机 ROM 时，定时器中断定时记录被打断的程序地址，按代码所在的区域统计，JIT 任务还附带 `Sm83Jit` 的计数器；GBA 的 ROM 另外记下 CPU 所在的核等渲染线程的次数和时长。`run_roms.py` 和 `run_gba_roms.py` 把结果按 ROM 写进报告目录的 `profile.txt`，再用 `tools/profile_report.py` 汇总，见[工具说明](../tools/README.md)。

```sh
eim run 'idf.py --preset sila_p4-test-profile build' v6.1
eim run 'idf.py --preset sila_p4-test-profile -p PORT -b 460800 app-flash' v6.1
python3 tests/runner/run_roms.py PORT --rom blargg/cpu_instrs/cpu_instrs \
  --report <report-directory>
```

### 运行 SM83 单步用例

`tests/runner/run_sm83.py` 使用 Python 3.11 或更新版本。runner 在连接前逐条核对套件与用例包，再核对板子返回的清单校验值，任一步不一致都不发送执行请求。烧录测试固件后，运行修订套件的全部用例：

```sh
python3 tests/runner/run_sm83.py PORT --all \
  --sm83-source build/sm83-stop-pc-v1
```

`--all` 可以换成按操作码或单个用例选择，`--list` 只核对数据并保存选择清单，不连接板子。

不带 `--baseline` 时，测试同时检查解释器是否满足套件判据，以及 JIT 与解释器的实际结果是否一致。解释器全部通过且执行完整的报告可以作为后续验证基准。只改 JIT 时，可以用 `--baseline <report-directory>` 引用这份报告，省略重复的套件判定，仍然实际执行两个后端并比较结果。基准必须覆盖本次选择的用例，并匹配数据清单、板型、芯片修订、主频、构建配置、编译器、SDK、解释器和共用测试代码，JIT 源码不参与基准标识；不适用时 runner 在发送用例前报错。基准不把解释器输出保存成替代套件的 golden。

默认的 `--mode device` 在板上比较 CPU 状态、模拟周期数、总线采样，以及套件指定和两个后端实际写入的内存地址，失败时返回两个后端的实际结果，由电脑定位第一处差异。需要进一步排查出错用例时，才用 `--mode diagnostic` 回传所选用例的无损实际结果，由电脑独立判定。所有 guest 内存写入必须经过总线，才能覆盖实际写入的地址。

报告默认保存在 `build/test-reports/<UTC 时间>/`，记录条件、版本、选择范围、判定和失败详情。`tests/runner/rejudge_sm83.py` 不连接板子，按当前判据重新判定已有报告；只有保存了实际结果的用例能够复判，其余标为 `UNVERIFIED`，不能据此报告全量通过。`--rerun-failed <report-directory>` 只重跑上次失败或观测超限的用例。尚未收到结果的用例计入 `not_run`，不计为失败或通过。

每条用例在两个后端上各执行一条指令，CPU、内存和总线状态逐条复位。HALT、STOP 和锁死状态按用例给出的观测长度继续采样，超限报告为 `TIMEOUT`。收到重启标志、帧损坏或批次缺失时终止运行，不继续累计通过。

### 运行 ARM7TDMI 单步用例

`tests/runner/run_arm7.py` 的用法和 SM83 的 runner 相同，`--data` 默认是 `tests/sdcard/gba`：

```sh
python3 tests/runner/run_arm7.py PORT --all
```

`--all` 可以换成 `--file arm_bx` 选整个文件，或者用 `--case v1/arm_bx.json.bin::17` 选单个用例。用例 ID 是源文件的路径加上用例在文件里的序号，从 0 开始。runner 从 `refs/tests/arm7tdmi` 读取预期值。连接前先核对本地套件的来源版本、说明和许可，以及 `--data` 下的 `cases.bin` 都和清单一致，再按 `cases.bin` 的索引把源文件对应到板上的文件序号；连接后核对板子返回的清单校验值。每个文件发出第一个请求之前，runner 把选中的用例按打包的格式重新编码，和 `cases.bin` 里的记录逐字节比较，这一步在后台线程里提前做，和板上执行前一个文件同时进行。任何一步不一致都不再发送执行请求。`--list` 不连接板子，同步比较全部选中的用例。`--mode diagnostic`、`--rerun-failed` 和报告目录的规则都和 SM83 相同，离线复判用 `tests/runner/rejudge_arm7.py`。

判定按套件的 [README.md](../refs/tests/arm7tdmi/README.md) 比较 R0 至 R15、各模式的分组寄存器、CPSR、5 个 SPSR、流水线和下一次取指的 N/S，以及每次总线访问的类型、宽度、地址、数据和 N/S，访问的先后顺序也要一致。R15 不比较 ARM 状态下的 bit 1–0 和 Thumb 状态下的 bit 0。SWP 的 Lock 不比较；`cycle` 也不比较，因为 README 说它没有正确记录，报告里记下 `cycle` 不同的用例数。没有定义结果的 R15 写入，以及 CP14 的 MRC 和 MCR，照常执行，但是不和套件比较，直接计为通过，报告里分别列出条数。这两类按编码认定，依据见 GBA [任务 1-3](plans/gba/task-1-3.md#开发核对结果)。

每个用例先由解释器执行并按套件判定，再从相同的初始状态由 JIT 执行，块长限制为一条指令。JIT 的结果和解释器比较全部 40 个状态字，以及每次总线访问的全部字段，Lock 和 `cycle` 也比较。不和套件比较的两类用例也要比较两个后端。回退到解释器执行的用例不算 JIT 通过，报告按文件列出由生成的代码执行和回退的条数，以及回退的原因。

板上判定时，两个后端都通过的连续用例每 128 条以内合成一个字节回传；解释器失败、两个后端不一致或者发生回退的用例附带两个后端的实际结果。用例包由另一个核上的读卡任务按顺序读出（ESP32-C5 只有一个核，读卡任务和执行在同一个核上），读卡和执行、回传同时进行。报告的 `device_time` 分开记下读卡、等待读卡、解释器执行、JIT 的编译、发布和执行，以及回传的时间。

## 运行整机 ROM

索引在 `tests/runner/rom_suite.py`，记录稳定 ID、卡上路径、机型、运行模式、模块标签、依赖任务、结束条件、判据、时间上限、按键脚本、RTC 初始状态和即时存档测试的存档时刻。它收录验收用的 ROM，以及只记入报告的 ROM。测试索引不包含 ROM 数据。

同一个 ROM 可以分别登记为 DMG-C 和 CPU CGB E 的用例，CGB E 用例的 ID 以 `cgb-e/` 开头。CGB 运行 CGB 标志第 7 位置位的 ROM 时用 CGB 模式，其余的 ROM 用兼容模式（Pan Docs “0143 — CGB flag”），runner 校验数据时按卡带头核对索引里的模式。CGB E 只选套件源码或说明中注明适用于 CGB E、或者不区分 CGB 修订的用例，各项的选择依据写在 `rom_suite.py`。Gambatte 的 cgb04c 期望值来自 CPU CGB C，而 AGE 显示这部分时序在 CGB C 和 E 上不同，所以 CGB E 的 Gambatte 用例只记入报告，`--all` 也不选它们。

```sh
python3 tests/runner/run_roms.py PORT --all
```

`--all` 可以换成按套件、模块或单个 ROM 选择，`--model` 只保留一种机型的用例。没有实现所需设备或判定方式的 ROM 标为 `UNSUPPORTED`，不下发执行，也不测速。混合选择的报告用 `PARTIAL` 表示覆盖不完整，不能把它当作整个里程碑通过。

只有全量验收 ROM 都完成，或固定回归名单完整执行时，才计算相应的时间预算，并且要求使用路线图规定的板子、测试固件 release 构建和 `isolated` 调度。预算采用整次运行的墙钟耗时，包含本地校验、读卡、结果回传和本次选中的只记入报告的 ROM。诊断运行不用于验收测速。

板子从 SD 卡把 ROM 完整载入 PSRAM，并对实际字节做 SHA-256 校验，不能只凭清单文件相同就执行。默认的 `--schedule isolated` 逐个运行解释器和 JIT，CPU 都在 core 1，渲染、混音和传输在 core 0。ESP32-C5 只有一个核，这些都在 core 0 上。`--schedule concurrent` 同时运行两个系统，用于检查并发运行是否改变结果。

每个 ROM 按套件规定的结束条件运行，判据见“通过判据”一节。按规定时长取帧的套件，跑到规定时长是正常结束；其他判据达到模拟上限仍然不能算通过。两个后端都要满足判据，并且 CPU、模拟时间、执行指令数、各块内存摘要、串口输出、RTC 和 PCM 摘要都要一致。只用于排错、没有参考图的 ROM 保存实际帧并记为 `MANUAL`，由人查看，不计入通过。

按键脚本和 RTC 初始状态随执行请求发给板子，测试固件在每个脚本时刻之后的第一个指令边界提交，所以两个后端的提交时刻相同。各 ROM 的按键、取帧时刻和依据写在 `rom_suite.py`。

每个 ROM 都生成立体声 PCM，测试固件计算摘要，runner 比较两个后端的 PCM 帧数和摘要，丢帧时判为 `PCM_DROPPED`。PCM 摘要只用来比较两个后端，不保存成标准答案。

画面比较需要 Python 的 Pillow。runner 校验参考图的清单摘要、原机分辨率和颜色值，然后逐像素比较，不做容差、缩放或颜色近似。GB 模式的帧是灰阶等级，参考图使用套件规定的 `#FFFFFF`、`#AAAAAA`、`#555555` 和 `#000000`。CGB 模式和兼容模式的帧是 BGR555，runner 按套件说明用 `(X << 3) | (X >> 2)` 把每个 5 位分量换成 8 位，参考图的每个分量也必须是这样换出来的值。失败时报告保存实际帧和差异图。只记录结果的用例遇到不合要求的参考图时报告 `UNJUDGEABLE_REFERENCE`，不计为通过；其他用例遇到无效参考图时在执行前报错。

报告的 `summary.json` 保存每个后端的模拟时间、墙钟耗时、速度、CPU 状态、内存摘要和 JIT 计数。计时包括冷缓存编译、CPU 执行，以及等待已提交的渲染和声音任务完成，不包括读卡和回传。达到模拟时间上限、CPU 暂停、设备不可用、板子重启和通信异常分别记录，缺少任一后端结果或完成消息都不能通过。

```sh
python3 tests/runner/run_roms.py PORT --rerun-failed <report-directory>
python3 tests/runner/run_roms.py PORT \
  --rom mooneye-test-suite/acceptance/bits/reg_f --diagnose
```

失败重跑要求相同的数据清单。锁步诊断只接受一个已支持、用寄存器判定的 ROM：两个 CPU 后端依次执行相同数量的指令，暂停后比较状态和写入的内存。JIT 的块划分、寄存器分配和标志位计算与正常运行相同。报告给出第一个出错块的入口和指令范围，以及第一处不同的访存或寄存器。记录溢出不能判为通过。

### 运行 GBA ROM

测试固件默认关闭 GBA 降低精度的宏 `GBX_GBA_RELAXED_TIMING`，两个后端按全部精度运行和比较。宏打开时会有一批计时类的测试不通过，清单在 GBA [任务 6-1](plans/gba/task-6-1.md)；要再看宏打开时的结果，另建一个构建目录，加 `-DGBX_GBA_RELAXED_TIMING=ON`，例如：

```sh
eim run 'idf.py --preset sila_p4-test-release -B build/sila_p4-test-relaxed -DGBX_GBA_RELAXED_TIMING=ON build' v6.1
```

GBA 的索引在 `tests/runner/gba_rom_suite.py`，记录稳定 ID、卡上路径、模块标签、依赖任务、结束条件、判据、报告结果的寄存器、时间上限、按键脚本、参考帧和即时存档测试的存档时刻。依赖的任务还没有完成的 ROM 标为 `UNSUPPORTED`，不下发执行。

```sh
python3 tests/runner/run_gba_roms.py PORT --all
```

`--all` 可以换成 `--regression`、`--suite`、`--module`、`--rom` 或 `--rerun-failed`。板子把 ROM 经过分页缓存从卡上读出，同时计算 SHA-256，和清单不同就不运行。默认的 `--mode paired` 让两个后端依次运行同一个 ROM，CPU 在 core 1，渲染和混音在 core 0。`--mode concurrent` 让两个后端同时运行，解释器在 core 0，JIT 在 core 1，各自的渲染和混音在另一个核上，每个后端用自己的分页缓存，用来检查并发运行是否改变结果。两个后端都要满足判据，结束时的 CPU、模拟时间、各块内存和存档芯片的摘要、读出的文字、最终帧、PCM 摘要和 RTC 都要一致；JIT 除了 VRAM、调色板 RAM 和 OAM 里的代码以外，不能把任何代码交给解释器执行。两个后端都不计执行的指令数，所以不比较指令数。

每个 ROM 都按 `--rate` 生成立体声 PCM，默认每秒 48000 帧，测试固件在每段运行之间取走 PCM 并计算摘要，这部分时间不计入速度。runner 比较两个后端的 PCM 帧数和摘要，丢帧时判为 `PCM_DROPPED`。PCM 摘要只用来比较两个后端和两种调度，不保存成标准答案。

`--tiny-cache` 把分页缓存减到两页，每个 ROM 开始前清空，页面会不断换进换出；`--small-pool` 把 JIT 的代码池减到 16KiB 和 64 个块。两者都不改变判据。

`--mode lockstep` 让两个后端同时运行，用来找出不一致的地方：JIT 每进入一次原生代码，解释器就运行到相同的指令数和停止点，比较 CPU 和模拟时间，每段运行结束时再比较内存。内存不同时从开机重放这一段，在每一步比较不同的区域。报告给出第一处不同的那一步和它前面的状态。默认这时块之间不连接，也不经过分发表，每次进入原生代码只运行一个块；`--linked` 保留连接，在回到 C++ 时比较，用来查连接和分发表引起的问题。

`--game roms/gba/<文件>` 测量游戏的速度：ROM 放在卡上的 `roms/` 下，电脑上的仓库里同一路径也要有这个文件，两个后端各运行 `--frames` 帧。没有判据，两个后端仍然要以相同的状态结束。游戏不属于测试数据，也不提交。

### 存档和读档

`--states` 运行索引里带存档时刻的 ROM 的存档和读档测试。每个存档时刻记录模拟时刻、要等待的条件和最长的等待时长，比如 OAM DMA 进行中、mode 3 中途有寄存器写入、HALT 期间、EI 还没有生效、CGB 倍速和 HBlank DMA 进行中；选择的理由写在 `rom_suite.py` 的 `STATES` 里。

```sh
python3 tests/runner/run_roms.py PORT --states
```

板子先在两个后端上不中断地运行这个 ROM，再在每个存档时刻、条件全部成立的那个指令边界取状态。超过等待时长条件仍然不成立，或者 ROM 在存档之前已经结束，都报告为失败，不改在别的时刻存档。每份状态先在存档的后端上读，再在另一个后端上读，所以四种后端组合都会运行。

读档以后的运行按套件原来的判据判定，结束状态还要和同一个后端不中断运行的结果相同，包括 CPU、模拟时间、指令数、内存摘要、最终帧、PCM 摘要、RTC 和串口输出。不中断运行的结果只在这一次运行里比较，不保存成标准答案。判据不通过、结束状态有任何一项不同、出现 JIT 回退或者丢失 PCM 帧，这个 ROM 都不能通过。

GBA 的 ROM 也用 `--states`，只在 `--mode paired` 下运行：

```sh
python3 tests/runner/run_gba_roms.py PORT --states
```

存档时刻和选择理由写在 `gba_rom_suite.py` 的 `STATES` 里，条件包括重复的 HBlank 或 FIFO DMA 已经推进了内部地址、HALT 期间、一行中途有寄存器写入、FIFO 不空、预取缓冲不空、Flash 或 EEPROM 正在执行命令，以及 RTC 正在传输。流程和判定与 GB 相同，比较的项目和两个后端之间比较的相同。结束条件是空循环的 ROM 只在模拟时间跨过 65536 的整数倍时检查空循环，所以读档以后的运行和不中断的运行在同样的时刻检查。runner 把状态的大小、取状态和恢复的耗时、另一个核上编码和解码的耗时，以及读档以后前两帧的耗时和编译的块数汇总在 `summary.json` 的 `state_timing` 里。

## 显示图案检查

显示后端的图案检查编入普通的测试固件，只有 runner 请求时才运行，不需要 SD 卡上的测试数据：

```
python3 tests/runner/run_display.py PORT
```

检查使用独立的内存缓冲，不启动 LVGL，也不向面板提交图像。期望值由已知的颜色和坐标构成，覆盖四种旋转方向、矩形边界、未修改区域、最近邻复制、双线性插值、脏区域的合并，以及 SPI 屏不经帧缓冲、按行分段发送的游戏画面，分段发送分别从 BGR555 和灰阶的帧取像素，并同时检查常规调度路径和直接硬件路径。CPU 显示后端不具备的硬件组记为 SKIP。用例在 `tests/display/backend.cpp`。缺少结果、错误或设备重启都不能判为通过。内存图案检查不代表实际面板的方向、撕裂或残影已经检查。

## 音频水位检查

`python3 tests/runner/run_audio.py PORT` 运行混音器的水位检查，不需要 SD 测试数据。它暂时阻止混音，检查已提交的音频是否计入节流水位、是否仍不允许播放线程读取，再检查混音、读取、复位和缓冲溢出后的数量。单核固件没有混音的工作线程，交出声音任务时就混音，没有“已提交、还不能读”的状态，所以这一项记为 SKIP。期望帧数由采样率、模拟时长和速度修正计算，不使用录制的 PCM 作为标准答案。用例在 `tests/audio/mixer.cpp`。解释器和 JIT 的整机音频一致性由 ROM 测试检查。

## JIT 后端检查

`tests/runner/run_jit.py` 运行板上的原生 RV32 检查，以及少量自带的 SM83 指令序列，分别检查解释器和 JIT 的块间控制流、映射访存、缓存淘汰、两级代码缓存和代码改写。几段自带的 ARM 和 Thumb 程序检查多条指令的块、条件执行的访存和乘法、ARM 和 Thumb 之间的切换、模式切换、异常的进入和返回，以及块之间的控制流：每段程序在平坦的 RAM 上分别由解释器和 JIT 运行，比较 CPU 状态、内存、总线访问和执行的指令数；总线周期的预算从 1 逐个加到全长，每个预算都比较一次，让块在每个指令边界退出。这些检查不代替单步套件，不需要 SD 卡上的测试数据。检查代码在 `tests/device/` 的 `jit_test.cpp`、`arm7_blocks.cpp` 等文件里。

```sh
python3 tests/runner/run_jit.py PORT
```

`--group` 可以只运行其中一组，比如 `memory` 只验证映射访存、缓存删除、外设及卡带/DMA 路径，`extensions` 只运行扩展指令的检查。固件启用的扩展必须得出 ISA 手册算出的期望值；没有启用的扩展，得出期望值或者触发非法指令异常都算通过。预期值依据固定版本的 [RISC-V ISA 和 psABI](../refs/README.md#文档)。成功时显示 `BACKEND PASS`，同时明确单步套件没有运行。缺少任何结果或完成消息都不能算通过。

编码另用 ESP-IDF 工具链的汇编器和链接器独立核对，关闭松弛优化。PC 上只生成并比较机器码，不执行它：

```sh
cmake --preset headless-debug -S host
cmake --build build/host-headless-debug --target gbx_rv32_encoding
eim run 'python tools/check_rv32_encoding.py build/host-headless-debug/bin/Debug/gbx_rv32_encoding --report build/rv32-encoding-check' v6.1
```

报告目录必须尚不存在。汇编器生成的参考结果保存在报告里，不作为仓库中的 golden。

## 执行和结果传输

电脑负责选择用例、确认数据版本和保存报告，板子负责从 SD 卡读取并执行。以后接入的套件也按各自的外部判据判定。

每个用例都从相同的初始状态分别在解释器和 JIT 上执行，两次之间复位 CPU、内存、设备和相关的执行状态。宿主执行时间不要求一致，没有运行的后端标为 `NOT RUN`。

ROM 按模拟时间或套件规定的结束条件运行，不为等待真实帧率而休眠。自动测试可以关闭屏幕刷新和喇叭播放，但仍然执行用例需要的 PPU、APU 和时序逻辑，保留待判定的帧或音频。

每个用例同时有模拟运行上限和电脑侧的墙钟超时。超时、板子重启、通信错误和测试断言失败分别报告。报告记录固件版本及工作区是否有未提交的改动、板型、构建 preset、数据清单校验值、模拟机型、执行后端和耗时，以便复现。

## 通过判据

各套件的 `game-boy-test-roms-howto.md` 和原始说明是判据来源，不能给所有 ROM 套用同一个结束条件。

| 测试 | 判定方式和来源 |
|---|---|
| SM83 单步 | 按 [README.MD](../refs/tests/sm83/README.MD) 检查预期寄存器、RAM 和总线活动。忽略 IE 和 IME，明确给出的 `final.ei` 仍需比较；未定义的总线值不当成必须匹配的数值 |
| ARM7TDMI 单步 | 按 [README.md](../refs/tests/arm7tdmi/README.md) 检查最终状态和总线访问的顺序，`cycle` 不比较；R15 的低位、Lock 和不比较的用例见[运行 ARM7TDMI 单步用例](#运行-arm7tdmi-单步用例) |
| Mooneye | 对使用自动测试协议的用例，在测试结束的 `LD B,B` 处检查 B、C、D、E、H、L 是否为 3、5、8、13、21、34，运行上限是 120 秒模拟时间；机型限制和人工检查项见[套件说明](../refs/tests/game-boy-test-roms/mooneye-test-suite/game-boy-test-roms-howto.md)。串口输出不是判据 |
| Blargg | 按具体 ROM 的完成信号和判据检查；合集提供了按模拟时间取帧比对的[说明](../refs/tests/game-boy-test-roms/blargg/game-boy-test-roms-howto.md)。不能假定所有 ROM 都输出同一种串口结果 |
| dmg-acid2、cgb-acid2、cgb-acid-hell、Mealybug | 在各自说明规定的结束点取得原始帧，按对应机型的参考图和调色方式比对，见 [dmg-acid2](../refs/tests/game-boy-test-roms/dmg-acid2/game-boy-test-roms-howto.md)、[cgb-acid2](../refs/tests/game-boy-test-roms/cgb-acid2/game-boy-test-roms-howto.md)、[cgb-acid-hell](../refs/tests/game-boy-test-roms/cgb-acid-hell/game-boy-test-roms-howto.md) 和 [Mealybug](../refs/tests/game-boy-test-roms/mealybug-tearoom-tests/game-boy-test-roms-howto.md) |
| SameSuite | 在 `LD B,B` 处检查 B、C、D、E、H、L 是否为 3、5、8、13、21、34，运行上限和 Mooneye 相同，见[合集说明](../refs/tests/game-boy-test-roms/same-suite/game-boy-test-roms-howto.md) |
| AGE | `m3-bg-*` 在 `LD B,B` 处取帧，和对应机型的参考图比较；其他用例在 `LD B,B` 处检查 B、C、D、E、H、L 是否为 3、5、8、13、21、34，见[合集说明](../refs/tests/game-boy-test-roms/age-test-roms/game-boy-test-roms-howto.md) |
| Gambatte | 按[合集说明](../refs/tests/game-boy-test-roms/gambatte/game-boy-test-roms-howto.md)从开机状态运行 15 帧，再取帧。文件名带 `_out` 的用例比较左上角的数字，数字的 8×8 图案取自 `refs/tests/gambatte-testrunner/testrunner.cpp`；带 PNG 的用例和 PNG 整帧比较，颜色按 `testrunner.cpp` 的规则只比较每个分量的高 5 位。期望值按 `testrunner.cpp` 的 `main()` 从文件名取出：dmg08 用于 DMG-C，cgb04c 用于 CGB E |
| GBMicrotest | 按[说明](../refs/tests/game-boy-test-roms/gbmicrotest/game-boy-test-roms-howto.md)的运行时长检查 `0xFF82`，`0x01` 为通过、`0xFF` 为失败；不能用 `0xFF80 == 0xFF81` 代替判定 |
| jsmolka-gba-tests | 运行到 idle 循环，检查 ROM 传给 `m_test_eval` 的寄存器为 0，多数是 `r12`，`thumb` 是 `r7`；`ppu` 下的 ROM 和参考帧比较，见 GBA [任务 1-1](plans/gba/task-1-1.md#用例选择) |
| alyosha-gba-tests | 运行到 idle 循环，按模式 4 在第 76 行画出的文字判定：从第 56 列开始是“All tests passed”，从第 60 列开始是“Failed test NNN”；没有画文字的按 `r12` 为 0 判定。只能看画面的 ROM 和参考帧比较或者交给人看，见 GBA 任务 1-1 |
| nba-hw-test | 从 BG0 的图块表读出打印的文字，每项要打印 `PASS`，没有 `FAIL`，打印总结的 ROM 还要有 `congratulations!`；`ppu/status-irq-dma` 用按键选择子测试，见 GBA 任务 1-1 |
| GBA 自写 ROM | `tests/gba/roms/` 下的 ROM 按 jsmolka 的约定在 idle 循环检查 `r12`，PPU 场景和参考帧比较，见 GBA [任务 1-2](plans/gba/task-1-2.md#核对结果) |
| 其余套件 | 接入前逐项确认适用机型、结束条件和判据；人工测试不加入自动通过统计 |

画面只和套件提供的参考图比较，使用其规定的灰阶或颜色。比较对象是原机分辨率的模拟输出，不是经过板子缩放、调色或拍照的屏幕。没有精确参考图的测试不能自录 golden image，也不能用当前解释器的输出代替硬件预期。

GBA 的画面还可以和 `tests/gba/frames/` 的参考帧比较。参考帧由固定版本的 NanoBoyAdvance 生成，生成的条件和核对方法写在 `tools/gba_reference_frames.py` 开头。ROM 跳过 BIOS 的开机流程，从复位开始运行，取第 60 次进入 VBlank 时完成的那一帧。帧是 BGR555，和 CGB 一样把每个分量换成 `(X << 3) | (X >> 2)` 以后逐像素比较。参考帧生成时已经确认第 30 至 90 帧都相同，所以帧号的起点和参考模拟器差几帧也不影响比较。

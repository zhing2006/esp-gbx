# 产品设计

esp-gbx 是运行在自制掌机上的 GB、GBC 和 GBA 模拟器，主控用 ESP32-P4 和 ESP32-S31。掌机从 SD 卡直接加载 ROM，不需要先在电脑上转换。项目要让游戏以原速运行、音画同步、卡带存档可靠，并做出可以复用的 RISC-V JIT。

模拟核心、解释器、JIT 和设备模型都依据硬件规格和指令集文档从零编写，不采用、也不移植现有模拟器的实现。第三方的测试用例、测试 ROM 和它们的预期结果可以拿来检查指令、设备时序和画面是否正确。

## 范围

首期只做单机游戏，包括：

- 按开发路线依次支持 GB、GBC 和 GBA 游戏。
- 游戏自己的卡带存档，这是必做功能。
- 给宝可梦等带时钟的卡带用的 RTC。
- 即时存档。
- 主界面和暂停菜单。启动游戏之前，在主界面选游戏和修改设置。游戏中按菜单键暂停，在暂停菜单里修改设置、即时存档和读档、退出游戏，并查看存档状态。设置包括音量、背光亮度、执行后端、调色板、GBC 画面的颜色校正和游戏缩放方式，游戏运行中不能修改的设置项和当前平台不支持的设置项显示为灰色。设置写在 SD 卡上，重启以后保留。主界面和暂停菜单都能打开故障列表，查看最近的故障，并熄灭 ERR 灯。

联机、红外、RTC 以外的特殊卡带外设、电量显示和休眠都放到后续。

## 开发路线

| 阶段 | 目标 | 内容 |
|---|---|---|
| 第 0 步 | GB 黑白游戏 | CPU、PPU、APU、定时器、中断、OAM DMA、卡带、卡带存档和即时存档，在解释器和 SM83 JIT 上同步实现，两边对照 |
| 第 1 步 | GBC 彩色游戏 | 在 GB 核心上增加彩色调色板、VRAM 和 WRAM 分组、倍速和 VRAM DMA；保留 GB 模式，并验证 CGB 的 DMG 兼容模式 |
| 第 2 步 | GBA | 实现 ARM7TDMI 的 ARM 和 Thumb 指令以及设备模拟，重点是 JIT、图形合成和内存优化 |

各阶段的任务划分和完成要求见[路线图](roadmap.md)。GB／GBC 的 SM83 和 GBA 的 ARM 各写一个前端，代码生成、代码缓存和调度这些设施在合适的地方共用。

## 模拟的原机

下表的内存容量不含卡带。GB／GBC 的 CPU 频率按 T-cycle 计，不等于每秒执行的指令数。

| 原机 | CPU | 工作内存／显存 | 显示 | 声音 |
|---|---|---|---|---|
| GB | 8 位 SM83，约 4.19MHz | 8KiB／8KiB | 160×144，4 级灰度 | 4 声道，立体声 |
| GBC | SM83，约 4.19MHz，倍速时约 8.39MHz | 32KiB／16KiB | 160×144，15 位 RGB 调色板 | 4 声道，立体声 |
| GBA | ARM7TDMI，约 16.78MHz，ARM／Thumb | 片内 32KiB + 外部 256KiB／96KiB | 240×160，15 位色 | 4 个传统声道 + 2 路 Direct Sound |

GB 和 GBC 的参数见 Pan Docs “[Specifications](https://gbdev.io/pandocs/Specifications.html)”，GBA 的参数见 [GBATEK](https://problemkaputt.de/gbatek.htm)。

### GB

要模拟的部件包括 CPU、开机状态、内存映射、MBC、PPU、APU、定时器、中断、按键和 OAM DMA。MBC 先支持无 MBC 和 MBC1、MBC2、MBC3、MBC5。重点验证 HALT、中断延迟、DIV 和 TIMA、VRAM 和 OAM 的访问限制，以及 OAM DMA 期间的总线行为。

OAM DMA 会占用源总线和 OAM，游戏通常会把启动 DMA 的例程复制到 HRAM 里执行，以避开总线冲突（Pan Docs “[OAM DMA Transfer](https://gbdev.io/pandocs/OAM_DMA_Transfer.html)”和 [GB Complete Technical Reference](https://gekkio.fi/files/gb-docs/gbctr.pdf) 第 8.1 节）。所以 JIT 必须能执行 HRAM 中的代码，并且在这段代码被改写以后让对应的缓存失效。

### GBC

GBC 在 GB 核心上增加彩色调色板、VRAM 和 WRAM 的分组、BG 和 OBJ 的优先级、GDMA／HDMA 以及倍速切换。按 Pan Docs “[CGB Registers](https://gbdev.io/pandocs/CGB_Registers.html)” 中 KEY1 一节的说明，倍速模式下 CPU、定时器和 DIV、串口以及 OAM DMA 的速度翻倍，但是 PPU、HDMA 和声音的时序保持不变。切换速度时 CPU 会停顿 2050 个 M-cycle，期间 DIV 不计数，这些也要模拟。

GB 模式和 CGB 的 DMG 兼容模式（Pan Docs 中的 Non-CGB Mode）要分别验证。能在 GB 上运行的游戏，也就是黑白游戏和 CGB 标志为 0x80 的游戏，由设置里的调色板决定运行模式。选灰阶或 DMG 绿时相当于卡带插在 GB 上，按 GB 模式运行。选彩色时相当于插在 GBC 上，0x80 的游戏按 CGB 模式运行，黑白游戏按兼容模式运行。CGB 标志为 0xC0 的游戏只能在 GBC 上运行，总是按 CGB 模式运行。兼容模式和 CGB 的 boot ROM 一样，按 ROM 头自动选择兼容调色板（Pan Docs “Compatibility palettes”）。CGB 模式和兼容模式的画面可以选择是否做 LCD 颜色校正。

### GBA

GBA 的 CPU 和设备时序一起实现。CPU 部分覆盖 ARM 和 Thumb 指令、条件执行、标志位、CPU 模式和分组寄存器、异常进入和 BIOS 行为，还要处理访存等待、预取，以及未对齐访问、open bus 这类特殊的访存语义。设备包括背景的图块模式和位图模式、OBJ、仿射变换、alpha 混合、声音 FIFO、4 个 DMA 通道、4 个定时器、中断、按键和卡带存档。BIOS 用的是用户从原机转储的文件，放在 SD 卡的 `bios/` 下，按校验值核对。CPU 照常执行 BIOS 的代码，JIT 也翻译它，所以中断入口、SWI 和 BIOS 区的 open bus 都和原机一致。开机不运行 BIOS 的开机流程，直接设置 BIOS 交出控制时的状态。卡上没有有效的 BIOS 时，GBA 游戏不能启动。卡带上的 RTC 通过 GPIO 访问（GBATEK “GBA Cart Real-Time Clock (RTC)”），按模拟时间推进，做法和 GB 的 MBC3 RTC 相同。

## 目标平台

两个平台从架构设计开始就同等支持，各自适配自己的硬件能力。但是 ESP32-S31 的片内 SRAM 放不下 GB 和 GBA 两套执行核心，所以它的固件只编入一个系统：现在的板子只跑 GBA，GBA 的解释器留在 flash，片内 RAM 给 JIT 代码池，见[路线图](roadmap.md)的里程碑 7。两块板子的到货时间和 SDK 的支持进度可以不同，不要求同一天交付。

| 项目 | ESP32-P4 v3.x | ESP32-S31 |
|---|---|---|
| HP CPU | 双核 RISC-V，最高 400MHz | 双核 RISC-V，最高 320MHz |
| L1 指令缓存 | 16KB | 每核 32KB |
| HP 片内内存 | 768KB L2MEM + 8KB SPM | 512KB SRAM |
| PSRAM 接口上限 | 16 位 DDR，250MHz | 8 位 DDR，250MHz |
| 目标配置 | ESP32-P4NRW32X，32MB PSRAM | ESP32-S31NRV32，32MB PSRAM |

表中是硬件规格的上限。S31 的 PSRAM 接口只有 P4 的一半宽，所以理论峰值带宽大约是 P4 的一半，但是不能拿它直接推算模拟器的速度。P4 的 L2MEM 还要划出一部分给 L2 缓存、系统和固件使用。来源：[P4 数据手册](https://documentation.espressif.com/esp32-p4_datasheet_en.html)、[S31 数据手册](https://documentation.espressif.com/esp32-s31_datasheet_en.html)、[S31 接口规格](https://www.espressif.com/en/products/socs/esp32-s31)、[P4 内存分配说明](https://documentation.espressif.com/en/PCN202600801_ESP32-P4_Chip_Revision_v3.2_Upgrade_Chip_Revision_v1.3_Demand_Collection_and_EOL_Plan_Description.html)。

本文参考的 S31 数据手册是 Pre-release v0.5，P4 手册也标着预发布，所以开发时要固定芯片修订、SDK 版本和勘误清单。SDK 用 ESP-IDF v6.1。在这个版本里 ESP32-S31 还是 preview target，选择它时要给 `idf.py` 加 `--preview`。

P4 旧版 v1.x 最高只有 360MHz，不作为本项目的性能基线。v3.x 把封装引脚 54 改成了 VDD_HP_1，PCB 必须按对应的版本设计。这里说的是封装脚号，不是 GPIO54。见 [v3.x 修订差异](https://documentation.espressif.com/esp32-p4-chip-revision-v3.x_user_guide_en.pdf)。

两者的基础指令集都是 RV32IMAFC，手册还列出了 Zc（Zcb、Zcmp、Zcmt）、Zb、XespV 和 XespLoop 扩展。公共的 JIT 后端按构建选定的芯片修订，只使用上板核对过的扩展指令：P4 v1.3 没有 Zba、Zbb、Zbs，v3.2 有，核对结果见[任务 5-2](plans/gb/task-5-2.md#扩展指令的核对)；S31 还没有核对。缓存同步、可执行内存、DMA 和外设由各平台的适配层处理。

## 掌机硬件

- 屏幕：型号、尺寸和接口还没有选定。选型时要验证屏幕在目标分辨率、色深和刷新率下的有效传输带宽，还要处理原机画面的缩放和显示比例。
- 按键：方向键、A、B、Start、Select 和一个单独的菜单键。GBA 的 L 和 R 从硬件第一版起就预留。
- 声音：I2S 输出。
- 存储：SD 卡，存放 ROM 和存档。
- 时钟：带后备电源的 RTC。

## 音画同步

GB、GBC 和 GBA 的画面都是约 59.73Hz，模拟器和各板屏幕都以这个频率为目标，屏幕时序需考虑硬件分频和计数取整。运行快慢由音频输出的节奏控制，节流水位包括已提交但尚未完成混音的音频，避免重复填补同一个缺口。水位偏高或偏低时，小幅调整重采样的比例。画面单独提交。环形音频缓冲和 I2S DMA 只用来吸收短暂的抖动，不用来掩盖持续的性能不足。

外壳上的速度拨杆有 1X 和 2X 两档，GB、GBC 和 GBA 游戏都要支持 2X。2X 时关闭声音，节奏改由墙钟控制。GBC 游戏打开 KEY1 倍速以后，CPU 的工作量本来就翻倍，2X 下是 GB 原速的 4 倍，能不能达到 2 倍以实测为准。

## SD 卡目录

| 卡上路径 | 内容 |
|---|---|
| `roms/` | 游戏 ROM，由用户放入，子目录可以随意划分 |
| `bios/` | GBA 的 BIOS，`gba_bios.bin` 是用户从原机转储的文件，模拟器只读取 |
| `saves/` | 卡带存档，由模拟器写入 |
| `states/` | 即时存档，由模拟器写入 |
| `config/` | 模拟器的设置，每块板子一个文件，比如 `config/sila_p4.ini`，PC 版是 `config/pc.ini` |
| `tests/` | 测试数据，见[真机测试计划](hardware-testing.md) |

设置文件是 `key=value` 的文本，每行一项。不认识的键忽略，缺少的或者值不合法的项用默认值，所以不同版本的固件可以读同一个文件。`SCALING` 提供 `NEAREST` 和 `SMOOTH`，两者的窗口、尺寸和黑边相同。有平滑实现的平台默认使用 `SMOOTH`，包括 ESP32-P4、ESP32-S31 的加速显示后端和 PC GUI；只有 CPU 显示后端的平台固定使用 `NEAREST`，`SCALING` 显示为灰色，设置文件里保存的 `SMOOTH` 也按 `NEAREST` 处理，不阻止启动游戏。游戏窗口放不下 2 倍画面的板子不缩放游戏画面，SETTINGS 里没有 `SCALING`。用户没有选过缩放方式时跟随平台默认值，修改其他设置也不会把默认值写成用户的选择；明确选过以后，重启或更新默认值都不会覆盖它。暂停时切换会在恢复游戏时生效，不重置模拟状态。

设置改动 1 秒后写回，离开 SETTINGS 时立即写回。同一张卡插到不同的板子上时，各自的设置不互相影响，因为板子支持的功能不同，比如 PC 不能调背光，也不能用 JIT。

菜单递归扫描 `roms/`，跳过以 `.` 开头的文件和目录，因为 macOS 往 FAT 卡上复制文件时会生成 `._` 开头的 AppleDouble 文件。ROM 适用的机型由模拟器读 ROM 头判断，不看所在的子目录，单凭 `.gb` 或 `.gbc` 扩展名也不能确定。

`saves/` 和 `states/` 下的子目录和 `roms/` 下的一样，文件名是 ROM 文件名去掉扩展名，再加上编号和后缀：

| ROM | 卡带存档 | 即时存档 |
|---|---|---|
| `roms/gb/Tetris.gb` | `saves/gb/Tetris.0.sav`、`saves/gb/Tetris.1.sav` | `states/gb/Tetris.1.state`、`states/gb/Tetris.2.state` 等 |

卡带存档的 0 和 1 是“卡带存档”一节里轮流写的两份文件。卡带上的 RTC 状态和存档数据写在同一个文件里，没有电池存档的游戏不创建存档文件。即时存档的编号是槽位号，从 1 开始。

因为存档按 ROM 的路径命名，所以 ROM 改名或者换了目录以后，存档文件也要跟着改名。同一个目录下只差扩展名的两个 ROM 会用到同一组存档文件，用户也可能把别的 ROM 改成原来的名字，所以卡带存档和即时存档里都记录 ROM 头里的标识，和当前 ROM 对不上时先提示用户，再由用户选择是否加载。即时存档还记录格式版本，版本不同时不加载。

## ROM 加载

ROM 直接从 SD 卡加载。GB 和 GBC 卡带的 ROM 最大 8MiB（Pan Docs “[MBC5](https://gbdev.io/pandocs/MBC5.html)”），整个载入 PSRAM。GBA 普通卡带的 ROM 最大 32MiB，但是 32MB PSRAM 里还要放帧缓冲和较冷的 JIT 代码，所以不能假设整个 ROM 都能装进去，GBA 的 ROM 都经过 SD 分页缓存和预读读取。ROM 比缓存小时整个留在缓存里，不会缺页。CPU 取指、数据读取和 DMA 都经过同一个访问层读取 ROM。分页抖动的影响单独测量，不预先承诺大 ROM 能跑到原速。

## 存档

### 卡带存档

游戏修改卡带存档以后，模拟器把存档标记为脏，延迟一小段时间写回 SD 卡，退出游戏时立即提交。存档用两份文件轮流写，每份带序号和校验，写完以后刷新存储，这样写到一半掉电时，上一份有效存档还在。FAT 上的一次 rename 不能当作掉电安全的保证。

启动游戏时如果没有载入已有的存档，比如两份都无效，或者存档属于别的 ROM、用户选择不载入，模拟器先把这两份复制成 `.0.sav.old` 和 `.1.sav.old`，再开始写回。备份只保留最近一次，每个 ROM 最多 4 个存档文件。存档文件在卡上但是读不出来时，比如卡出了错，游戏不启动，因为启动以后的写回会覆盖这份没有读到的存档。

### RTC

游戏运行时，RTC 按模拟时间推进，因为 rtc3test 用 GB 的定时器给 RTC 计时，测 tick 间隔时的容差只有 1ms（[rtc3test 的 tests.md](../refs/tests/game-boy-test-roms/rtc3test/tests.md)）。模拟器保存游戏时钟相对后备时钟的偏移，开机时根据后备时钟补算关机期间经过的时间。暂停期间经过的时间在恢复运行时补算；暂停中退出或者换游戏时，先补算再提交存档。

### GBA 存档类型识别

GBA 的 ROM 头里没有存档类型字段（GBATEK “GBA Cart Backup IDs”）。ROM 里有 EEPROM_V、SRAM_V、FLASH_V、FLASH512_V 或 FLASH1M_V 这些 ID 字符串时，按字符串判断。这些字符串是任天堂的库插入的，用别的工具做的 ROM 可能没有，这时按游戏第一次访问存档区的方式判断：在 E005555h 写 Flash 命令序列的是 Flash，向 D000000h 以上发 DMA 位流的是 EEPROM，直接按字节写 E000000h 的是 SRAM。EEPROM_V 不区分 512B 和 8KiB，容量按游戏第一次发出的位流用 6 位还是 14 位地址判断。没有 FLASH1M_V 时，Flash 按 64KiB 处理。已经有存档文件时，以文件里记下的类型和容量为准。

EEPROM 要实现 512B 和 8KiB 两种容量，Flash 要实现不同的容量、厂商 ID 和命令协议，只创建一个同名的存档文件是不够的。ID 字符串不表示卡带上有没有 RTC，带 RTC 的游戏按 ROM 头的游戏代码登记，每一条写明来源。自动判断出错时，用户还不能手动指定存档类型，见路线图 GBA 段的“未尽之事”。

### 即时存档

即时存档从 GB 阶段开始实现。恢复即时存档以后，RAM 中代码对应的 JIT 块要全部失效。

即时存档有 4 个槽位，每个槽位一个文件。写到一半掉电时，只有正在写的槽位可能损坏，读取时由校验发现，其他槽位和卡带存档不受影响。格式版本不同的即时存档不读取，也不做迁移。

读档时卡带 RAM 和 RTC 跟着恢复，下一次写回会用即时存档里的数据覆盖卡带存档，读档之前不另外提示。RTC 补算存档以后经过的时间，做法和关机期间的补算相同，所以游戏时钟不会因为读档而倒退。

## 技术方案

### 执行与 JIT

自研解释器是正确性基线，也是 JIT 的回退路径。JIT 按基本块翻译，采用寄存器常驻、按需计算标志位和快速访存；块间链接等可以叠加的优化按实测的瓶颈加入。代码缓存按机器模式和代码所在区域的 bank 区分翻译结果。RAM 和 HRAM 中的自修改代码以及 bank 切换都必须让对应的缓存正确失效。

### 事件调度

所有设备共用同一条模拟时间线。设备不逐个周期推进，在被访问或者事件到期时才追赶到当前时刻。设备事件在准确的模拟时刻生效。如果事件发生在指令中间，JIT 可以完成当前指令后退出块，但后续访存必须已经受到事件的影响，不能继续执行下一条指令。块内每个相关的 IO 访问点都要给出准确的周期数：读 LY、STAT、定时器寄存器或者修改显示寄存器之前，先把相关设备推进到这次访问的时刻。DMA 和总线等待都计入周期。

### 双核分工

一个核心负责 CPU 和关键时序，另一个核心按实际耗时承担图形合成、混音等工作。两个核心之间用按扫描线或片段划分的任务队列交接工作。每个任务带上对应的寄存器状态和必要的显存快照或版本号，并且保留行中修改的事件，这样另一个核心不会读到已经变化的实时状态。显示提交和 I2S 传输主要交给 DMA，负载轻时也可以只用一个核心计算。

### 内存布局

热的 JIT 代码、调度器和高频访问的数据优先放在片内可执行内存，PSRAM 放 ROM 缓存、帧缓冲和较冷的代码。生成或修改机器码以后，要处理数据缓存、指令缓存和必要的跨核同步。

### 平台接口

模拟核心不依赖 ESP-IDF。屏幕、I2S、按键、SD 卡和 RTC 都通过平台接口访问，所以同一个核心既能在 PC 上调试，也能在两个主控上共用。

## 正确性验证

测试在连着电脑的板子上运行：电脑上的测试程序从 `refs/tests` 选择用例，板子上的测试固件从 SD 卡读取并执行，再按套件的判据判定结果，具体做法见[真机测试计划](hardware-testing.md)。PC 版只有解释器，用来开发和调试。测试集不放进仓库，也不对外分发，测试不需要任何商业游戏 ROM。每个测试按所属套件自己的判据判断是否通过，不把模拟器自己的输出录成标准答案。在解释器上跑的测试也都要在 JIT 上跑，两边的结果必须一致。

GB 和 GBC 的 CPU 指令用 [SingleStepTests/sm83](https://github.com/SingleStepTests/sm83) 的用例检查，整机行为用 [game-boy-test-roms](https://github.com/c-sp/game-boy-test-roms) 收录的测试 ROM 检查，判据有串口输出、最后执行 `LD B,B` 时的寄存器值和套件提供的参考图像。比较画面时只用套件自带的参考图像。

GBA 用这几套测试：

- [SingleStepTests/ARM7TDMI](https://github.com/SingleStepTests/ARM7TDMI)：ARM 和 Thumb 指令的单步用例，比对执行后的寄存器和总线访问。预期结果由 NanoBoyAdvance 生成，作者注明其中的周期数不一定准确。
- [jsmolka/gba-tests](https://github.com/jsmolka/gba-tests)：ARM、Thumb、内存、BIOS、存档等测试 ROM，结束时 `r12` 为 0 表示通过。
- [alyosha-tas/gba-tests](https://github.com/alyosha-tas/gba-tests)：jsmolka/gba-tests 的 fork，补充了总线、DMA、中断、预取、定时器和串口等方面的测试，判据相同。
- [nba-emu/hw-test](https://codeberg.org/nba-emu/hw-test)：总线、DMA、中断、定时器和 PPU 状态等硬件测试，在屏幕上逐项打印 PASS 或 FAIL。

现成套件里没有能逐像素判定 GBA PPU 画面的测试：jsmolka/gba-tests 的 ppu 测试只能目测，nba-emu/hw-test 的 PPU 测试附带的参考图不是 240×160 的原始画面，有的是实机照片，不能逐像素比对。所以 GBA 的画面还和参考帧比较。参考帧由固定版本的 NanoBoyAdvance 在 PC 上生成，选择的理由见 GBA [任务 1-2](plans/gba/task-1-2.md#核对结果)，取帧的条件和判据见[真机测试计划](hardware-testing.md)。参考模拟器的代码不进入本项目，写模拟器时也不阅读它的实现。

自己编写的 PPU 测试 ROM 覆盖各种背景模式、OBJ、仿射变换、混合、窗口、mosaic 和逐行修改寄存器，只用 GBATEK 写清楚了的行为，它们的参考帧还要和按 GBATEK 算出的画面完全相同。第三方 ROM 的参考帧只有参考模拟器的结果，先对照测试的说明或照片目测，用户确认以后才用于比较。画面和参考帧不一致时，先查 GBATEK 和硬件测试确认哪一边正确，不直接照着参考模拟器修改。

EEPROM、没有 ID 字符串时的存档类型判断、卡带 RTC 和 16MiB 以上 ROM 的读取没有现成的测试，所以也自己编写测试 ROM，由 ROM 按 GBATEK 自己判定结果。

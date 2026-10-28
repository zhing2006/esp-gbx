# 参考资料

本目录的索引、抓取脚本和 `boards/` 下的板子资料随仓库保存。其余资料通过脚本下载，不入库，下载后约 1.4GB。macOS 和 Linux 上运行 `./fetch.sh`，Windows 上运行 `powershell -ExecutionPolicy Bypass -File fetch.ps1`，需要 Windows 10 1803 以后系统自带的 `tar.exe`。两个脚本的条目要保持一致。GitHub 和 Codeberg 来源按提交固定版本，取自 Wayback Machine 的页面固定为一份存档，其余为下载时的最新版。

## 文档

| 路径 | 内容 | 来源 |
|---|---|---|
| `docs/riscv-isa/` | RISC-V 指令集规范，RV32I、M 和 Zifencei 是 JIT 后端的编码与同步依据 | riscv/riscv-isa-manual，20240411，提交 6ee57d8 |
| `docs/riscv-psabi/` | RISC-V ELF psABI，整数与硬浮点调用约定 | riscv-non-isa/riscv-elf-psabi-doc，v1.0，提交 74936a9 |
| `docs/pandocs/` | Pan Docs 源文件（`src/`），GB／GBC 主参考 | gbdev/pandocs |
| `docs/gbctr.pdf` | Game Boy: Complete Technical Reference，SM83 逐周期指令时序 | gekkio.fi |
| `docs/Opcodes.json` | 全部操作码的长度、周期、标志位，可生成解码表 | gbdev/gb-opcodes |
| `docs/gbz80.7` | RGBDS 的 SM83 指令说明（`man ./gbz80.7` 查看） | gbdev/rgbds |
| `docs/gbedg/` | Game Boy Emulator Development Guide，偏实现思路 | Ashiepaws/GBEDG |
| `docs/gb-sound-hardware.html` | APU 细节 | gbdev wiki |
| `docs/tcagbd.pdf` | The Cycle-Accurate Game Boy Docs，部分结论已过时，冲突时以测试为准 | AntonioND/giibiiadvance |
| `docs/gbatek.htm` | GBATEK，GBA 主参考，也包含 ARM7TDMI 指令说明 | problemkaputt.de |
| `docs/tcrf-cgb-boot-rom.html` | TCRF 关于 CGB boot ROM 的笔记，其中 “Assigned Palette Configurations” 是兼容调色板表的来源，Pan Docs “Compatibility palettes” 引用了这个页面 | tcrf.net 的修订 2183057，取自 Wayback Machine 2026-09-18 的存档 |
| `docs/gbc-lcd/` | 一台 GBC 屏幕的测量数据：白、黑、R、G、B、Y、C、M 在两个角度下的反射光谱（`gbc-color1`、`gbc-color2`），以及两个角度下 32 级灰阶的读数（`gbc-gamma1`、`gbc-gamma2`）。测量者 Pokefan531，仪器 X-Rite ColorMunki Photo。作者标注数据不完整、可能不可靠，说明见同目录的 `README.md`。`tools/lcd_colors.py` 由它算出颜色校正的参数。仓库没有许可证，只下载这几个文件 | Brankale/Handheld-Color-Space-Project，提交 66b7f4c，`handhelds/Nintendo GBC/[2026-05-13][Pokefan531]/measurements` |

## 测试

| 路径 | 内容 | 来源 |
|---|---|---|
| `tests/game-boy-test-roms/` | 测试 ROM 合集：blargg、mooneye、acid2、mealybug、rtc3test 等，各子目录附通过判据 | c-sp/game-boy-test-roms v7.0 |
| `tests/mooneye-source/` | mooneye 测试源码，用于核对 ROM 的设备依赖、开机相位和判据，不包含模拟器实现 | Gekkio/mooneye-test-suite，提交 8d742b9，与合集 v7.0 的版本记录对应 |
| `tests/same-suite-source/` | SameSuite 测试源码，用于确认用例适用的机型、是否用到倍速和判据 | LIJI32/SameSuite，提交 f71b4b3，与合集 v7.0 的版本记录对应 |
| `tests/age-source/` | AGE 测试 ROM 的源码，用于核对每个用例测的时刻、机型和判据，不包含模拟器实现 | c-sp/age-test-roms，提交 cd3f654，与合集 v7.0 的版本记录对应 |
| `tests/gambatte-testrunner/testrunner.cpp` | Gambatte 的测试程序。合集的 Gambatte 说明引用了它：ROM 运行 15 帧以后，画面左上角显示的十六进制数字和这里的单色数字图案比较，文件名决定在 DMG 还是 CGB 上运行。runner 运行时从这个文件读取数字图案，只用它的图案和运行条件，不参考 libgambatte 的实现。Gambatte 的仓库是 GPLv2，图案不复制进本仓库 | pokemon-speedrunning/gambatte-core，提交 d819bad，与合集说明引用的版本相同 |
| `tests/sm83/` | 每个操作码 1000 个 JSON 用例，含逐周期总线活动 | SingleStepTests/sm83 |
| `tests/gameboy-doctor/` | blargg cpu_instrs 的逐指令 CPU 状态日志 | robert/gameboy-doctor |
| `tests/arm7tdmi/` | ARM7TDMI 单步用例，ARM 20 个文件、Thumb 25 个文件，每个文件 50000 条，含每次总线访问。文件是 `.json.bin` 格式，`v1/transcode_json.py` 里有格式说明。预期结果由 NanoBoyAdvance 生成，作者注明 `cycle` 字段不一定准确 | SingleStepTests/ARM7TDMI |
| `tests/jsmolka-gba-tests/` | GBA 测试 ROM。arm、memory、bios、nes、save 运行到 `idle` 循环时，`r12` 为 0 表示全部通过，否则是第一个失败的测试编号；thumb 的结果在 `r7`，它不写 `r12`。ppu 的 3 个只画一幅画面，没有结果寄存器，只能看画面；unsafe 的作者说明它在实机上不通过，可能是烧录卡的原因，不用。各 ROM 的判据见 GBA 任务 1-1 | jsmolka/gba-tests |
| `tests/alyosha-gba-tests/` | jsmolka/gba-tests 的 fork，用同一套显示结果的代码，但是报告结果的寄存器不都是 `r12`，非 0 的结果常常是测到的计数值。arm、bios、memory、nes、save、thumb、ppu、unsafe 中和 jsmolka 同名的文件与 `jsmolka-gba-tests` 的固定版本相同，只有 arm/arm.gba 是缺少 test 254 修复的旧版本，这些都以 `jsmolka-gba-tests` 为准。使用新增的 Bus、DMA、Interactions、Serial、fifo_dma、irq、prefetcher、psr、timer、timing 共 91 个 ROM，以及上面几个目录里新增的 16 个和顶层的 STM_ALU_IMM。LDM 测的行为和机型有关，作者只在 Game Boy Player 上测过；`prefetcher/bounday_test_1.gba` 没有源码；unsafe 新增的 3 个会让实机死机。这几项不用。选择和判据见 GBA 任务 1-1 | alyosha-tas/gba-tests |
| `tests/nba-hw-test/` | NanoBoyAdvance 作者的硬件测试。bus、dma、haltcnt、irq、timer 下的 10 个 ROM 和 ppu 下的 status-irq-dma、vram-mirror 用 libgba 的控制台在屏幕上逐项打印 PASS 或 FAIL，打印总结的 ROM 全部通过时打印 `congratulations!`。bus/128kb-boundary 不打印总结，status-irq-dma 要用按键在菜单里选择子测试。ppu 下另外 5 个测试附的 `expected.png` 不是 240×160 的原始画面，有的是实机照片，只能目测对照；ram-access-timing 只打印测量值，没有预期结果，不用；`archive/` 是旧测试，不用。判定办法见 GBA 任务 1-1 | nba-emu/hw-test（Codeberg） |

## 本地资料

下面两个目录不由 fetch 脚本下载，也不入库，被 `.gitignore` 的 `/refs/*` 忽略。获取方式和用法见 [`tools/README.md`](../tools/README.md)。

| 路径 | 内容 | 来源 |
|---|---|---|
| `emulators/` | GBA 参考模拟器，只用来生成和核对参考帧，写模拟器时不阅读它们的实现。`nba-src/` 和 `mgba-src/` 是源码，`nba-frames/` 和 `mgba-frames/` 是链接核心库的取帧程序，`scripts/` 是 GBA 任务 1-2 比较两个模拟器和核对画面用的脚本 | nba-emu/NanoBoyAdvance v1.8.3，提交 8c3b159；mgba-emu/mgba 0.10.5，提交 26b7884 |
| `toolchains/` | 构建自写 GBA 测试 ROM 的 Arm GNU Toolchain，`tools/gba_test_roms.py` 的 `--toolchain` 指向它 | Arm GNU Toolchain 15.3.Rel2，`arm-gnu-toolchain-15.3.rel2-darwin-arm64-arm-none-eabi` |

## 板子

板子资料随仓库提交，不经过 fetch 脚本。原理图按电路区域裁成 PNG，放在各板子的 `schematic/` 下。有用的厂商配置和初始化代码直接保存在仓库中，不依赖原始下载或解压目录。原始 PDF 和独立的 LICENSE 文件不放进板子资料目录，参考源码保留原有版权和许可声明。

| 路径 | 内容 | 来源 |
|---|---|---|
| `boards/jc1060p470/` | GUITION JC1060P470 的原理图（`schematic/`）、JD9165 初始化序列（`jd9165-init.dtsi.txt`）和厂商对 BSP 的修改说明（`vendor-readme.txt`），整理结果见 `docs/boards/jc1060p470.md` | 厂商资料包的镜像 cheops/JC1060P470C_I_W，提交 7d6f3ce |
| `boards/m5stack_tab5/` | M5Stack Tab5 的原理图（`schematic/`，由厂商的 PDF 逐页转成 PNG）、M5Stack 的 ST7121 驱动（`esp_lcd_st7121.c.txt`）和出厂固件的 BSP（`m5stack_tab5.c.txt`），整理结果见 `docs/boards/m5stack_tab5.md` | 原理图来自 M5Stack 文档站，两个源码文件来自 m5stack/M5Tab5-UserDemo，提交 b4e356b |
| `boards/osptek_p4c5/` | OSPTEK ESP32-P4C5-Module 底板 V1.5 和 4.3 寸 LCD 转接板按电路区域裁剪的原理图（`schematic/`）、IDF 6.1 示例的触摸和 DSI 配置、ST7102 驱动和时序宏、面板初始化序列、屏厂 GX09C 初始化文件，整理结果见 `docs/boards/osptek_p4c5.md` | gitee osptek/esp32-p4c5-module-dev-board，提交 `dacb40fa25d6400bfa2524c4e6d4ecbf11b7fea5`；osptek/tft-4.3-480x800-mipi-st7102，提交 `601868ee4a83271281689be94cb4164c1270076c`；校验值见板子文档 |
| `boards/sila_p4/` | Sila P4 按电路区域裁剪的原理图（`schematic/`）、厂商 Sila 引脚配置和板级代码、音频与背光初始化代码、ST7703 驱动和时序定义，整理结果见 `docs/boards/sila_p4.md` | 用户提供的 `P4-4IN` 资料包（底板 V3.1、模组 V1.2、小智 2.03 示例），校验值见板子文档；显示驱动来自 `waveshare/esp_lcd_st7703` 1.0.5 |
| `boards/sila_p4x/` | P4X 原理图分区裁图（`schematic/`）、R3 正反面 PCB 预览（`pcb/`），以及显示、AXS15260D 时序、初始化序列和触摸报告格式、触摸共用复位、PMU、音频、SD 和芯片配置摘录，整理结果见 `docs/boards/sila_p4x.md` | 用户提供的 `SCH_P4X_2026-10-13.pdf` 和 PCB 预览，校验值见板子文档；Ergou-/esp32-p4xc5-aibox，提交 `8c6bc38bc7a082d9c4475e08c40778442a8dd8be` |
| `boards/waveshare_c5_lcd28/` | Waveshare ESP32-C5-Touch-LCD-2.8 按电路区域裁剪的原理图（`schematic/`），以及厂商仓库的 CST3530 驱动、ST7789 初始化序列、CH32V003 IO 扩展器的说明和代码、BSP 的引脚和外设初始化、音频、TF 卡和电池示例与 sdkconfig 摘录，整理结果见 `docs/boards/waveshare_c5_lcd28.md` | waveshareteam/ESP32-C5-Touch-LCD-2.8，提交 `186eb7f3b86b6e673627bba32f8f1a9c60687093`；校验值见板子文档 |
| `boards/wks_s31_cb/` | 慧勤智远 ESP32-S31 CB V1.1 按电路区域裁剪的原理图（`schematic/`），以及厂商基础例程的 RGB 屏、GT1151Q 触摸、TF 卡、ES8389 和 I2S 代码与 sdkconfig 摘录，整理结果见 `docs/boards/wks_s31_cb.md` | 用户提供的“慧勤智远 ESP32-S31 CB V1.1小系统板”资料包，校验值见板子文档 |

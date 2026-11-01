# esp-gbx

[English](README.md) | 简体中文

![esp-gbx 在八块板子上运行](docs/boards/esp-gbx-boards.png)

esp-gbx 是在 ESP32-P4、ESP32-S31 和 ESP32-C5 上运行的 GB / GBC / GBA 模拟器。模拟核心、解释器、RISC-V JIT 和设备模型都依据 [Pan Docs](https://gbdev.io/pandocs/)、[GBATEK](https://problemkaputt.de/gbatek.htm) 等硬件资料从零编写，没有采用现有模拟器的代码。游戏直接从 SD 卡加载，不需要先在电脑上转换。

固件包括：

- 解释器，以及把 SM83 和 ARM7TDMI 代码翻译成 RISC-V 的 JIT。解释器是正确性的基线，也是 JIT 的回退路径，设置里的 CPU 一项在两者之间切换。
- 卡带存档、带 RTC 的 MBC3 和 GBA 卡带的时钟，以及每个游戏 4 个即时存档槽位。卡带存档轮流写进两个文件，所以写到一半掉电时，上一份存档还在。
- 主界面和暂停菜单。设置包括音量、背光亮度、执行后端、调色板、GBC 画面的颜色校正和缩放方式，保存在卡上。
- 画成掌机样子的触摸界面，有屏幕按键和 1X、2X 两档的速度拨杆，2X 时关闭声音。有相应接口的板子还可以用 USB HID 手柄和蓝牙 LE HID 手柄。

测试在板子上运行，解释器和 JIT 都要跑。用例来自 [SingleStepTests](https://github.com/SingleStepTests) 和第三方测试 ROM，每个测试按所属套件自己的判据判断，见[真机测试计划](docs/hardware-testing.md)。`host/` 下的 PC 版有和固件相同的菜单，但是只有解释器，用来开发和调试。

## 支持的机型

| 芯片 | 机型 | 执行后端 |
|---|---|---|
| ESP32-P4 | GB、GBC、GBA | 解释器、SM83 JIT、ARM JIT |
| ESP32-S31 | GBA | 解释器、ARM JIT |
| ESP32-C5 | GB、GBC | 解释器、SM83 JIT |
| PC（`host/`） | GB、GBC、GBA | 解释器 |

固件只编入执行核心放得进芯片片内 RAM 的系统。ESP32-S31 的片内 RAM 放不下 GB 和 GBA 两套执行核心，两块 S31 板子都只编入 GBA。GBA 的解释器也不放在片内 RAM 里，空出来的地方给 JIT 的代码池，所以 S31 上的解释器后端会慢一些。ESP32-C5 只有一个核和 384KB SRAM，所以只编入 GB 和 GBC。C5 的游戏页控件在开机时画好，以后除了速度拨杆，不再随触摸改变外观，也没有 VFD。它的 SPI 屏每 25ms 最多取一帧游戏画面，原速时只显示一半的帧。

能在 GB 上运行的游戏，也就是黑白游戏和 CGB 标志为 0x80 的游戏，按 GB 还是按 GBC 运行由 PALETTE 设置决定。CGB 标志为 0xC0 的游戏总是按 CGB 模式运行。

GBA 游戏需要从原机转储的 BIOS，放在卡上的 `bios/gba_bios.bin`。模拟器按校验值核对它，卡上没有有效的 BIOS 时不能启动 GBA 游戏。

## SD 卡

卡用 FAT 文件系统，文件名可以是 UTF-8 编码的长文件名。

| 卡上路径 | 内容 |
|---|---|
| `roms/` | 游戏 ROM，由用户放入，子目录可以随意划分 |
| `bios/` | `gba_bios.bin`，用户从原机转储的 GBA BIOS，模拟器只读取 |
| `saves/` | 卡带存档，由模拟器写入 |
| `states/` | 即时存档，由模拟器写入 |
| `config/` | 设置，每块板子一个文件，比如 `config/sila_p4.ini`，PC 版是 `config/pc.ini` |
| `tests/` | 测试固件用的测试数据，见[真机测试计划](docs/hardware-testing.md) |

菜单递归扫描 `roms/`，跳过以 `.` 开头的文件和目录，比如 macOS 往 FAT 卡上复制文件时生成的 `._` 文件。游戏适用的机型由模拟器读 ROM 头判断，不看它所在的目录和扩展名。

`saves/` 和 `states/` 下的目录和 `roms/` 下的一样。文件名是 ROM 文件名去掉扩展名，再加上编号和后缀：

| ROM | 卡带存档 | 即时存档 |
|---|---|---|
| `roms/gb/Tetris.gb` | `saves/gb/Tetris.0.sav`、`saves/gb/Tetris.1.sav` | `states/gb/Tetris.1.state` 至 `Tetris.4.state` |

两个卡带存档文件轮流写，没有电池存档的游戏不创建存档文件。即时存档的编号就是槽位号。因为存档按 ROM 的路径命名，所以 ROM 改名或者换了目录以后，存档也要跟着改名。两种存档都记录了 ROM 头里的标识，和当前 ROM 对不上时，模拟器先提示，再由用户选择是否加载。

设置文件每行一项 `key=value`。不认识的键会被忽略，缺少的或者值不合法的项用默认值，所以不同版本的固件可以读同一个文件。因为各板支持的功能不同，所以每块板子各用一个设置文件。

SD 卡目录的完整说明见[产品设计](docs/product-design.md#sd-卡目录)。

## 板子

| 板子 | 芯片 | 屏幕 | 机型 | 声音 | 手柄 | RTC |
|---|---|---|---|---|---|---|
| [M5Stack Tab5](docs/boards/m5stack_tab5.md) | ESP32-P4 v1.3，360MHz | 5″ 720×1280 MIPI DSI | GB、GBC、GBA | 喇叭，立体声耳机 | USB、蓝牙 LE | RX8130CE |
| [慧勤智远 ESP32-S31 CB](docs/boards/wks_s31_cb.md) | ESP32-S31，320MHz | 4.3″ 800×480 RGB | GBA | 两个喇叭 | USB | 无 |
| [Waveshare ESP32-C5-Touch-LCD-2.8](docs/boards/waveshare_c5_lcd28.md) | ESP32-C5，240MHz | 2.8″ 240×320 SPI | GB、GBC | 喇叭 | 无 | PCF85063A |
| [GUITION JC1060P470](docs/boards/jc1060p470.md) | ESP32-P4 v1.0，360MHz | 7″ 1024×600 MIPI DSI | GB、GBC、GBA | 喇叭 | 蓝牙 LE | RX8025T |
| [OSPTEK ESP32-P4C5-Module](docs/boards/osptek_p4c5.md) | ESP32-P4 v3.2，400MHz | 4.3″ 480×800 MIPI DSI | GB、GBC、GBA | 喇叭 | USB、蓝牙 LE | 无 |
| [Sila P4](docs/boards/sila_p4.md) | ESP32-P4 v1.3，360MHz | 720×720 MIPI DSI | GB、GBC、GBA | 喇叭 | 无 | 无 |
| [Sila P4X AIBox](docs/boards/sila_p4x.md) | ESP32-P4 v3.2，400MHz | 6.2″ 452×1280 MIPI DSI | GB、GBC、GBA | 两个喇叭 | 蓝牙 LE | 无 |
| [乐鑫 ESP32-S31-Korvo-1](docs/boards/esp32_s31_korvo_1.md) | ESP32-S31，320MHz | 4.3″ 800×480 RGB | GBA | 两个喇叭 | USB | 无 |

每块板子都接入了屏幕、触摸、声音和 SD 卡。表中的芯片修订是手上这块板子的。P4 的固件和芯片修订绑定，v1.x 和 v3.x 要分别构建，v1.x 最高只有 360MHz。有 RTC 的板子开机时，卡带的时钟会补上关机期间经过的时间，即时存档也会记下存档时的时间。只有一个喇叭的板子把两个声道混在一起送给喇叭。各板的资料来源、引脚和初始化约束见 [docs/boards/](docs/boards/) 下的文档，各板的界面布局见 [UI 布局](docs/ui-layouts/index.md)。

### M5Stack Tab5

preset 是 `m5stack_tab5`。ESP32-P4 v1.3，32MB PSRAM。屏幕是 5 寸 720×1280 MIPI DSI，对应显示驱动 IC 为 ST7121 的 Tab5，竖着使用，触摸是 ST7123。默认布局的游戏窗口是 720×480，GBA 游戏放大 3 倍。关掉 `GBX_TAB5_X3_LAYOUT` 以后，窗口换成 480×320，放大 2 倍。ES8388 只把左声道送给喇叭，插上耳机时固件关掉喇叭，耳机里是立体声。USB-A 口可以接 HID 手柄，板上的 ESP32-C6 用来连接蓝牙 LE 手柄。板上的时钟是 RX8130CE。

### 慧勤智远 ESP32-S31 CB V1.1

preset 是 `wks_s31_cb`。ESP32-S31-WROOM-3-N16R16V，16MB 八线 PSRAM。屏幕是同一厂家的 4.3 寸 800×480 RGB 触摸屏 WKS43WV067-WCT，转成竖屏使用，触摸是 GT1151Q，背光用 PWM 调光。ES8389 驱动左右两个喇叭，是立体声。USB-A 口可以接 HID 手柄。固件只编入 GBA。

### Waveshare ESP32-C5-Touch-LCD-2.8

preset 是 `waveshare_c5_lcd28`。ESP32-C5-WROOM-1-N32R8 模组里的 ESP32-C5，8MB 四线 PSRAM。2.8 寸 240×320 SPI 屏的显示驱动是 ST7789V2，触摸是 CST3530，背光经 CH32V003 IO 扩展器调光。SD 卡和屏幕共用一条 SPI 总线。ES8311 驱动一个喇叭，板上的时钟是 PCF85063A。固件编入 GB 和 GBC，这块板子上不支持手柄。

### GUITION JC1060P470

preset 是 `jc1060p470`。ESP32-P4 v1.0，32MB PSRAM。7 寸 1024×600 IPS MIPI DSI 屏，显示驱动是 JD9165，触摸是 GT911。板子竖着拿，所以 UI 转了 270 度。ES8311 驱动一个喇叭，板上的时钟是 RX8025T。板上的 ESP32-C6 用来连接蓝牙 LE 手柄。

### OSPTEK ESP32-P4C5-Module

preset 是 `osptek_p4c5`。OSPTEK 的底板加 ESP32-P4C5 核心板，ESP32-P4 v3.2，32MB PSRAM。屏幕是 OSPTEK 的 4.3 寸 480×800 YDP430BT009-V1，显示驱动是 ST7102，触摸是 ST7123，经 OSPTEK 的 4.3 寸转接板接到底板背面的 15 针 DSI 接口。**背光调光需要另接一根跳线**，不接时背光一直全亮，见板子文档。ES8311 驱动一个喇叭。USB-A 口可以接 HID 手柄，核心板上的 ESP32-C5 用来连接蓝牙 LE 手柄。

### Sila P4

preset 是 `sila_p4`。ESP32-P4 v1.3，32MB PSRAM。720×720 MIPI DSI 屏，显示驱动是 ST7703，触摸是 GT911。ES8311 驱动一个喇叭。这块板子上不支持手柄，因为板子从串口供电时，接在另一个 USB-C 口上的手柄没有电，P4 也检测不到它。

### Sila P4X AIBox

preset 是 `sila_p4x`。ESP32-P4 v3.2，32MB PSRAM，32MB flash。屏幕是 6.2 寸 452×1280 的模组，显示和触摸都由 AXS15260D 负责，横着使用。ES8389 驱动左右两个喇叭，是立体声。板上的 ESP32-C5 经 1 位 SDIO 总线连接蓝牙 LE 手柄。**手上这块板子背光电路的 R26 量得约 1Ω，图纸上是 10Ω。满占空比时模组排线一端发烫，屏幕从这一端开始变黑，运行完整固件时整板还会掉电，所以固件把 100% 亮度对应的占空比限制在 5%。** 要改这个限制，先看板子文档。

### 乐鑫 ESP32-S31-Korvo-1

preset 是 `esp32_s31_korvo_1`。ESP32-S31-WROOM-3，16MB 八线 PSRAM。套件里的屏幕是 ESP32-S3-LCD-EV-Board-SUB3，4.3 寸 800×480 RGB，显示驱动是 ST7262E43，触摸是 GT1151，转成竖屏使用。板子有电时背光就亮着，不能调光，所以设置里的 BRIGHTNESS 不可用。ES8389 驱动左右两个喇叭，是立体声。USB-A 口可以接 HID 手柄。固件只编入 GBA。

## 烧录发布的固件

每个 [GitHub Release](https://github.com/zhing2006/esp-gbx/releases) 里每块板子有一个完整固件 `esp-gbx-<版本>-<板子>.bin`，bootloader、分区表、应用和 UI 素材都在这一个文件里。用 ESP-IDF v6.1 的 esptool 从 0x0 烧录：

```
esptool --chip esp32p4 -p <端口> -b 460800 write-flash 0x0 esp-gbx-v0.1.0-sila_p4.bin
```

芯片写 `esp32p4`、`esp32s31` 或 `esp32c5`，S31 的板子还要在 `write-flash` 前面加 `--no-stub`。P4 的固件只能在构建时选定的芯片修订上运行：`jc1060p470`、`m5stack_tab5` 和 `sila_p4` 的固件用于 v1.x，`osptek_p4c5` 和 `sila_p4x` 的用于 v3.x。Release 里的 `SHA256SUMS` 列出了各个固件的 SHA-256。每个版本的改动见 [CHANGELOG.md](CHANGELOG.md)，用英文写。

## 构建

仓库里的 PNG、JPEG 和 PDF 用 Git LFS 保存，clone 之前要先安装 Git LFS，并运行一次 `git lfs install`。

固件用 ESP-IDF v6.1 构建，ESP-IDF 由 EIM（ESP-IDF Installation Manager）安装，打包 UI 素材还需要 [uv](https://docs.astral.sh/uv/)。`CMakePresets.json` 里的 preset 选择板子和构建类型，比如 `sila_p4-release` 和 `sila_p4-debug`：

```
eim run "idf.py --preset sila_p4-release build" v6.1
eim run "idf.py --preset sila_p4-release -p <端口> -b 460800 flash monitor" v6.1
```

ESP-IDF v6.1 把 ESP32-S31 作为 preview target。S31 板子的 preset 在构建目录里第一次构建以前，要先用 `idf.py --preset wks_s31_cb-release --preview set-target esp32s31` 选择 target，同样通过 `eim run` 执行。target 设置好以后，构建就不需要 `--preview` 了。PC 版是 `host/` 下的 CMake 工程：

```
cd host
cmake --preset debug
cmake --build --preset debug
```

构建配置和模块划分见[架构](docs/architecture.md)，添加新板子的步骤见[加板指南](docs/adding-a-board.md)。

## 许可证

[Apache License 2.0](LICENSE)

菜单使用[缝合像素字体](https://github.com/TakWolf/fusion-pixel-font) 2026.10.16 版，许可证是 [SIL Open Font License 1.1](raws/ui/common/fonts/OFL.txt)。字体源文件，以及它所取材的几套字体的许可证，放在 `raws/ui/common/fonts/`。

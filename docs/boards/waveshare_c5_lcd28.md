# Waveshare ESP32-C5-Touch-LCD-2.8

项目代号是 `waveshare_c5_lcd28`，板级名称是 Waveshare ESP32-C5-Touch-LCD-2.8。这是微雪（Waveshare）的 ESP32-C5 触摸屏开发板，主控是 ESP32-C5-WROOM-1-N32R8 模组，屏幕是 2.8 英寸 240×320 的 SPI 屏，显示驱动是 ST7789V2，触摸控制器是 CST3530。板上还有 CH32V003 IO 扩展器、ES8311 codec、PCF85063A RTC、QMI8658 和 SHTC3。

ESP32-C5 只有一个高性能核，片内 RAM 也比 P4 和 S31 少，所以这块板子的固件只编入 GB，UI 打开了 `GBX_UI_STATIC_SHELL`，见[架构](../architecture.md#板子)和 [UI 布局](../ui-layouts/index.md#效果图)。

## 型号和版本

原理图只有一页，标题栏写着 ESP32-C5-Touch-LCD-2.8，没有版本号和日期；PDF 的创建日期是 2026-04-25。厂商仓库的出厂固件有 v1.1、20261022 和 20261024 三个版本，资料里没有写它们对应哪一版 PCB。实物 PCB 的丝印版本尚未核对。

手上这块板经 USB-Serial/JTAG 读取，esptool v5.5.0 报告 ESP32-C5HR8 revision v1.2、48MHz 晶振，特性一栏写的是 Wi-Fi 6（双频）、BT 5 (LE)、IEEE802.15.4、Single Core + LP Core、240MHz、封装内 8MB PSRAM（AP_3v3）。flash 的 JEDEC ID 为 `20 40 19`，容量 32MB，和模组型号 N32R8 一致。

出厂固件做了整片备份，32MB，SHA-256 为 `1f67d055b2b4bcb7de11d7124ea72342fe91c17838dbacc37e46c983de2c3b2a`，保存在本地，不入库。备份里 0x8000 的分区表和厂商 v1.1 镜像的逐字节相同，应用部分和三个厂商镜像都不同，所以它不是其中任何一个。后 16MB 全是 0xFF。

## 资料

资料来自厂商仓库 https://github.com/waveshareteam/ESP32-C5-Touch-LCD-2.8 ，固定提交 `186eb7f3b86b6e673627bba32f8f1a9c60687093`，许可是 Apache-2.0。数据手册来自 Waveshare 资料页“Resources And Documents”的链接。下表是整理时用到的原始文件，校验值是 SHA-256：

| 原文件 | 内容 | SHA-256 |
|---|---|---|
| `hardware/schematic/ESP32-C5-Touch-LCD-2.8.pdf` | 原理图，1 页 | `22e6ff67d7a5892789acbc362cc3cd4c8d8ddae29fabac16d753ccb83ed501ae` |
| `firmware/ESP32-C5-Touch-LCD-2.8_FactoryOnly_v1.1.bin` | 出厂固件 v1.1 | `92d0f25d7b8c43900c8882e2ca9cee1ca7c8a9338504bd392567f3d1e8cdf174` |
| `firmware/ESP32-C5-Touch-LCD-2.8_FactoryOnly_20261022.bin` | 出厂固件 20261022 | `7b47c2a622077fb7b36276ae63611ddfced58be23237526d341c7fb3b6d9816a` |
| `firmware/ESP32-C5-Touch-LCD-2.8_FactoryOnly_20261024.bin` | 出厂固件 20261024 | `e577a5451b58630acabdc322829fbeb3d52f4c1dc4248e1c6f06a7e8f34d24b3` |
| ES8311 数据手册 Rev 10.0 | 引脚、I2C 地址、寄存器 | `be42a2cb09093378feaccf55c227be579edc5a17786a11cc34254ea91264538a` |
| ES8311 用户指南 | 时钟和寄存器配置 | `601320b63290526af8c684ab1a12592231f8cbd68014222e86d4d21b0ea904fe` |
| PCF85063A 数据手册 Rev 7.3 | 寄存器、STOP 和 OS 标志 | `c1caa777a329021d764cb3d5db73434544267434ca6d05209f00c02d8f16e0ae` |
| ST7789V2 数据手册 V1.0 | 串口时序、复位时序、命令 | `8937678475e20598c8deb17c3cdd41403da6317a3b6518f86144b8a609de9f46` |

开发需要的图片和参考源码保存在 `refs/boards/waveshare_c5_lcd28/`。原理图按电路区域裁成 300dpi PNG，保存在 `schematic/`，`crops.json` 记录原文件的 SHA-256、页码和以 PDF 点为单位的裁剪坐标。

| 图片 | 裁剪内容 |
|---|---|
| `01-esp32c5-module.png` | 模组 U5、SPI 线上的 0Ω 电阻 R75-R77、EN 的阻容 |
| `02-gpio-table.png` | GPIO 和网络的对照表，表里的横线就是连线 |
| `03-usb-c.png` | USB-C 口 J2 |
| `04-battery-charger-power-key.png` | 充电 ETA6098、电源键控制 U2、Q4/Q5 反相、电池分压、电池座 J1 |
| `05-power-3v3.png` | MP1605 降压、Q2 电源切换、D1/D2 |
| `06-io-expander.png` | CH32V003 |
| `07-lcd-touch.png` | 屏幕 FPC L4、触摸 FPC L5、背光三极管 T1 |
| `08-tf-card.png` | TF 卡座 TF1 |
| `09-codec-amplifier.png` | ES8311、NS4150B、RT9193 |
| `10-microphone.png` | 麦克风 MIC1 |
| `11-rtc.png` | PCF85063A 和 J7 |
| `12-keys.png` | BOOT 和 RESET 键 |
| `13-pin-headers.png` | 排针 J8、J5、J6 |
| `14-imu.png` | QMI8658 |
| `15-shtc3.png` | SHTC3 |
| `16-title.png` | 标题栏 |

参考源码是厂商仓库的原文件。CST3530 驱动和 BSP 带着 Espressif 和 Waveshare 的 SPDX 声明（Apache-2.0），Arduino 库和 IDF 示例的文件没有文件头，按仓库的 Apache-2.0 使用：

| 文件 | 原文件 | 内容 |
|---|---|---|
| `esp_lcd_touch_cst3530.c.txt`、`esp_lcd_touch_cst3530.h.txt` | `examples/factory/components/waveshare__esp_lcd_touch_cst3530/` | CST3530 的寄存器地址和报告格式 |
| `Display_ST7789.cpp.txt`、`Display_ST7789.h.txt` | `examples/arduino/libraries/board_config/src/` | ST7789 的初始化序列和 SPI 引脚 |
| `Board_IO.cpp.txt`、`Board_IO.h.txt` | 同上 | CH32V003 的寄存器、复位时序、电池 ADC |
| `esp32_c5_touch_lcd_2.8.h.txt` | `examples/factory/components/esp32_c5_touch_lcd_2_8/include/bsp/` | BSP 的引脚定义 |
| `display.h.txt` | 同上 | 分辨率、SPI 时钟和颜色顺序 |
| `esp32_c5_touch_lcd_2.8.c.txt` | `examples/factory/components/esp32_c5_touch_lcd_2_8/` | I2C、共用的 SPI2、I2S 和 ES8311、屏幕和触摸的初始化 |
| `bsp_board.c.txt` | 同上 | CH32V003 初始化、电池 ADC、BOOT 键、SDSPI 挂载和初始化顺序 |
| `bsp_board_extra.cpp.txt` | `examples/esp-idf/07_audio/components/bsp_extra/src/` | codec 的打开、关闭和功放的先后顺序 |
| `ch32v003.md.txt` | `docs/ch32v003.md` | CH32V003 固件的说明 |
| `02_sd_card-main.c.txt` | `examples/esp-idf/02_sd_card/main/main.c` | TF 卡示例 |
| `08_battery_voltage-main.c.txt` | `examples/esp-idf/08_battery_voltage/main/main.c` | 电池电压示例 |
| `vendor-config.txt` | 出厂工程的 `sdkconfig.defaults`、`partitions.csv`、`sdkconfig`，`07_audio` 的 `sdkconfig.defaults`，BSP 的 `idf_component.yml` 和 `.github/product.json` | 摘录，每段带原文件的 SHA-256 |

## 主控和存储

ESP32-C5-WROOM-1-N32R8 是 32MB flash、8MB PSRAM 的模组。ESP32-C5 是单核 RV32IMAC，没有 Zba/Zbb/Zbs，片内 HP SRAM 384KB，不经过缓存；flash 和 PSRAM 经一个 32KB 的指令和数据共用缓存读取。芯片没有 SDMMC 控制器，也没有 PPA 和 2D-DMA。项目配置如下：

- CPU 240MHz，和厂商示例相同。
- PSRAM 是四线模式，120MHz，厂商示例用的是 80MHz。代码、数据和画面都要经过 PSRAM，所以用了 IDF 给 ESP32-C5 提供的最高一档。
- flash 用 QIO、20MHz，大小按实际的 32MB 配置。共用的分区表只用前 16MB，所以读 flash 用的都是 3 字节地址。厂商示例的 flash 是 80MHz，但是 flash 和 PSRAM 共用 MSPI 的核心时钟，PSRAM 用 120MHz 时，flash 只能用 20MHz 或 120MHz（IDF v6.1 中 ESP32-C5 的 `mspi_timing_tuning_configs.h`）：80MHz 要求的核心时钟和 PSRAM 不同；40MHz 的分频比是 3，而 SDR 模式要求分频比是 1 或偶数；120MHz 要打开 flash 的 HPM。代码和只读数据在启动时已经复制到 PSRAM（`SPIRAM_XIP_FROM_PSRAM`），运行时从 flash 读的主要是 UI 资源分区。
- GB JIT 的第二级等存档和即时存档分到内存以后再分配，取剩下的 PSRAM，留出 256KB（`GBX_GB_JIT_STORE_LEAVE_KB`）给游戏运行时菜单和存档的分配。4MB 的游戏差不多占满 PSRAM，所以 ROM 列表改成最多 1024 项、名字共 64KB（`GBX_LIBRARY_ENTRIES`、`GBX_LIBRARY_NAMES_KB`），比默认值小，给第二级多留一些。
- 片内 RAM 放不下 192KB 的 GB JIT 代码池。JIT 的块表也要放在片内 RAM 里，它的大小随代码池的块数变化，所以 `GBX_GB_JIT_POOL_KB` 设成 64，让代码池、共用代码和块表都留在片内。
- 控制台是 USB-Serial/JTAG，见“烧录和恢复”。厂商的出厂工程用 UART0、2000000 波特率，USB-Serial/JTAG 作为第二控制台。

## IC

| 位号 | 型号 | 用途 | 7 位地址 |
|---|---|---|---|
| U5 | ESP32-C5-WROOM-1-N32R8 | 主控模组 | |
| U9 | CH32V003F4U6 | IO 扩展器、背光 PWM、电池 ADC，运行 Waveshare 的固件 | 0x24 |
| 触摸屏上 | CST3530 | 电容触摸 | 0x58 |
| U4 | ES8311 | 音频 codec，CE 经 R16 10kΩ 接 AGND | 0x18 |
| U7 | NS4150B | D 类功放，3V3 供电 | |
| U8 | RT9193-33PB | 模拟 3.3V LDO（A3V3） | |
| U11 | MP1605GTF-Z | VCC 降压到 3V3 | |
| U1 | ETA6098 | 单节锂电池充电 | |
| U2 | ECJ23001-4FCBD6 | 电源键控制 | |
| U6 | PCF85063ATL | RTC | 0x51 |
| U3 | QMI8658 | 六轴 IMU，SA0 接地，项目不用 | 0x6B（推测，见“待确认”） |
| U10 | SHTC3 | 温湿度，项目不用 | 0x70 |
| T1 | 8050 | 背光低边开关 | |

屏幕的型号原理图上没有写，厂商的 README 写的是 2.8 英寸 ST7789 SPI 屏，240×320。

## 屏幕和触摸

### 连接

屏幕是 18 针 FPC L4，触摸是 FPC L5，原理图见 `07-lcd-touch.png`：

| 信号 | 连接 | 说明 |
|---|---|---|
| SCK | GPIO6 | 和 TF 卡共用 SPI2，10kΩ 上拉 R34 |
| SDA（MOSI） | GPIO7 | 同上，R33 |
| SDO（MISO） | GPIO8 | 同上，R35 |
| RS（D/C） | GPIO9 | |
| CS | GPIO10 | |
| RST | CH32V003 EXIO1 | 低电平复位 |
| LEDA、LEDK | VLED+ 接 3V3，四个 LEDK 各串 10Ω 到 VLED- | 背光，见“背光” |
| TP_SDA、TP_SCL | GPIO0、GPIO1 | 共用 I2C |
| TP_INT | GPIO5 | 没有画上拉，项目不用 |
| TP_RST | CH32V003 EXIO0 | 低电平复位 |

屏幕没有引出 TE。L4 的 15-18 脚是电阻触摸的 XL、YU、XR、YD，没有连接。MISO 不是 SPI2 的 IOMUX 引脚，所以总线经 GPIO 矩阵。

### 显示配置

屏幕用 SPI2 的 mode 0 驱动，像素是 RGB565。ST7789V2 数据手册 7.4.2 节的串口写周期最短 16ns，也就是 62.5MHz；SPI2 从 PLL_F160M 分频，160MHz / 3 = 53.33MHz 是不超过它的最高频率，项目用这个频率。厂商 BSP 用 80MHz，超出了数据手册的值。按 53.33MHz 算，整屏 153600 字节传一次约 23ms，这是计算值。

初始化先由 IDF 的 ST7789 驱动发 SLPOUT、MADCTL、COLMOD（16 位）和 RAMCTRL，RAMCTRL 设成低字节在前，和 UI、帧缓冲里的 RGB565 一致；厂商 Arduino 代码同样设了小端（`B0h 00h E8h`）。然后按厂商 `Display_ST7789.cpp` 的顺序发前后肩、门极电压、VCOM、电源、帧率（`C6h 0Fh`）和 gamma，最后发 INVON 和 DISPON。不发 INVON 时颜色是反的，厂商 BSP 同样调用了 `esp_lcd_panel_invert_color(true)`。复位按数据手册的复位时序：EXIO1 拉低再拉高，120ms 以后才发命令。

面板按扫描方向是 240×320 竖屏，和 UI 尺寸相同，`Rotation::none`。帧缓冲只有一个，在 PSRAM 里，SPI 的 DMA 直接读它。游戏画面不经过帧缓冲，由片内 RAM 里的两段缓冲分段发送，每段放得下 8 个整行，第一段以后用 RAMWRC（ST7789V2 数据手册 9.1.33）接着写；每 25ms 最多发送一帧（`GBX_DISPLAY_GAME_HALF_RATE`）。GB 模式的画面按灰阶交给呈现任务，见[架构](../architecture.md#显示和触摸)。板子没有给面板设偏移。

### 背光

背光的低边由 T1（8050）开关，基极经 R49 1kΩ 接 CH32V003 PC3 的 PWM 输出（LCD_BL）。PWM 由 CH32V003 寄存器 05h 设定，0 到 255。T1 的基极没有下拉，CH32V003 复位期间背光的状态由它的引脚决定，原理图看不出来。PWM 频率没有资料。

### 触摸

CST3530 的地址是 0x58，寄存器地址 32 位，高字节在前。地址和读取分成两次传输，和厂商驱动一样。报告从 `D0070000h` 读起，前 4 字节里第 4 字节的低 4 位是点数；每点 5 字节，依次是 X 低字节、Y 低字节、压力、Y 和 X 的高 4 位、事件（1 按下、2 保持、0 抬起）和手指编号。第二点起从 `D0070900h` 读，读完写 `D00002ABh` 交还报告。板级代码 `cst3530_touch.cpp` 按厂商驱动的寄存器地址和报告格式读取，没有引入厂商的组件。

复位由 EXIO0 完成。厂商 Arduino 代码拉低 100ms、等 500ms，BSP 各用 10ms。项目在复位结束以后每 20ms 探测一次，最多等 500ms。平台轮询触摸，不用 TP_INT。

触摸报告的坐标范围是 240×320，和面板扫描坐标相同，不需要交换或翻转。

## IO 扩展器

CH32V003 运行 Waveshare 的固件，地址 0x24，和触摸、codec 在同一条 I2C 上。按 `docs/ch32v003.md` 和 Arduino 的 `Board_IO.cpp`，寄存器 02h 设方向（1 为输出），03h 是输出电平，05h 是背光 PWM，06h 是电池 ADC（两个字节，低字节在前，10 位）。厂商代码先写输出电平再设方向，项目也是这样，所以引脚变成输出时已经是想要的电平：背光关、功放关、屏幕和触摸在复位中。

| 引脚 | 端口 | 网络 | 用途 |
|---|---|---|---|
| 5 | PD0 | EXIO0 | TP_RST |
| 12 | PC5 | EXIO1 | LCD_RST |
| 16 | PD2 | EXIO2 | RTC_INT |
| 17 | PD3 | EXIO3 | PA_CTRL，经 R19 0Ω 接 NS4150B 的 CTRL，R21 10kΩ 下拉 |
| 18、19 | PD4、PD5 | EXIO4、EXIO5 | 只到测试点 TP3、TP4 |
| 20 | PD6 | EXIO6 | J8-9 |
| 13 | PC6 | EXIO7 | J8-10 |
| 11 | PC4 | EXIO_ADC | BAT_ADC |
| 10 | PC3 | EXIO_PWM | LCD_BL |
| 9、8 | PC2、PC1 | EXIO_SCL、EXIO_SDA | I2C |
| 15 | PD1 | SWDIO | 测试点 TP1 |
| 1 | PD7/NRST | EXIO_RESET | 接 ESP32-C5 的 CHIP_PU |

厂商的 `ch32v003.md` 写 EXIO2 保留、EXIO4/5 是 SHTC3 的接口，和原理图不同：原理图上 EXIO2 是 RTC_INT，SHTC3 接在 GPIO0/1 的 I2C 上，BSP 也在这条总线上找 SHTC3。

## 音频

ES8311 的 MCLK 脚没有连接，codec 从 SCLK 取时钟，项目和 BSP 都不送 MCLK（`use_mclk = false`）。codec 的 PVDD、DVDD 经 R18 接 3V3，AVDD 经 R20 接 A3V3。OUTP、OUTN 经 100nF 和 150kΩ 接 NS4150B 的输入，功放输出到 2 针的喇叭座 H1。功放由 3V3 供电，CTRL 是 EXIO3，下拉，默认关闭。没有耳机插座。I2S 连接如下：

| ES8311 | GPIO | 说明 |
|---|---|---|
| CCLK、CDATA | GPIO1、GPIO0 | 共用 I2C |
| SCLK | GPIO24 | 22pF 到 AGND |
| LRCK | GPIO25 | 22pF 到 AGND |
| ASDOUT | GPIO26（原理图） | codec 输出，项目不用 |
| DSDIN | GPIO27（原理图） | codec 的 DAC 输入，R44 10kΩ 上拉 |

**原理图和 BSP 在两根数据线上相反。** 原理图的 GPIO 表和 codec 部分都是 GPIO27 接 DSDIN、GPIO26 接 ASDOUT；厂商 BSP 的 `BSP_I2S_DOUT` 是 GPIO26（注释“MCU to codec DAC”），`BSP_I2S_DIN` 是 GPIO27。项目暂时按 BSP 用 GPIO26 输出，哪一个对要靠放音确认，见“待确认”。

厂商打开音频的顺序是：功放关、打开 codec、设音量、取消静音、打开功放；关闭时先静音、关功放、再关 codec。项目在 codec 打开以后打开功放。ES8311 只有一个 DAC，取左声道；NS4150B 驱动一个喇叭。

麦克风 MIC1 是模拟麦克风，经 C64 接 MIC1P。它的供电由三焊盘的 R53 选择 A3V3 或 ADCVREF，原理图看不出焊在哪一边。项目不用麦克风。

## TF 卡

TF 卡座 TF1 和屏幕共用 SPI2：CLK GPIO6、CMD（MOSI）GPIO7、D0（MISO）GPIO8、CD/D3（CS）GPIO23。六根信号线都有 10kΩ 上拉到 3V3（R32-R35、R73、R74）。卡座的 VDD 直接接 3V3，没有开关。卡检测脚和地接在一起，没有插卡检测。

卡上电以后处于 SD 模式，会把总线上发给屏幕的数据当成命令。按 SD 物理层简化规范 7.2.1 节，CS 为高时送至少 74 个时钟，再在 CS 为低时发 CMD0，卡就进入 SPI 模式，CS 为高时不再理会总线。所以 `board::init()` 初始化 SPI2 以后先做这一步，再让屏幕使用总线。厂商 BSP 是先挂载 TF 卡再初始化屏幕。平台以 SDSPI 挂载，20MHz：SPI 模式下的高速要发 CMD6，IDF 的驱动不发。

## RTC

PCF85063A 接 32.768kHz 晶振 Y1，INT 是 EXIO2，不接 ESP32-C5 的 GPIO，项目不用。VDD 由 3V3 经 D5、由 J7 的 1 脚经 D6 供电，J7 另经 D4 和 R22 4.7kΩ 从 3V3 充电，原理图没有写 J7 接什么电池。板上没有超级电容。

读时间时，项目检查 Control_1（00h）的 STOP 和 12_24 位，以及秒寄存器的 OS 位（数据手册 7.2 和 7.3 节）。写时间时先置 STOP，写完再清除，写秒寄存器会清除 OS。星期寄存器是 0 到 6 的数，排在日之后。

## 电源

USB-C 口 J2 的 CC1、CC2 各有 5.1kΩ 下拉，板子只作为受电端。VBUS 经 D3 供给充电芯片 ETA6098 和系统电源，ETA6098 给 J1 上的单节锂电池充电，充电电流按原理图上的表由 R2 820kΩ 设为 0.27A，STAT 驱动绿色的 LED1。系统电源 VCC 在有 USB 时经 D2 来自 VBUS，没有 USB 时经 Q2（DMP2066LSN）来自电池。

电源键 Key1 接 U2（ECJ23001），U2 的 OUT 是 PWR_EN，经 Q4、Q5 反相成 PWR_N_EN，打开 MP1605（3V3，最大 2A）和 RT9193（A3V3）。3V3 经 R37 0Ω 成为模组的 ESP_3.3V，同时给屏幕、TF 卡、CH32V003、传感器、codec 的数字电源和功放供电，红色的 LED2 指示 3V3。

ESP32-C5 的 GPIO 读不到也控制不了 PWR_EN 和 Key1，所以固件不能保持或切断电源。电池电压 VBAT 经 R7 200kΩ、R10 100kΩ 分压成 BAT_ADC，由 CH32V003 采样，按厂商代码换算为 `raw × 3.3 / 1023 × 3`。项目还没有读电池电压。

## GPIO 总表

| GPIO | 网络 | 连接 |
|---|---|---|
| 0、1 | IIC_SDA、IIC_SCL | 共用 I2C：触摸、RTC、CH32V003、ES8311、QMI8658、SHTC3、J5，2kΩ 上拉 R54、R55 |
| 2、3 | GPIO2、GPIO3 | J8-7、J8-8，空闲；strapping |
| 4 | IMU_INT1 | QMI8658 |
| 5 | TP_INT | 触摸中断，项目不用 |
| 6、7、8 | LCD_SCK、LCD_MOSI、LCD_MISO（SDIO_SCLK、SDIO_MOSI、SDIO_MISO） | 屏幕和 TF 卡共用的 SPI2，10kΩ 上拉；GPIO7 是 JTAG 选择的 strapping，只在 eFuse 打开时有效 |
| 9 | LCD_DC | 屏幕 |
| 10 | LCD_CS | 屏幕 |
| 11、12 | UART_TXD、UART_RXD | UART0，J6 |
| 13、14 | USB_N、USB_P | USB-C 口，各串 22Ω；也引到 J8-3、J8-4 |
| 23 | SDIO_CS | TF 卡片选，10kΩ 上拉 |
| 24、25 | I2S_SCLK、I2S_LRCK | ES8311；GPIO25 是 SDIO 的 strapping |
| 26 | I2S_ASDOUT | ES8311，见“音频”；strapping |
| 27 | I2S_DSDIN | ES8311，R44 10kΩ 上拉，见“音频”；strapping |
| 28 | BOOT0 | BOOT 键 Key2，R45 10kΩ 上拉；strapping |
| CHIP_PU | CHIP_PU | RESET 键 Key3、CH32V003 的 NRST、J8-12，R39 10kΩ 上拉 |

按 ESP-IDF v6.1 的 `io_mux_reg.h`，启动模式由 GPIO28、27、3、2、26 决定：GPIO28 为高时从 SPI flash 启动，GPIO28 为低、GPIO27 为高时进入 UART0 或 USB 下载模式。可以自由使用的 GPIO 只有 J8 上的 GPIO2 和 GPIO3，它们都是 strapping 脚。

排针 J8 是 12 针：GND、USB_5V、USB_N、USB_P、GND、3V3、GPIO2、GPIO3、EXIO6、EXIO7、BOOT0、CHIP_PU。J5 是 I2C（SCL、SDA、3V3、GND），J6 是 UART0（RXD、TXD、3V3、GND）。

## 烧录和恢复

USB-C 口直接接 ESP32-C5 的 USB-Serial/JTAG（GPIO13/14），板上没有 USB 转串口芯片。烧录、日志和测试的二进制传输都走这个口，idf.py 和 esptool 能通过它自动进入下载模式和复位。手动进入下载模式：按住 BOOT，按一下 RESET，再松开 BOOT。UART0 只引到 J6。

恢复出厂固件时，用 esptool 把本地的整片备份从地址 0 写回，或者写厂商仓库 `firmware/` 里的出厂镜像，写法见厂商的 `firmware/README.md`。

## 待确认的硬件约束

- ES8311 的 DSDIN 接 GPIO26 还是 GPIO27：原理图是 GPIO27，BSP 是 GPIO26，项目用 GPIO26。喇叭放音未实测。
- 显示方向、颜色和触摸坐标还没有上板确认。
- 背光 PWM 的频率没有资料，CH32V003 复位期间的背光状态没有确认。
- CH32V003 的 PD7 接 CHIP_PU，它按 NRST 还是 GPIO 工作取决于它的选项字节，资料里没有写。
- ECJ23001 没有数据手册，电源键的上电默认状态、长按和短按的行为不清楚。
- J7 接的后备电池类型没有资料。
- 麦克风供电跳线 R53 焊在哪一边没有核对。
- QMI8658 的地址：BSP 用 `QMI8658_ADDRESS_HIGH`，这个宏在仓库外的组件里；Arduino 的 `qmi8658c.h` 里它是 0x6B。项目不用 IMU。

## 来源

- Waveshare ESP32-C5-Touch-LCD-2.8 原理图，仓库提交 `186eb7f`，第 1 页。
- 厂商仓库同一提交：`README.md`、`docs/ch32v003.md`、`docs/getting-started-idf.md`（下载模式），`examples/factory/components/esp32_c5_touch_lcd_2_8/`（BSP），`examples/factory/components/waveshare__esp_lcd_touch_cst3530/`，`examples/arduino/libraries/board_config/src/`（`Display_ST7789.cpp`、`Board_IO.cpp`），`examples/esp-idf/02_sd_card`、`07_audio`、`08_battery_voltage`。
- ST7789V2 数据手册 V1.0：7.4.2 节串口时序、复位时序、RAMCTRL 和 FRCTRL2 命令。
- ES8311 数据手册 Rev 10.0：第 5 节 I2C 地址、引脚说明。
- PCF85063A 数据手册 Rev 7.3：7.2、7.3 节寄存器，I2C 地址。
- SD Physical Layer Simplified Specification：7.2.1 节进入 SPI 模式。
- ESP-IDF v6.1：`components/soc/esp32c5/include/soc/soc_caps.h` 和 `soc.h`（单核、共用缓存、片内 RAM 范围），`components/soc/esp32c5/register/soc/io_mux_reg.h`（strapping），`components/esp_lcd/src/esp_lcd_panel_st7789.c`（初始化命令和 RAMCTRL），`components/esp_hw_support/port/esp32c5/cpu_region_protect.c`（PSRAM 可执行）。

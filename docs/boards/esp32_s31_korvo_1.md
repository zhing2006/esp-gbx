# 乐鑫 ESP32-S31-Korvo-1

项目代号是 `esp32_s31_korvo_1`，板级名称是 Espressif ESP32-S31-Korvo-1。这是乐鑫的 ESP32-S31 多媒体开发板，主控是 ESP32-S31-WROOM-3 模组，板上有 ES8389 codec、两路 NS4150B 功放、TF 卡座、DVP 摄像头和 USB-A 主机口。套件里的屏幕是 ESP32-S3-LCD-EV-Board-SUB3：4.3 英寸 800×480 RGB 屏，显示驱动 ST7262E43，触摸 GT1151，插在主板的 LCD 接口 J14 上。

项目固件只编入 GBA，接入了屏幕、触摸、音频和 SD 存储，USB-A 口打开了 USB 手柄。屏幕的背光不能调，见“背光”。板上的 ADC 按键和摄像头项目没有使用。

## 型号和版本

用户指南描述的是 V1.1。按它的“硬件版本”一节，V1.0 和 V1.1 只差在阻焊颜色、PCB 尺寸和屏幕的安装方式（V1.1 的屏叠在主板上方），GPIO 分配没有变。主板原理图的标题栏是 Rev 1.1，日期 2026-05-12、05-13。手上这块板的 PCB 丝印版本没有核对。

手上这块板经 CP2102N 串口读取，esptool v5.5.0 报告 ESP32-S31 revision v0.0、40MHz 晶振，特性一栏写的是 Wi-Fi 6、BT 5.4 (LE)、IEEE802.15.4、Dual Core + LP Core、300MHz。flash 的 JEDEC ID 为 `46 40 18`，容量 16MB。出厂固件的启动日志报告 16MB 八线 PSRAM，厂商 ID 0x1a，运行在 250MHz，CPU 320MHz。

SUB3 屏板的原理图标题栏是 ESP32-S3-LCD-EV-Board-SUB3 Rev 1.1，屏幕一栏写着 `LCD_800X480_4.3 inch_WSK`。乐鑫给出的屏幕规格书是 WKS43246 V0.0（2023-01-05），规格书的驱动 IC 一栏空着；ST7262E43 这个型号来自 ESP32-S3-LCD-EV-Board 用户指南。出厂固件读出的触摸 IC 版本是 `GT1158_000101(Patch)_0102(Mask)`。

## 资料

资料来自乐鑫的 esp-dev-kits 文档和仓库，下表是整理时用到的原始文件，校验值是 SHA-256：

| 原文件 | 来源 | 内容 | SHA-256 |
|---|---|---|---|
| `esp32-s31-korvo-1-schematics.pdf` | `https://dl.espressif.com/schematics/` | 主板原理图 V1.1，4 页 | `6841cdb55bc4749376a4309365a0efeb45b2cb4cc882a48008bf592d029ae0ec` |
| `SCH_ESP32-S3-LCD-EV-Board-SUB3_V1.1_20230315.pdf` | `https://dl.espressif.com/dl/schematics/` | SUB3 屏板原理图，1 页 | `b3e5ceacde87bffa134ff05ebe391a1eea5c693d6097ca2e1e93ad82f252b189` |
| `ST7262E43_V0.1_201905.pdf` | 同上 | ST7262E43 数据手册 V0.1：RGB 接口模式、输入时序、寄存器 | `d73037ad729104558718f34d76825eca576589ce3614e082637509f1ed680bb9` |
| `WKS43246_SPEC_V0.0.pdf` | 同上 | 屏幕模组规格书：接口定义、背光、触摸、输入时序 | `8f28baf17a52aafb903781bfbe3d08e8c1cd628235b19df1d10480e307f32b25` |
| ESP32-S31-Korvo-1 用户指南 | esp-dev-kits 文档，提交 `bd4d4ac` | 组件、引脚分配表、硬件版本、下载方式 | 网页，无校验值 |
| `examples/esp32-s31-korvo/examples/` | espressif/esp-dev-kits，提交 `bd4d4ac18657e73583af3d7c0b0a23c3cddf3735` | 板级 BSP `common_components/esp32_s31_korvo` 和出厂例程 `factory_demo` | 见下表 |

两份数据手册的每页都标着厂商的 CONFIDENTIAL，没有裁图入库，用到的内容整理在本文里。原理图按电路区域裁成 300dpi PNG，保存在 `refs/boards/esp32_s31_korvo_1/schematic/`，`crops.json` 记录原文件的 SHA-256、页码和以 PDF 点为单位的裁剪坐标。

| 图片 | 原文件和裁剪内容 |
|---|---|
| `01-module.png` | 主板第 3 页：模组 U10 的焊盘和网络、各接口的 0Ω 电阻、I2C 上拉 R18、R19 |
| `02-tf-card.png` | 同页：TF 卡座 J7、供电开关 Q1 和 ESD 器件 |
| `03-lcd-connector.png` | 同页：屏幕接口 J14 |
| `04-boot-reset.png` | 同页：BOOT 键 SW2 和 RESET 键 SW1 |
| `05-usb-uart.png` | 第 2 页：UART 口 J1、CP2102N 和自动下载电路 |
| `06-usb-host-power-in.png` | 同页：USB-A 主机口 J2、TPS2051C 和 PWR 口 J5 |
| `07-power-rails.png` | 同页：TLV62569 降压和各路 LDO |
| `08-codec.png` | 第 4 页：ES8389 |
| `09-amplifiers.png` | 同页：两个 NS4150B 和喇叭接口 |
| `10-key-array.png` | 同页：ADC 按键的分压电阻 |
| `11-sub3-connector.png` | SUB3 原理图：接主板的 J1 |
| `12-sub3-backlight.png` | 同页：背光升压 ETA1611 |
| `13-sub3-lcd-fpc.png` | 同页：屏幕的 40 针 FPC J3 |
| `14-sub3-touch.png` | 同页：触摸的 6 针 FPC J2 |
| `15-sub3-low-bits.png` | 同页：低位数据线的 0Ω 电阻 |

参考源码是 esp-dev-kits 固定提交里的原文件，文件头是 SPDX Apache-2.0：

| 文件 | 原文件 | 内容 |
|---|---|---|
| `esp32_s31_korvo.h.txt` | `common_components/esp32_s31_korvo/include/bsp/esp32_s31_korvo.h` | 屏幕、触摸、摄像头、TF 卡、音频、按键和 LED 的引脚 |
| `display.h.txt` | `common_components/esp32_s31_korvo/include/bsp/display.h` | RGB 屏的分辨率、像素时钟和时序宏 |
| `button.h.txt` | `common_components/esp32_s31_korvo/include/bsp/button.h` | ADC 按键的编号和电压 |
| `esp32_s31_korvo.c.txt` | `common_components/esp32_s31_korvo/esp32_s31_korvo.c` | RGB 面板、GT1151、TF 卡供电和挂载、ES8389、ADC 按键的初始化 |
| `factory_demo-sdkconfig.defaults.txt` | `factory_demo/sdkconfig.defaults` | 出厂例程的 flash、PSRAM 和 CPU 配置 |

## 主控和存储

ESP32-S31-WROOM-3 的模组和慧勤智远 ESP32-S31 CB 相同，16MB flash、16MB 八线 PSRAM。项目配置和那块板一样：CPU 320MHz，flash QIO 80MHz，PSRAM 八线 250MHz。出厂固件的引导程序同样以 QIO 80MHz 启动这颗 flash。

控制台是 UART0（GPIO58 TX、GPIO59 RX，接 CP2102N），波特率 460800。S31 的 USB-Serial/JTAG 在 GPIO33/34 上，这块板子把这两个脚用作 LCD_R4、LCD_R5，所以屏幕工作时没有 USB 控制台，`CONFIG_ESP_CONSOLE_SECONDARY_NONE`。

## IC

| 位号 | 型号 | 用途 |
|---|---|---|
| U10 | ESP32-S31-WROOM-3 | 主控模组 |
| U15 | CP2102N | USB 转串口，DTR/RTS 经 U16（BC847BDW1T1G）控制 EN 和 GPIO61 |
| U5 | ES8389 | 音频 codec |
| U9、U8 | NS4150B | 左、右声道 D 类功放 |
| U6、U7 | LMA3729T381-OY3S | 模拟麦克风 |
| U11 | TLV62569 | 5V 转 3.3V 降压，给模组和外设供电 |
| U2 | ME6211C33M5G-N | 音频 3.3V LDO（Aud_3V3） |
| U12、U13 | ME6211C28M5G-N、ME6211C15M5G-N | 摄像头的 2.8V 和 1.5V LDO |
| U14 | TPS2051CDBVR | USB-A 口的 VBUS 开关，限流 500mA |
| U3 | WS2812B | RGB 指示灯，经 Q2（LBSS138LT1G）电平转换 |
| J4 上 | OV3660 | 摄像头模组，出厂固件读出 PID 0x3660 |
| SUB3 U1 | ETA1611 | 背光升压 |
| SUB3 屏上 | ST7262E43、GT1151 | 显示驱动和电容触摸 |

U1（1.8V LDO）、U4（SPI NAND）和相关电阻不贴装。

## 屏幕和触摸

### 连接

屏幕接口 J14 是 2×20 排针，SUB3 的 J1 插在上面。数据线是 RGB565 共 16 根，主板上每根线串一个 0Ω 电阻（`01-module.png`）：

| 信号 | GPIO | 信号 | GPIO |
|---|---|---|---|
| B3 | 8 | G6 | 17 |
| B4 | 9 | G7 | 18 |
| B5 | 10 | R3 | 19 |
| B6 | 11 | R4 | 33 |
| B7 | 12 | R5 | 34 |
| G2 | 13 | R6 | 35 |
| G3 | 14 | R7 | 36 |
| G4 | 15 | PCLK | 40 |
| G5 | 16 | DE（LCD_H_EN） | 43 |
| HSYNC | 44 | VSYNC | 45 |
| I2C SDA | 0 | I2C SCL | 1 |

J14 还引出了 LCD_MOSI（GPIO60）、LCD_CS（GPIO38）和 LCD_SCK（GPIO61），给需要 SPI 初始化的屏用；SUB3 的 J1 没有接这三个脚，ST7262E43 也不需要初始化序列。J14 的 35、36 脚是 VCC_3V3，39、40 脚是 VCC_5V。

SUB3 上屏幕 FPC 的 R0-R2、G0-G1、B0-B2 不接 GPIO，经 0Ω 电阻 R16、R15、R14 分别并到 R3、G2、B3，也就是重复 RGB565 每个分量的最低位，接地的 R19、R18、R12 不贴装（`15-sub3-low-bits.png`）。DISP 经 R10 10kΩ 上拉到 LCD_VCC（3.3V），屏一上电就退出待机。屏幕的 VDD 是 3.3V，背光升压由 5V 供电。

### 显示配置

屏幕由 S31 的 LCD_CAM 以 RGB 并口驱动，没有初始化序列，也没有 ID 可读。ST7262E43 数据手册 7.3 节按 HSYNC、VSYNC、DE 是否接地选择 SYNC、DE 或 SYNC-DE 模式；这块板三根线都由 S31 驱动。WKS43246 规格书第 8 节写明 DCLK 上升沿锁存数据，所以 S31 在 PCLK 下降沿输出数据（`pclk_active_neg`）。

ST7262E43 数据手册 7.3.4 节的时序表：DCLK 23-27MHz，典型 25MHz；一行 808-896 个 DCLK，后肩 4-48、前肩 4-48、同步宽度 2-8；一帧 488-504 行，后肩 4-12、前肩 4-12、同步宽度 2-8。7.3.1 节的时序图里，后肩从同步脉冲开始算，ESP-IDF 的 `hsync_back_porch` 不含同步脉冲。WKS43246 规格书第 9 节的范围相同，只是 PCLK 的上限写作 30MHz。

乐鑫 BSP 的 `display.h` 用 26MHz，HSYNC 宽 1、后肩 40、前肩 20，VSYNC 宽 1、后肩 10、前肩 5，同步宽度小于上表的下限；出厂固件的日志打印的是 20MHz。

项目让屏幕接近 GB/GBA 的帧率 4194304/70224Hz。LCD_CAM 的像素时钟是时钟源经小数分频（分母不超过 64）再二分频，见慧勤智远 S31 CB 的[显示配置](wks_s31_cb.md#显示配置)。项目用 PLL_F160M ÷ (3 + 1/33) ÷ 2 = 26.4MHz。IDF 的参数是 HSYNC 宽 4、后肩 39、前肩 34，VSYNC 宽 4、后肩 8、前肩 12；按数据手册的算法，一行的后肩 43、前肩 34、共 877 个 DCLK，一帧的后肩 12、前肩 12、共 504 行，都在上表的范围里。算出的帧率是 59.727426Hz，比 GB 帧率低 1.3ppm。43 和 12 也是 ST7262E43 的 H_BLANKING（17h）和 V_BLANKING（18h）的默认值，也就是 SYNC 模式下数据开始的位置，所以屏幕不看 DE 时画面也不会错位。

帧缓冲放在 PSRAM，RGB565，两个。

面板按扫描方向是横屏。掌机用法是以面板扫描方向的左边为上，UI 是 480×800 竖屏，`Rotation::cw270`。

### 背光

背光由 SUB3 上的 ETA1611（U1）从 LCD_5V 升压驱动，LED 电流由 R5（10Ω）设定。ETA1611 的 EN 经 R3（10kΩ）和 R4（10Ω）接 LCD_5V，J1 和 J14 上都没有背光控制线，所以板子有 5V 时背光就亮，不能关也不能调光。`board::Display::backlight` 为 false，设置菜单里的 BRIGHTNESS 不可用。上电到第一帧画面之间，屏上可能显示面板自己的内容。

### 触摸

GT1151 接在 SUB3 的 6 针 FPC J2 上。I2C 的 SDA、SCL 经 J1 的 5、7 脚接到主板 GPIO0、GPIO1，SUB3 上的上拉 R1、R2 不贴装，上拉是主板的 R18、R19（4.7kΩ 到 3.3V）。RST 经 R8 10kΩ 上拉到 LCD_3V3，电容 C12 不贴装；INT 只接到测试点 TP1。所以主机不能复位 GT1151，也收不到它的中断，平台轮询坐标。主板原理图在 J14 旁标注的 I2C 地址是 0x14，和乐鑫 BSP 的 `ESP_LCD_TOUCH_IO_I2C_GT1151_CONFIG()` 相同。

项目和慧勤智远 S31 CB 一样用 `esp_lcd_touch_gt911` 读坐标：esp-bsp 的 `esp_lcd_touch_gt1151` 驱动读的也是 0x814E 起的状态和坐标寄存器，产品 ID 在 0x8140。WKS43246 规格书第 6 节写的触摸分辨率是 800×480、最多 5 点，坐标和面板扫描坐标相同，跟随显示的旋转一起换算。

## 音频

ES8389 由 Aud_3V3 供电（U2，EN 经 R42 接 VCC_5V，常开）。原理图标注的 I2C 地址是 0x20（8 位写法），7 位地址是 0x10。它的 I2C 经 R25、R26（33Ω）接 GPIO0、GPIO1，和触摸、摄像头共用一条总线。I2S 连接见原理图第 3、4 页：

| 信号 | GPIO | 说明 |
|---|---|---|
| MCLK | 2 | 经 R95 0Ω |
| SCLK | 3 | 经 R96 0Ω |
| LRCK | 4 | 经 R97 0Ω |
| DSDIN | 5 | S31 输出到 codec，经 R98 0Ω |
| ASDOUT | 6 | codec 输出到 S31，项目不用 |
| PA_CTRL | 7 | 两个 NS4150B 共用，高电平开，R83、R87 下拉 |

项目送 256 倍采样率的 MCLK，和出厂固件相同。两个 NS4150B 由 VCC_5V 经 R86、R90 供电。左声道接 U9 和喇叭接口 J13，右声道接 U8 和 J15，都是 PH2.0 两针，没有耳机插座。

## TF 卡

TF 卡座 J7 接在 S31 SDMMC slot 0 的 IOMUX 引脚上：CLK GPIO24、CMD GPIO25、D0-D3 GPIO20-23，每根线串 0Ω（R7、R65-R69）。同一组 GPIO 也通往不贴装的 SPI NAND，那一路的电阻 R22、R23、R1-R4 不贴装。CMD 和 DAT 线没有外部上拉，项目打开内部弱上拉。卡座的 CD 脚接 SD_DET，只到测试点 TP5，没有插卡检测。

卡的电源 SD_3V3 由 P 沟道 MOSFET Q1（AO3401A）从 VCC_3V3 接通，栅极是 SD_CTRL（GPIO39 经 R146），R138 10kΩ 下拉，上拉 R70 不贴装。所以 GPIO39 不驱动时卡也有电，低电平接通。GPIO39 同时是 strapping 引脚（Boot Mode 1）。`board::init()` 把 GPIO39 设为输出低电平。卡和 IO 都是 3.3V，不支持 1.8V 信号；项目按 4 线 high-speed（请求 40MHz）挂载。

## 按键和其他接口

KEY Array 的四个按键经分压接 GPIO42（BT_ARRAY_ADC，R78 0Ω），R77 10kΩ 上拉到 VCC_3V3。原理图标注的电压是 SET 1.87V、MODE 1.34V、VOL- 0.82V、VOL+ 0.38V，下拉电阻依次是 R79 13kΩ、R80 6.8kΩ、R81 3.3kΩ、R82 1.3kΩ；用户指南把 MODE 写作 PLAY。乐鑫 BSP 用 ADC1 通道 0 读它。项目没有使用这些按键。BOOT 键 SW2 接 GPIO61，RESET 键 SW1 接 EN。

USB-A 口 J2 接模组的 USB_DP、USB_DM（经 R8、R9），VBUS 由 TPS2051C 从 VCC_5V 接出，EN 经 R73 10kΩ 上拉，所以板子有电时 VBUS 就有电。项目在这个口上接 USB 手柄，打开了 `GBX_USB_GAMEPAD`，并用 `CONFIG_USB_HOST_DWC_DMA_CAP_MEMORY_IN_PSRAM` 把 USB 控制器的 DMA 缓冲放在 PSRAM。手柄在这个口上未实测。

摄像头接口 J4 上是 OV3660 模组：D0-D7 为 GPIO46-53，PCLK GPIO54，XCLK GPIO55，VSYNC GPIO56，HREF GPIO57，SCCB 与触摸共用 GPIO0/1。项目不用摄像头。WS2812 的数据线是 GPIO37。

## 电源

两个 USB-C 口都能供电：J1（UART 口，CP2102N）和 J5（PWR 口，只供电）。两路 5V 各经一个 AO3401A 和 LMBT3906DW1T1G 组成的防倒灌开关并成 VCC_IN，再经电源开关成为 VCC_5V。TLV62569（U11）的 EN 经 R11 接 VCC_5V，上电即输出 VCC_3V3。用户指南写明，喇叭大功率工作并且 USB-A 口带设备时，总输入电流要到 3A，应另接一根供电线。

## GPIO 总表

| GPIO | 网络 | 连接 |
|---|---|---|
| 0、1 | CAM_I2C、LCD_I2C SDA/SCL | 共用 I2C，4.7kΩ 上拉；ES8389、GT1151、OV3660 |
| 2-6 | I2S_MCLK、SCLK、LRCK、DSDIN、SDOUT | ES8389 |
| 7 | PA_CTRL | 两个 NS4150B |
| 8-19 | LCD_B3-B7、G2-G7、R3 | 屏幕 |
| 20-25 | SD_D0-D3、SD_CLK、SD_CMD | TF 卡，SDMMC slot 0 |
| 33-36 | LCD_R4-R7 | 屏幕；33、34 也是 USB-Serial/JTAG |
| 37 | WS2812_CTRL | RGB 指示灯 |
| 38 | LCD_CS、GM_FK | J14 的 SPI 片选；到摄像头的 R46 不贴装；strapping（Boot Mode 0） |
| 39 | SD_CTRL | TF 卡供电，低电平开；strapping（Boot Mode 1） |
| 40 | LCD_PCLK | 屏幕；strapping（Boot Mode 2） |
| 42 | BT_ARRAY_ADC | ADC 按键 |
| 43-45 | LCD_H_EN、LCD_H_SYNC、LCD_V_SYNC | 屏幕 |
| 46-57 | DVP 数据、PCLK、XCLK、VSYNC、HREF | 摄像头 |
| 58、59 | TX0、RX0 | CP2102N，控制台 |
| 60 | LCD_MOSI | J14 的 SPI 数据，SUB3 不用；strapping（Boot Mode 3） |
| 61 | LCD_SCK、BOOT | BOOT 键和自动下载电路；strapping（Boot Mode 4） |

strapping 的说明来自用户指南的引脚分配表。

## 烧录和恢复

烧录和日志都走 UART 口 J1 的 CP2102N。DTR、RTS 经 U16 控制 EN 和 GPIO61，idf.py 能自动进入下载模式和复位。手动进入下载模式：按住 BOOT，按一下 RESET，再松开 BOOT。

ESP-IDF v6.1 里 S31 是 preview target，构建目录第一次选择 target 时要用 `idf.py --preview set-target esp32s31`。idf.py 给 S31 生成的烧录命令带 `--no-stub`。用 esptool 的 stub 读整片 flash 时，921600 和 460800 都在中途报 `Invalid head of packet`，230400 能读完。

出厂固件的完整备份保存在本地 `roms/firmware/esp32_s31_korvo_1/`，不入库，校验值和读取方法记在同目录的 `README.txt` 里。恢复时把它写到 0x0：

```sh
eim run 'esptool --chip esp32s31 -p PORT -b 460800 --no-stub write-flash 0x0 roms/firmware/esp32_s31_korvo_1/factory-full-16MB.bin' v6.1
```

出厂例程的源码在 esp-dev-kits 的 `examples/esp32-s31-korvo/examples/factory_demo`，也可以重新编译。

## 待确认的硬件约束

- 手上这块板的 PCB 版本没有核对丝印。
- USB 手柄在 USB-A 口上未实测。
- 屏幕工作在 SYNC-DE 还是 SYNC 模式，资料里没有写屏上模式脚的接法。
- ETA1611 的数据手册不在资料里，背光 LED 电流没有算。
- esptool 列出 300MHz，项目和出厂例程都跑 320MHz，依据待查，和慧勤智远 S31 CB 相同。
- stub 读 flash 在 460800 以上出错的原因没有查。

## 来源

- ESP32-S31-Korvo-1 原理图 V1.1，第 2-4 页。
- ESP32-S31-Korvo-1 用户指南（esp-dev-kits 提交 `bd4d4ac`）：组件、引脚分配、硬件版本、下载方式。
- ESP32-S3-LCD-EV-Board-SUB3 原理图 V1.1。
- Sitronix ST7262E43 数据手册 V0.1：7.3 节 RGB 接口模式和时序，8.2 节 H_BLANKING、V_BLANKING。
- WKS43246 规格书 V0.0：第 6 节触摸、第 8 节接口定义、第 9 节输入时序。
- esp-dev-kits 提交 `bd4d4ac18657e73583af3d7c0b0a23c3cddf3735` 的 BSP `esp32_s31_korvo` 和出厂例程 `factory_demo`。
- esp-bsp 的 `components/lcd_touch/esp_lcd_touch_gt1151/esp_lcd_touch_gt1151.c`：寄存器地址。
- ESP-IDF v6.1：`components/esp_hal_lcd/lcd_hal.c` 和 `components/hal/hal_utils.c`（像素时钟分频）。

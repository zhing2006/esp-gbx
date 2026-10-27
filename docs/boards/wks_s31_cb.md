# 慧勤智远 ESP32-S31 CB V1.1

项目代号是 `wks_s31_cb`，板级名称是 Waiken-Smart ESP32-S31 CB V1.1。这是东莞市慧勤智远科技有限公司（Waiken-Smart，丝印和资料里写作 WKS）的 ESP32-S31 小系统板，主控是 ESP32-S31-WROOM-3-N16R16V 模组。本项目配的屏幕是同一厂家的 4.3 英寸 RGB 触摸屏 WKS43WV067-WCT，800×480，触摸控制器是 GT1151Q，屏幕的 40 针 FPC 插在板子正面的 RGB 接口上。

## 型号和版本

板子的原理图标题栏写着 ESP32-S31 CB V1.1、设计时间 2026-9-17；用户手册是 V1.0，日期 2026-10-17。

手上这块板经 CH343P 串口读取，esptool v5.5.0 报告 ESP32-S31 revision v0.0、40MHz 晶振，特性一栏写的是 Wi-Fi 6、BT 5.4 (LE)、IEEE802.15.4、Dual Core + LP Core、300MHz。flash 的 JEDEC ID 为 `46 40 18`，容量 16MB；PSRAM 16MB。模组丝印是 ESP32-S31-WROOM-3 MAN16R16V，手册写明内部芯片是 ESP32-S31NRV16。

屏幕规格书 WKS43WV067-WCT_SPEC 的修订记录为 V0.0、2024-12-31。触摸控制器的产品 ID 寄存器读出 `"1158"`，补丁版本 01.01，配置版本 0x82，配置里的输出范围是 800×480、最多 5 点；资料包里的配置文件 `BT_GT1151Q_V130_800x480_Config_20200915_PP.cfg` 开头也是 `0x82, 0x20, 0x03, 0xE0, 0x01, 0x05`，即同一版配置。

出厂固件没有备份。厂商的例程固件在资料包 `3. 程序源码/1，基础例程/基础例程固件.zip` 里。

## 资料

资料来自用户提供的“慧勤智远 ESP32-S31 CB V1.1小系统板”目录。下表是整理时用到的原始文件，校验值是 SHA-256：

| 原文件 | 内容 | SHA-256 |
|---|---|---|
| `2. 原理图/ESP32-S31 CB V1.1_SCH.pdf` | 原理图，4 页 | `9a35fbb8e86e4a03575819b001c0d5f454d44d90c8d77ceb8cf4637e4751d1c1` |
| `1. 用户手册/ESP32-S31 CB V1.1_用户手册_V1.0.pdf` | 用户手册：模组型号、资源表、引脚分配表、下载方式 | `63937a42b5ac81dfaa36608e4a57af9c8925dbc386eaba4618c834d39e8c9a0d` |
| `3. 程序源码/1，基础例程/basic__routines.zip` | ESP-IDF 6.1 例程，屏幕、触摸、SD 卡、音频的引脚和配置取自 `12_rgblcd_touch_screen`、`15_sdcard`、`19_music` | `1256d83dd8a65632d9c90d84d82b87331335cfa924711680aaf91da8d9d8fa8f` |
| `8. RGB接口触摸显示屏资料/WKS43WV067-WCT产品资料/WKS43WV067-WCT_SPEC_V0.0.pdf` | 屏幕规格书：接口定义、背光电路、输入时序 | `0c874e6784253aadb5869b9b8c6a5e7dc966efed01e81b3b95c029f02a2c87db` |
| `8. RGB接口触摸显示屏资料/WKS43WV067-WCT产品资料/GT1151Q编程指南_20140717_Rev.01.pdf` | GT1151Q 编程指南 Rev.01：地址、寄存器、上电和复位时序 | `81bbbb9a0e76e99dd4dd789574fed29b23f3752e88dd31480ea794b92fbf499b` |

资料包里还有 4.3、5.0、7.0 寸 RGB 屏和 2.4、3.5 寸 SPI 屏的资料，以及 OV2640、OV5640 摄像头模块的资料。本项目只用 4.3 寸的 WKS43WV067-WCT。

开发需要的图片和参考源码保存在 `refs/boards/wks_s31_cb/`。屏幕规格书每页标着 WKS CONFIDENTIAL，没有裁图入库，用到的内容整理在本文里。原理图按电路区域裁成 300dpi PNG，保存在 `schematic/`，`crops.json` 记录原文件的 SHA-256、页码和以 PDF 点为单位的裁剪坐标。

| 图片 | 原文件和裁剪内容 |
|---|---|
| `01-module.png` | 原理图第 1 页：模组 U1 的焊盘和网络、I2S 和 SPI 屏接口的 0Ω 电阻、SD 线的 22Ω 电阻、I2C 上拉 R1、R2 |
| `02-tf-card.png` | 同页：TF 卡座和 ESD 器件 E7-E12 |
| `03-pin-headers.png` | 同页：排针 P1、P2 |
| `04-rgb-connector.png` | 第 2 页：40 针 RGB 屏接口 |
| `05-spi-connector.png` | 同页：20 针 SPI 屏接口 |
| `06-keys.png` | 同页：KEY0 和 RESET，R7 不贴 |
| `07-camera.png` | 同页：DVP 摄像头接口和 LCD_RST 的 R43 |
| `08-rgb-led.png` | 同页：RGB 指示灯和电平转换 Q3 |
| `09-codec.png` | 第 3 页：ES8389 和地址电阻 |
| `10-amplifiers.png` | 同页：两个 NS4150B 和 PA_CTRL |
| `11-audio-ldo.png` | 同页：音频 LDO RT9013 |
| `12-usb-uart.png` | 第 4 页：COM 口、CH343P 和自动下载电路 |
| `13-power.png` | 同页：SY8089A1AAC 降压 |
| `14-usb-power.png` | 同页：PWR 口，以及不贴的 R50、R51 |

参考源码是厂商基础例程的原文件，文件头写着 Waiken-Smart 慧勤智远，没有许可声明：

| 文件 | 原文件 | 内容 |
|---|---|---|
| `rgb_lcd.h.txt`、`rgb_lcd.c.txt` | `12_rgblcd_touch_screen/components/BSP/LCD/` | RGB 引脚、时序和 `esp_lcd_rgb_panel` 配置 |
| `gt9xxx.h.txt`、`gt9xxx.c.txt` | `12_rgblcd_touch_screen/components/BSP/TOUCH/` | 触摸引脚、复位和坐标读取 |
| `sdmmc.c.txt` | `15_sdcard/components/BSP/SDMMC/` | TF 卡的 SDMMC 配置 |
| `myes8389.c.txt`、`myi2s.h.txt`、`myi2s.c.txt` | `19_music/components/BSP/` 的 `MYES8389/`、`MYI2S/` | ES8389 和 I2S 配置 |
| `vendor-config.txt` | `12_rgblcd_touch_screen/sdkconfig` | flash、PSRAM、CPU 频率和控制台的设置摘录 |

## 主控和存储

ESP32-S31-WROOM-3-N16R16V 按手册是 16MB flash、16MB PSRAM、512KB HP SRAM、32KB LP SRAM。项目配置如下：

- CPU 320MHz，和厂商例程的 `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_320` 相同。esptool 在特性里列出的是 300MHz，两者的差别还没有查到依据。
- flash 用 QIO、80MHz。`46` 开头的 JEDEC ID 在 ESP-IDF 的引导程序里按 XMC 的 D 系列处理（`bootloader_flash.c` 的 `is_xmc_chip_strict()`），QE 位由 QIO 表的默认项写入：RDSR2 读、WRSR2 写，状态寄存器 2 的第 1 位。厂商例程同样是 QIO 80MHz。
- PSRAM 是八线模式，250MHz，PSRAM 供电用片内 LDO 的 1.8V，和厂商例程相同。
- 控制台是 UART0（TX0、RX0 接 CH343P），波特率跟仓库其他板子一样用 460800。S31 的 USB-Serial/JTAG 在 GPIO33/34 上，这块板子把这两个脚用作 LCD_R4、LCD_R5，所以屏幕工作时没有 USB 控制台，`CONFIG_ESP_CONSOLE_SECONDARY_NONE`。

## IC

| 位号 | 型号 | 用途 |
|---|---|---|
| U1 | ESP32-S31-WROOM-3-N16R16V | 主控模组 |
| U9 | CH343P | USB 转串口，DTR/RTS 经 Q1、Q2（S8050T）控制 EN 和 BOOT |
| U3 | ES8389 | 音频 codec |
| U4、U5 | NS4150B | 左、右声道 D 类功放 |
| U6 | RT9013-33GB | 音频 3.3V LDO（AUDIO_3V3） |
| U10 | SY8089A1AAC | 5V 转 3.3V DC-DC，给模组和外设供电 |
| MIC1、MIC2 | LMA3729T381-OAS02 | MEMS 麦克风 |
| LED | XL-2121RGBC-2812B | WS2812 兼容的 RGB 指示灯，经 Q3 电平转换 |
| 屏幕上 | MP3302 | 背光升压，见“背光” |
| 屏幕上 | GT1151Q | 电容触摸，读出的产品 ID 是 `1158` |
| 屏幕上 | HX8264-D03、HX8664-B | 资料包里屏幕目录下的源极和栅极驱动数据手册，厂商例程的注释也写着 HX8264；规格书的 Driver IC 一栏空着，屏上实际的驱动 IC 没有核对 |

## 屏幕和触摸

### 连接

RGB 接口（原理图第 2 页）是 40 针，屏幕按规格书的 LCD1 接口定义接入。数据线是 RGB565 共 16 根，接口上 R0-R2、G0-G1、B0-B2 分别并到 R5-R7、G6-G7、B5-B7，低位重复高位：

| 信号 | GPIO | 信号 | GPIO |
|---|---|---|---|
| B3 | 8 | G6 | 17 |
| B4 | 9 | G7 | 18 |
| B5 | 10 | R3 | 19 |
| B6 | 11 | R4 | 33 |
| B7 | 12 | R5 | 34 |
| G2 | 13 | R6 | 36 |
| G3 | 14 | R7 | 35 |
| G4 | 15 | PCLK（DCLK） | 40 |
| G5 | 16 | DE | 43 |
| HSYNC | 44 | VSYNC | 45 |
| BL_CTR | 38 | LCD_RST | 60 |
| TP_SDA | 0 | TP_SCL | 1 |
| TP_INT | 55 | TP_RST | 39 |

注意 R6 是 GPIO36、R7 是 GPIO35，顺序和其他数据线不一样。屏幕用 5V（SYS_5V 经 FB2）供电，IO 电平 3.3V。

GPIO10、11、14-17 同时经 0Ω 电阻（R54、R55、R60、R58、R62、R57）接到背面的 SPI 屏接口，GPIO33/34 另有不贴的 R50/R51 通往 PWR 口的 USB 数据线。用 RGB 屏时 SPI 接口不能再插屏。LCD_RST（GPIO60）同时经 R43 接摄像头接口的 RESET；规格书写明 LCD1 接口的第 40 脚 LCD_RST 不连接，这块屏不需要复位。

### 显示配置

屏幕由 S31 的 LCD_CAM 以 RGB 并口驱动，没有初始化序列，也没有 ID 可读。时序取自厂商例程 `12_rgblcd_touch_screen/components/BSP/LCD/rgb_lcd.h`：800×480，HSYNC 宽 48、后肩 88、前肩 40，VSYNC 宽 3、后肩 32、前肩 13，数据在 PCLK 下降沿输出（`pclk_active_neg`），PCLK 时钟源 PLL_F160M。ESP-IDF 的后肩不含同步脉冲，所以一行共 976 个 PCLK、一帧 528 行。

规格书第 9 节的时序表给出：一行 889-1143 个 PCLK，典型 928；一帧 513-767 行，典型 525；前肩和同步宽度 1-255；PCLK 典型 30MHz、最大 50MHz。后肩只有典型值，一行的典型值 928 = 800 + 88 + 40，所以表里的后肩从同步脉冲开始算。厂商例程的 25MHz 按上面的时序算出的帧率是 48.5Hz。

项目让屏幕接近 GB/GBA 的帧率 4194304/70224Hz。LCD_CAM 的像素时钟由时钟源经小数分频再二分频得到，小数的分母不超过 64（ESP-IDF 的 `lcd_hal_cal_pclk_freq()`），所以请求的频率不一定分得出来：保持厂商的 976×528 时，请求 30.779MHz 实际得到 30.731707MHz，帧率 59.64Hz。项目保留厂商的同步宽度和后肩，把前肩改成 HFP 36、VFP 31，一行 972、一帧 546；PCLK 用 PLL_F160M 正好分得出的 160MHz × 21/106 = 31.698113MHz，算出的帧率是 59.727523Hz，比 GB 帧率高 0.4ppm。这几个值都在规格书的范围内。

帧缓冲放在 PSRAM，RGB565，两个。扫屏一直从 PSRAM 读数据：800×480×2 字节、59.73Hz 时平均约 46MB/s，和从 PSRAM 执行的代码、模拟器访问的数据争用带宽。对模拟速度的影响未实测。

这块屏在 60Hz 左右刷新时，中度灰画面的四周有闪烁，白色画面上看不出来，厂商例程 `12_rgblcd_touch_screen` 也一样。刷新率提到 109Hz 左右时看不到闪烁。规格书里没有 VCOM 的资料，原因没有确认，这个现象和液晶按帧翻转极性、正负两帧亮度不一致时的闪烁相符。

但是项目的固件不能提高刷新率。LCD 的 DMA 要按 PCLK 的速度一直从 PSRAM 取帧缓冲，固件的代码和只读数据也从 PSRAM 执行（`CONFIG_SPIRAM_XIP_FROM_PSRAM`），刷新率到 GB 帧率的 1.3 倍时 DMA 就会取数不及，LCD 报 underrun。underrun 以后，S31 的 RGB 驱动不会重新对齐帧的起点，画面会一直上下错位，直到复位。所以屏幕按 GB 帧率刷新，保留中度灰的闪烁。

面板按扫描方向是横屏，横放时扫描起点在左上角。掌机用法是以面板扫描方向的右边为上，UI 是 480×800 竖屏，`Rotation::cw90`。

### 背光

BL_CTR（GPIO38）经屏上 10kΩ 接 MP3302 的 EN，EN 另有 4.7kΩ 下拉（规格书第 5 节的背光电路），所以 GPIO38 不驱动时背光关闭。规格书写明背光可以用 BL_CTR 开关或者用 PWM 调光，没有给出 PWM 频率范围。项目用 LEDC 20kHz PWM 驱动 GPIO38，1kHz 和 20kHz 下都能调光。

### 触摸

GT1151Q 的 7 位地址固定为 0x14（编程指南第 1 节），寄存器地址 16 位。它和 ES8389、摄像头接口共用 GPIO0/1 上的 I2C，总线有 4.7kΩ 上拉 R1、R2 到 3.3V，GT1151Q 最高 400kHz。

复位按编程指南 4.1、4.2 节：主机把 INT 设为带上拉的输入，RESET 拉低至少 100µs 再拉高，60ms 后读 0x8044 的请求寄存器，读到 0x01 时才需要下发配置。这块屏的触摸已经存有配置，不需要主机下发。坐标在 0x814E 起：0x814E 的第 7 位表示数据就绪，低 4 位是点数，每点 8 字节，读完要把 0x814E 写 0。

触摸报告的坐标范围是 800×480，和面板扫描坐标相同，不需要交换或翻转，跟随显示的旋转一起换算。

项目用 ESP 组件库的 `esp_lcd_touch_gt911` 读坐标，因为它读写的 0x814E 状态寄存器和从 0x814F 起每点 8 字节的格式（编号一个字节，X、Y 各两个字节，低字节在前）和 GT1151Q 相同。这个驱动的复位流程是 GT911 的地址选择，不适用于 GT1151Q，所以复位由板级代码按上面的时序完成，驱动不接 RESET 和 INT。驱动初始化时从 0x8047 读出的“配置版本”在 GT1151Q 上不是配置区，GT1151Q 的配置区从 0x8050 开始，这个值只出现在日志里。

## 音频

ES8389 由 AUDIO_3V3 供电（RT9013，EN 经 R24 接 SYS_5V，常开），AD0 经 R22 100kΩ 下拉，7 位地址 0x10，识别寄存器 0xFD、0xFE 读出 `83`、`89`。I2S 连接见原理图第 1、3 页：

| 信号 | GPIO | 说明 |
|---|---|---|
| MCLK | 2 | 经 R61 0Ω |
| SCLK | 3 | 经 R59 0Ω |
| LRCK | 4 | 经 R52 0Ω |
| DSDIN | 5 | S31 输出到 codec，经 R53 0Ω |
| ASDOUT | 6 | codec 输出到 S31，经 R16 0Ω，项目不用 |
| PA_CTRL | 7 | 两个 NS4150B 共用，高电平开，R11、R17 下拉 |

厂商例程 `19_music` 不送 MCLK，让 ES8389 从 BCLK 产生时钟；项目和 Sila P4X 一样送 256 倍采样率的 MCLK，codec 能按 48kHz 打开。两个功放由 SYS_5V 分别经 FB5、FB6 供电，左声道 LOUTP/LOUTN 接 U4 到 SPKL，右声道 ROUTP/ROUTN 接 U5 到 SPKR，没有耳机插座。喇叭放音未实测。

## TF 卡

TF 卡座接在 S31 SDMMC slot 0 的 IOMUX 引脚上：CLK GPIO24、CMD GPIO25、D0-D3 GPIO20-23，每根线串 22Ω（R63-R68）。卡座 VDD 接 3.3V（经 FB7），和 IO 同一电压，不支持 1.8V 信号。CMD 和 DAT 线上画的 ESD 器件 E7-E12 标着 NC，原理图上没有外部上拉；厂商例程和项目都打开内部弱上拉。卡座的 CD 脚没有连接，没有插卡检测。项目按 4 线 high-speed（请求 40MHz）挂载。

## 摄像头和其他接口

18 针 DVP 摄像头接口：D0-D7 为 GPIO46-53，PCLK GPIO54，VSYNC GPIO56，HSYNC GPIO57，PWDN GPIO42（R42 10kΩ 下拉），RESET 来自 LCD_RST（GPIO60），SCL/SDA 与触摸共用 GPIO1/0。项目不用摄像头。

KEY0 接 GPIO61，也就是 BOOT 引脚，按下接地；它的上拉 R7 没有贴装，用作按键时要开内部上拉。RESET 键接 EN。RGB 指示灯的数据线是 GPIO37。

20 针的 P1 排针引出 GPIO8、9、12、13、15、18、19、37、38、39、42、46、54、55、56、57，其中大部分已经被屏幕、触摸或摄像头占用；10 针的 P2 引出 RX0、TX0、GPIO61、EN 和 SYS_5V。

## 电源

板子由两个 USB-C 口中的任一个供 5V：COM 口（CH343P）和 PWR 口，经 1.5A 自恢复保险丝 F1 成为 SYS_5V。SY8089A1AAC（U10）的 EN 经 R39 接 5V，上电即输出 3.3V 给模组和外设。RT9013（U6）单独给音频供 3.3V。USB-A 口接模组的 USB 2.0 OTG（USB_DP/USB_DM），VBUS 直接接 SYS_5V。手册给出的整板电流：跑马灯例程 48mA，4.3 寸 RGB 屏加 OV5640 摄像头 445mA。

## GPIO 总表

| GPIO | 网络 | 连接 |
|---|---|---|
| 0、1 | TP_SDA、TP_SCL（CDATA、CCLK、DVP_SDA、DVP_SCL） | 共用 I2C，4.7kΩ 上拉 |
| 2-6 | I2S_MCLK、SCLK、LRCK、DSDIN、SDOUT | ES8389 |
| 7 | PA_CTRL | 两个 NS4150B |
| 8-19 | LCD_B3-B7、G2-G7、R3 | RGB 屏，10、11、14-17 并到 SPI 屏接口 |
| 20-25 | SD_D0-D3、SD_CLK、SD_CMD | TF 卡，SDMMC slot 0 |
| 33、34 | LCD_R4、LCD_R5 | RGB 屏；也是 USB-Serial/JTAG 的 D-/D+ |
| 35、36 | LCD_R7、LCD_R6 | RGB 屏 |
| 37 | LED_CTR | RGB 指示灯 |
| 38 | BL_CTR | 屏幕背光，LEDC PWM |
| 39 | TP_RST | 触摸复位 |
| 40 | LCD_PCLK | RGB 屏 |
| 42 | DVP_PWDN | 摄像头 |
| 43-45 | LCD_DEN、LCD_HSYNC、LCD_VSYNC | RGB 屏 |
| 46-54、56、57 | DVP_D0-D7、DVP_PCLK、DVP_VSYNC、DVP_HSYNC | 摄像头 |
| 55 | TP_INT | 触摸中断 |
| 58、59 | TX0、RX0 | CH343P，控制台 |
| 60 | LCD_RST | 摄像头 RESET；这块屏不用 |
| 61 | BOOT | KEY0，strapping |

## 烧录和恢复

烧录和日志都走 COM 口的 CH343P。DTR、RTS 经 Q1、Q2 控制 EN 和 BOOT，idf.py 能自动进入下载模式和复位。手动进入下载模式：按住 KEY0，按一下 RESET，再松开 KEY0。

ESP-IDF v6.1 里 S31 是 preview target，构建目录第一次选择 target 时要用 `idf.py --preview set-target esp32s31`。esptool 写这块板子时使用 `--no-stub`。

## 待确认的硬件约束

- esptool 列出 300MHz，项目和厂商例程都跑 320MHz，依据待查。
- 屏上的驱动 IC 型号没有核对，规格书没写。
- MP3302 的 EN 端 PWM 调光频率范围没有资料。
- 中度灰画面在 60Hz 左右闪烁的原因没有确认，见“显示配置”。
- `esp_lcd_touch_gt911` 读 GT1151Q 的坐标未实测。
- 喇叭放音未实测。

## 来源

- 慧勤智远 ESP32-S31 CB V1.1 原理图，2026-9-17，第 1-4 页。
- 慧勤智远 ESP32-S31 CB V1.1 用户手册 V1.0：1.4 节资源说明、1.5 节引脚分配表、2 节硬件电路。
- WKS43WV067-WCT 规格书 V0.0：第 5 节背光、第 8 节接口定义、第 9 节输入时序。
- Goodix GT1151Q 编程指南 Rev.01：第 1 节接口、第 3 节寄存器、第 4 节上电和复位时序、第 5 节坐标读取。
- 厂商基础例程 `12_rgblcd_touch_screen`、`15_sdcard`、`19_music`。
- ESP-IDF v6.1：`components/esp_hal_sd/esp32s31/include/soc/sdmmc_pins.h`（SDMMC 引脚）、`components/esp_hal_gpio/esp32s31/include/hal/gpio_ll.h`（USB-Serial/JTAG 在 GPIO33/34）、`components/bootloader_support/bootloader_flash/src/flash_qio_mode.c` 和 `bootloader_flash.c`（QIO 和 XMC）。

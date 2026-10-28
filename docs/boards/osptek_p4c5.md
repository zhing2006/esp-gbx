# OSPTEK ESP32-P4C5-Module

项目代号是 `osptek_p4c5`，板级名称是 OSPTEK ESP32-P4C5-Module。它由鱼鹰光电（OSPTEK）的“ESP32P4模组基础底板”和 ESP32-P4C5 核心板组成，无线连接通过核心板上的 ESP32-C5 完成。本项目配的屏幕是 OSPTEK 的 4.3 英寸 YDP430BT009-V1，480×800，显示驱动是 ST7102，触摸驱动是 ST7123，经 OSPTEK 的 4.3 寸 LCD 转接板接到底板背面的 15 针 DSI 接口。

项目固件已经接入屏幕、背光调光、触摸、音频和 SD 存储，背光调光需要一根跳线，见“屏幕和触摸”。

## 型号和版本

手上这块板经 CH343P 串口读取，esptool v5.4.0 报告 ESP32-P4 revision v3.2、40MHz 晶振，flash 的 JEDEC ID 为 `68 40 18`，容量 16MB。出厂固件的启动日志报告 32MB PSRAM，厂商 ID 0x0d（AP），X16 模式；它运行在 200MHz，CPU 400MHz。

底板有两份原理图：资料仓库 `docs/` 下单独的 `ESP32P4模组基础底板V1.3.pdf`，标题栏为 V1.3、2025/12/03；使用指南 2026.09.10 的最后一页是同一块底板的 V1.5，日期 2026/08/28。按使用指南的“硬件版本”表，V1.4 和 V1.5 只改了 C5 BOOT 引脚的连接，本文以 V1.5 为准。实物 PCB 的丝印版本尚未核对。核心板 `esp32-p4c5-core-board` 有独立的资料仓库，资料里没有它的原理图，所以核心板内部的 flash、PSRAM 供电、LDO 接法和 C5 的连接都没有图纸依据。

屏幕的规格书型号是 YDP430BT009-V1，修订记录为 V.0、2026-04-02。转接板原理图的标题是“4.3寸LCD转接板”，V1.0，日期 20260202。

出厂固件的工程名是 `esp32p4-4.3-tft-480x800-mipi-st`，用 ESP-IDF v5.5.3 编译，运行 LVGL benchmark。它读出的 ST7102 ID 为 `80 A0 FB`，ST7123 的固件版本为 `1(1.71.1.5)`，坐标范围 480×800，最多 5 点。这份出厂固件没有备份。

## 资料

开发需要的图片和参考源码保存在 `refs/boards/osptek_p4c5/`，不依赖原始下载目录。资料来自用户提供的 OSPTEK 目录，其中两个资料仓库是 git 克隆：

| 仓库 | 上游 | 固定提交 | 许可 |
|---|---|---|---|
| `esp32-p4c5-module-dev-board` | https://gitee.com/osptek/esp32-p4c5-module-dev-board | `dacb40fa25d6400bfa2524c4e6d4ecbf11b7fea5` | CC BY 4.0 |
| `tft-4.3-480x800-mipi-st7102` | https://gitee.com/osptek/tft-4.3-480x800-mipi-st7102 | `601868ee4a83271281689be94cb4164c1270076c` | MIT，Copyright (c) 2026 Osptek |

用户目录里另外两份 PDF 与仓库中的同名文件逐字节相同：`ESP32-P4-Module_使用指南2026.09.10（含原理图）(1).pdf` 和 `YDP430BT009-V1.pdf`。`Camera.txt` 写明摄像头是 SC2336。

原理图按电路区域裁成 300dpi PNG，保存在 `schematic/`，`crops.json` 记录原文件的 SHA-256、页码和以 PDF 点为单位的裁剪坐标。

| 图片 | 原文件和裁剪内容 |
|---|---|
| `01-module.png` | 使用指南第 6 页，底板 V1.5：核心板 U4 的焊盘和网络、复位 RC |
| `02-pin-headers.png` | 同页：两组 2×17 排针 J2、J4 |
| `03-camera-24p.png` | 同页：正面 24P 摄像头接口 J1、复位、24MHz 晶振 X1、电平转换 U5 和摄像头 LDO U1 |
| `04-display-30p-backlight.png` | 同页：正面 30P 屏幕接口 J3 和它的背光升压 U2 |
| `05-csi-dsi-15p.png` | 同页：背面 15P 摄像头接口 J5、15P 屏幕接口 J6 和 P2 |
| `06-buck-tf.png` | 同页：5V 转 3.3V 降压 U6 和 TF 卡座 P1 |
| `07-usb-uart-download.png` | 同页：调试口 J7、CH343P、自动下载电路和 P4/C5 下载切换 U10 |
| `08-c5-boot.png` | 同页：V1.5 的 C5 BOOT 电阻 R54～R56 |
| `09-usb-host.png` | 同页：数据口 J8、LM66200 电源选择、TS3USB221 切换和 USB-A |
| `10-audio.png` | 同页：麦克风、ES8311、音频 LDO U8 和 NS4150B |
| `11-lcd-adapter.png` | 屏幕仓库 `docs/4.3寸LCD转接板V1.0.pdf` 第 1 页：转接板全部电路 |

下面的源码直接复制到仓库，以 `.txt` 后缀保存，路径相对于屏幕仓库的 `versions/YDP430BT009-V1/`。`gx09c-st7102-boe4.3-2lane-90hz.c.txt` 原文件是 GBK 编码，转成了 UTF-8，内容没有改动；其余文件逐字节复制。示例目录 `examples/esp32p4-idf6_st7102-mipi_esp-lvgl-port_lvgl9_ppa/` 下面简写为 IDF6 示例。

| 仓库中的参考文件 | 保留的内容 | 原路径 | 原文件 SHA-256 |
|---|---|---|---|
| `idf6-example-main.c.txt` | 触摸 I2C、lane 速率、DPI 宏的调用、复位和背光脚的设置 | IDF6 示例 `main/main.c` | `dc269d888f95616f6f61ad3bc7bf76e093a8599a3282f6730ba986d9b625691a` |
| `st7102_init_cmds.h.txt` | 这块面板的初始化序列 | IDF6 示例 `main/st7102_init_cmds.h` | `ea108439b0746ac42d5827d194112d82d0896ab47320299c568f6932c20ceb08` |
| `esp_lcd_st7102_mipi.c.txt` | 读 ID、软件复位、序列之前的 MADCTL 和 COLMOD | IDF6 示例 `components/esp_lcd_st7102/esp_lcd_st7102_mipi.c` | `e5c2112001922f39e0816b4bd1eef80403da17aa1705f4bc9a1eef76772d93dd` |
| `esp_lcd_st7102.h.txt` | 480×800 的 DPI 时序宏 | IDF6 示例 `components/esp_lcd_st7102/include/esp_lcd_st7102.h` | `d2e514a25102931a4cb7f7a179ef2553446187f09365d8c6cbb6724a4a78723b` |
| `st7102_interface.h.txt` | 上面驱动用到的页命令宏 | IDF6 示例 `components/esp_lcd_st7102/priv_include/st7102_interface.h` | `c543cc8c3c3d1ccc2cf56928d731055e0a145791308b367236c7c646fcff0998` |
| `gx09c-st7102-boe4.3-2lane-90hz.c.txt` | 屏厂的上电时序、复位延时和初始化序列 | `docs/GX09C_ST7102+BOE4.3_2LANE_90HZ.c` | `4b149074dbb4431380665e41682ee9bc7bc66b8a73d5241e25dd34889f0eeb35` |

ST7102 驱动的三个文件保留 Espressif 的 Apache-2.0 声明，`idf6-example-main.c.txt` 的文件头写的是 OSPTEK 的 CC-BY-4.0。另外两个文件没有单独的许可声明，适用屏幕仓库的 MIT 许可。

没有入库的原始文件及其 SHA-256：

| 文件 | SHA-256 |
|---|---|
| 底板仓库 `docs/ESP32-P4-Module_使用指南2026.09.10（含原理图）.pdf` | `f5c351b32d1e1c1683617b339a8305b444cb40ce2d5f21f34b106440843b3ca8` |
| 底板仓库 `docs/ESP32P4模组基础底板V1.3.pdf` | `88912834110a2077695000727aeb0ce7eb71667bf0e6c5795c741d01ae0c6fb8` |
| 屏幕仓库 `docs/YDP_430_BT_009_V1_a418b8470b.pdf`，屏幕规格书 | `e6075bb881c661eda3bc28d624d758a89063f834fd979f96f6ac4401d777673a` |
| 屏幕仓库 `docs/4.3寸LCD转接板V1.0.pdf` | `eb0dfa09150067073664031804d74410be3403518301f68d8bfa1c78a1a95a3c` |
| 屏幕仓库 `docs/ST7102_Datasheet_V0.22.pdf` | `06821a479a033404df2cb4bb44c83480965140b3420d587e9cb01233a8130a8f` |
| 屏幕仓库 `docs/RGB_Output_Timing_90Hz.jpg`，屏厂调试工具的时序截图 | `5adc0472e9ffb7c8466c248fd2d5974061dcd7bd769ae6fadc7f79a10a0eb9cf` |
| 底板仓库 `docs/SC2336_数据手册_V0.7(1).pdf` | `7285a101f4bd3697aed7085b8c4ac1e86726028e50a650468b3998163423a4a9` |
| 底板仓库 `firmware/ESP32C5_2.12.9_0X0.bin`，C5 的合并镜像 | `a9b2eaec9428ba37fc4aec23f16908462215eecef230e61ffab8b44f81c39a73` |

示例工程、外壳 STEP、DWG 和产品图都没有入库。

## 主控和存储

P4 v3.2 使用 v3.x 的修订配置：`CONFIG_ESP32P4_SELECTS_REV_LESS_V3` 关闭，最低修订 v3.1，CPU 400MHz。它不能运行为 v1.x 编译的固件。

flash 的厂商 ID `0x68` 是 Boya，具体型号没有从芯片丝印核对。项目开启 `CONFIG_SPI_FLASH_SUPPORT_BOYA_CHIP`，让 IDF 按 Boya 驱动识别它，使用 QIO 80MHz。出厂 bootloader 也以 QIO 启动这颗 flash。

PSRAM 使用 HEX 接口、250MHz，与 IDF6 示例的配置相同；ESP-IDF v6.1 的 `SPIRAM_SPEED_250M` 只在 v3.x 的修订配置下可选。出厂固件用的是 200MHz。开机内存检查由构建类型决定。配置以 `boards/osptek_p4c5/sdkconfig.defaults` 为准。

bootloader 的日志级别设为 warning：debug 构建的 QIO bootloader 在 info 级别下超出 `0x2000` 到分区表 `0x8000` 之间的空间。

## IC

| 位置 | 型号 | 用途 |
|---|---|---|
| 底板 U4 | ESP32-P4C5 核心板，图纸符号为 `ESP32-P4C6/C5_Module` | 主控和 ESP32-C5，88 个邮票孔焊盘 |
| 底板 U9 | CH343P | 调试口的 USB 转 UART，本机 VID:PID 为 `1a86:55d3` |
| 底板 U7 | DDC114TU，图中备选 BC847BDW1T1G | DTR/RTS 自动下载 |
| 底板 U10 | RS2233XTSS16-Q1 | 按 S1 把串口、复位和 BOOT 接到 P4 或 C5 |
| 底板 U6 | TLV62569DBVR，图中备选 ETA3485、SY8088、RY3420 | VIN_5V 降到 +3V3 |
| 底板 U14 | LM66200DRLR | 两个 Type-C 的 5V 选通，输出 VIN_5V |
| 底板 U15 | TS3USB221ARSER | P4 的高速 USB 在 Type-C J8 与 USB-A 之间切换 |
| 底板 U12 | MT9700 | USB-A 的 5V 限流开关 |
| 底板 U2 | SY7200 | 正面 30P 屏幕接口 J3 的背光升压 |
| 底板 U1、U5 | VRD1830MLX、TXS0102DCUR | 摄像头的 3.0V/1.8V LDO 和 I2C 电平转换 |
| 底板 U11 | ES8311 | 单声道 codec |
| 底板 U8 | XC6206P332MR | 从 VIN_5V 产生 codec 和麦克风的 ALDO3V3 |
| 底板 U13 | NS4150B | 单声道功放 |
| 底板 MIC1 | LMA3729T421-OA1 | 模拟麦克风 |
| 转接板 U1 | SY7200 | 4.3 英寸屏幕的背光升压 |
| 屏幕 | ST7102、ST7123 | 分别负责显示和触摸 |

## 屏幕和触摸

### 连接

转接板的 J1 是 15 针 1.0mm FPC，接底板背面的 J6。J6 只有 DSI 的 2 条数据 lane 和时钟 lane、I2C（SCL 为 GPIO8，SDA 为 GPIO7）、+3V3 和 GND，见 `05-csi-dsi-15p.png`。转接板把屏幕的 30 针 FPC 接到 J1，屏幕的复位、触摸复位、触摸中断和背光使能则引到 5 针排针 J2：LCD_RST、TP_RST、TP_INT、LCD_BL、GND，见 `11-lcd-adapter.png`。LCD_RST 和 TP_RST 各有 10kΩ 上拉和 0.1µF 对地电容，LCD_BL 经 R2 10kΩ 上拉到 3.3V，TP_INT 没有上拉。

J6 上没有这几根线，所以 J2 要另外接线才能连到 P4。J2 不接线时，背光由 R2 拉高，上电后一直全亮，P4 无法调光。

项目约定用一根跳线把 J2 的 4 脚 LCD_BL 接到排针 J4 的 34 脚 GPIO23。GPIO23 在底板上没有接其他电路，GND 已经经 J6 相连。LCD_RST、TP_RST 和 TP_INT 不接：项目用软件复位初始化面板，触摸用轮询。没有这根跳线时，GPIO23 上的 PWM 不起作用，背光仍然全亮，其他功能不受影响。

**不要把 4.3 英寸屏幕的排线直接插进底板正面的 30P 接口 J3。** J3 按 YDP400BT001-V4 的引脚排列（`04-display-30p-backlight.png`），和 YDP430BT009-V1 的 30 针定义不同。比如 J3 的 4 脚是 VCI，屏幕的 4 脚是背光阴极 LEDK。

屏幕规格书第 5 页的 FPC 定义：

| 脚 | 信号 |
|---|---|
| 1、2 | LEDA |
| 4、5 | LEDK |
| 7、10、13、16、19、29 | GND |
| 11、12 | MIPI CLK+/− |
| 14、15 | MIPI D1+/− |
| 17、18 | MIPI D0+/− |
| 21 | RSTB |
| 22、23 | VCC，3.0～3.6V，典型 3.3V |
| 25、26 | TP_SCL、TP_SDA，规格书写“TYP. 1.8V” |
| 27、28 | TP_RST、TP_INT |
| 30 | TP_VDD |
| 3、6、8、9、20、24 | NC |

规格书把触摸 I2C 写成典型 1.8V，但转接板用 3.3V 给 TP_VDD 供电，并把 TP_SCL、TP_SDA 经 10kΩ 上拉到 3.3V。项目按转接板的接法使用，没有资料说明这两处的差别。

### 显示配置

面板按 480×800 竖屏扫描，规格书给出的可视区为 56.16×93.60mm，像素间距 117µm。帧缓冲和其他板子一样是 RGB565，DSI bridge 把它转成 RGB888 发给面板，面板的 COLMOD 是 `0x77`。按 ST7102 数据手册的数据类型表，面板接收的像素流只有 `3Eh` Packed Pixel Stream, 24-bit RGB 一种。DSI 用 2 lane，每 lane 820Mbps，取自 IDF6 示例。MIPI PHY 由片内 LDO3 供电，按示例设为 2500mV。

屏厂资料和 IDF6 示例的时序，按 HSYNC/HBP/HFP、VSYNC/VBP/VFP 列出：

| 来源 | 像素时钟 | 水平 | 垂直 | 按参数算出的帧率 |
|---|---|---|---|---|
| `esp_lcd_st7102.h.txt` 的 `ST7102_MIPI_480_800_PANEL_60HZ_DPI_CONFIG`，IDF6 示例使用 | 37.8MHz | 2 / 40 / 40 | 2 / 10 / 310 | 约 59.94Hz |
| `RGB_Output_Timing_90Hz.jpg`，屏厂工具的截图 | 57MHz | 2 / 40 / 40 | 2 / 10 / 310 | 工具显示 90.40Hz |
| 本项目 | 40MHz | 2 / 40 / 47 | 2 / 10 / 365 | 约 59.7271Hz |

项目以 GB/GBA 的 `4194304 / 70224 ≈ 59.7275 Hz` 为刷新目标。40MHz 是 PLL_F240M 的六分频，没有取整误差，所以 IDF 不会补偿 DPI bridge 的水平前肩。bridge 的总周期为 569×1177，按参数计算约 59.7271Hz，比目标低约 7ppm。DSI host 的水平时序按 lane byte clock 计数，820Mbps 时是 102.5MHz，一行 569 个像素折合 1458.0625 个周期，IDF 取整为 1458，按这个计算约 59.7296Hz，比目标高约 36ppm。面板实际跟随哪一边的时序，资料里没有说明。取整和补偿逻辑见 IDF v6.1 的 `esp_hal_lcd/mipi_dsi_hal.c`。

初始化流程按 `esp_lcd_st7102_mipi.c.txt` 的顺序：先读 ID（寄存器 `04h`），读不到时显示初始化失败；因为复位脚没有接到 P4，用软件复位 `01h`，等 120ms；再发 `F0h 00`、MADCTL `00`、COLMOD `77`，然后发 `st7102_init_cmds.h.txt` 的序列。这个序列去掉最后的 `35h` 以后，与屏厂的 GX09C 文件逐字节相同。GX09C 文件在显示打开后发 `35h`（TE 打开），IDF6 示例把它注释掉了，项目也不发，TE 也没有接到 P4。sleep out `11h` 之后等 600ms、display on `29h` 之后等 120ms，两份来源一致。`esp_lcd_st7102_mipi.c.txt` 里的默认序列 `vendor_specific_init_default` 是另一块玻璃的，项目不用。

在乐鑫组件仓库搜索 st7102，只找到 `kevincoooool/esp_lcd_st7102`，没有 espressif 命名空间的 ST7102 组件，屏幕仓库的驱动也只在示例目录里。所以项目不引入面板组件，`board.cpp` 自己发初始化序列，然后直接创建 DPI 面板。

UI 与面板同为 480×800，`display_init()` 返回 `Rotation::none`，不旋转。

### 触摸

ST7123 在共用 I2C 上，7 位地址 `0x55`。项目使用 `espressif/esp_lcd_touch_st7123` 1.0.2，与 IDF6 示例的依赖版本相同，I2C 400kHz 也与示例相同。RST 和 INT 都不接，驱动按平台的节奏轮询。控制器报告 480×800 的坐标，与面板分辨率相同，示例也不交换、不翻转坐标。

ST7102 数据手册说明芯片内置触摸控制器，屏幕资料把触摸驱动写作 ST7123，项目按资料使用 ST7123 的协议。

## 音频

ES8311 的 CE 经 R37 10kΩ 接地，7 位地址 `0x18`，和触摸、摄像头共用 GPIO7/8 的 I2C。这条总线在底板上有 R49、R52 两个 10kΩ 上拉，转接板上还有 R5、R6 两个 10kΩ 上拉。I2S 连接如下：

| 信号 | GPIO |
|---|---|
| MCLK | 13 |
| SCLK | 12 |
| LRCK | 10 |
| P4 输出到 ES8311 DSDIN | 9 |
| ES8311 ASDOUT 输入到 P4 | 11 |
| NS4150B 的 STD | 53 |

ES8311 的 OUTP/OUTN 经隔直电容和 75kΩ 电阻接 NS4150B 的差分输入，功放输出到喇叭插座 J9（MX1.25-2P）。NS4150B 的 VCC 接 VIN_5V，所以 codec 的 `pa_voltage` 按 5.0V 设置；功放本身的增益从图纸上看不出来，没有计入。STD 有 R44 10kΩ 下拉，`board::init()` 也把它设为输出低电平，codec 打开以后才拉高。喇叭是单声道，GB 的立体声先把左右声道平均再输出。

ES8311 和麦克风由 U8 产生的 ALDO3V3 供电，U8 的输入是 VIN_5V。麦克风 MIC1 接 ES8311 的 MIC1P/MIC1N，项目没有用录音。

## TF 卡

TF 卡座 P1 使用 SDMMC slot 0，D0～D3 为 GPIO39～42，CLK 为 GPIO43，CMD 为 GPIO44，4 位模式。卡的 VDD 接固定的 +3V3，没有电源开关。D0～D3 和 CMD 各有 10kΩ 外部上拉（R11～R13、R18、R19），CLK 没有上拉。卡座的检测脚接 GPIO45，有 R16 10kΩ 上拉；插卡时的电平没有资料说明，平台不用这个脚。

GPIO39～48 在 P4 的 VDD_IO_5 电压域。核心板焊盘 66 标为 `VFB_VO4`，底板只给它标了 `LDO_VO4_ADJ` 网络名，没有其他连接。VDD_IO_5 是否由 LDO4 供电，没有核心板图纸可以确认。和其他 P4 板一样，`board::init()` 把 LDO4 固定在 3300mV，供电不依赖 SD 卡是否挂载；在 P4 的 IDF 实现中这是输入电源直通模式。项目不启用 1.8V UHS。

## 摄像头和 C5

正面 24P 接口 J1 按 SC2336 和 OV2710 兼容设计，2 lane CSI。摄像头的 1.8V 和 3.0V 由 U1 VRD1830MLX 产生，两个使能都接 +3V3，上电就开着。复位 CSI_RST 由 R20 10kΩ 上拉到 1.8V，GPIO26 经二极管 D16 只能把它拉低，图纸注明 IO 用开漏输出。I2C 经 U5 TXS0102 转到 1.8V，24MHz MCLK 来自独立晶振 X1。背面 15P 接口 J5 兼容树莓派排线，它的 CSI_RST 和 3.3V 的 I2C 直接相连。SC2336 数据手册给出的 7 位地址是 `0x30`。项目没有接入摄像头。

C5 在核心板上，P4 与 C5 之间的接口没有图纸。底板的 U10 在 S1 拨到 C5 时，把串口接到 C5 的 U0TXD/U0RXD，把自动下载电路的复位接到 GPIO54 这条网络，把 BOOT 接到 C6/C5_BOOT；V1.5 的 C6/C5_BOOT 经 R54～R56 接 C5 的 IO27、IO28 和 IO25（`08-c5-boot.png`）。项目不改写 C5，也不操作 GPIO54。

## 电源

两个 Type-C 口各自输入 VIN1_5V（调试口 J7）和 VIN2_5V（数据口 J8），U14 LM66200 选通后输出 VIN_5V。U6 把 VIN_5V 降到 +3V3，给核心板、TF 卡、屏幕和触摸供电。VIN_5V 还直接给 NS4150B、音频 LDO U8，并经 U12 MT9700 给 USB-A 供电，限流电阻 R43 为 5.1kΩ，图中标注“ILimit: 5.1K~1.3A”。两个 Type-C 各有一个 5V 指示灯 D4、D5。

背光有两套电路。底板 U2 SY7200 只给 30P 接口 J3 的背光供电，EN 接 GPIO20 并由 R1 10kΩ 上拉；本项目的屏幕不接 J3，固件不操作 GPIO20。转接板 U1 SY7200 给 4.3 英寸屏幕的背光供电，EN 是 J2 的 LCD_BL。两边的电流采样电阻都是 5.1Ω，FB 基准 200mV，按图纸算出约 39mA；屏幕规格书给出的背光电流典型值为 40mA，LED 电压 10.8～13.6V。

转接板图纸只写了 SY7200，没有写后缀。按 Sila P4X 用的 SY7200A 的手册，EN/PWM 调光频率范围是 20kHz～1MHz（见 [Sila P4X 文档](sila_p4x.md#屏幕与触摸)），所以固件在 GPIO23 上用 25kHz、11 位的 LEDC，和 Sila P4 相同。板级初始化先关闭背光，应用在画面准备好以后开启。

## GPIO 总表

除了 GPIO14～19，P4 的 GPIO 都引到两组排针 J2、J4 上，排针定义见 `02-pin-headers.png`。

| GPIO | 网络或用途 | 连接与限制 |
|---|---|---|
| 0、1 | 排针 | P4 的 32kHz 晶振复用脚 |
| 2、3 | 排针 | 底板未接其他电路 |
| 4 | LCD_TE | J3 的 TE，经 R3 0Ω |
| 5 | LCD_RST | J3 的复位，经 R2 0Ω；R53 10kΩ 上拉、C14 0.1µF |
| 6 | TP_RST | J3 的触摸复位，经 R8 0Ω |
| 7、8 | I2C_SDA、I2C_SCL | 触摸、ES8311、摄像头共用 |
| 9～13 | DSDIN、LRCK、ASDOUT、SCLK、MCLK | ES8311 |
| 14～19 | 不在底板上 | 核心板内部使用，没有图纸 |
| 20 | J3 背光使能 | 底板 U2 的 EN，R1 10kΩ 上拉 |
| 21 | TP_INT | J3 的触摸中断，经 R6 0Ω |
| 22 | 排针 | 底板未接其他电路 |
| 23 | 排针，跳线接转接板 J2 的 LCD_BL | 背光 PWM，高电平开启 |
| 24、25 | 排针 | USB-Serial/JTAG 的 D−/D+，底板没有接 USB 口 |
| 26 | CSI_RST | 经 D16 拉低摄像头复位 |
| 27～31 | 排针 | 底板未接其他电路 |
| 32、33 | 排针 | 核心板焊盘标注 I3C_SCL、I3C_SDA，底板未接其他电路 |
| 34 | 排针 | strapping 脚 |
| 35 | BOOT | 下载 strapping，经 U10 接自动下载电路，按键 K2，R35 10kΩ 上拉 |
| 36 | 排针 | R24 10kΩ 上拉，strapping 脚 |
| 37、38 | DBG_TXD、DBG_RXD | UART0，经 U10 接 CH343P，也是 strapping 脚 |
| 39～44 | SD_D0～D3、SD_CLK、SD_CMD | SDMMC slot 0，VDD_IO_5 电压域 |
| 45 | SD_DET | 卡检测，R16 10kΩ 上拉 |
| 46～52 | 排针 | 底板未接其他电路，46～48 属于 VDD_IO_5 电压域 |
| 53 | PACTRL | NS4150B 的 STD，R44 10kΩ 下拉 |
| 54 | 排针，U10 的 4B1 | S1 拨到 C5 时由自动下载电路驱动，项目不操作 |

GPIO4、5、6、20、21 只在使用 J3 的屏幕时有意义。本项目的屏幕接 J6，这几个脚没有用上。

## 烧录和恢复

调试口 J7 经 CH343P 接 UART0，板级控制台选择 `CONFIG_ESP_CONSOLE_UART_CUSTOM=y`，波特率由公共配置设为 460800。GPIO24/25 只引到排针，没有 USB 口，所以 `CONFIG_ESP_CONSOLE_SECONDARY_NONE=y` 关闭了 USB-Serial/JTAG 辅助控制台。

烧录 P4 时，S1 要拨向 P4，资料仓库的 README 写的是“往上拨烧录 ESP32-P4”。esptool 通过 DTR/RTS 自动进入下载模式和复位。手动进入下载模式时按住 BOOT 键 K2（GPIO35）再按 RESET 键 K1。S1 拨向 C5 时，串口、复位和 BOOT 都切到 C5，这时不能烧录 P4。

另一个 Type-C 口 J8 接 P4 的高速 USB。U15 由 J8 的 VIN2_5V 经 Q1 控制：J8 有 5V 时 P4 的 USB 接到 J8，否则接到 USB-A。USB-A 的 5V 由 U12 MT9700 从 VIN_5V 得到，EN 经 R41 上拉到 VIN_5V，板子有电时一直输出（`09-usb-host.png`）。项目在 USB-A 上接 USB 手柄，这块板子打开了 `GBX_USB_GAMEPAD`。用手柄时 J8 不能插线，烧录和日志都走调试口 J7。

出厂固件没有备份。厂商的 IDF6 示例可以重新编译，作为接近出厂状态的显示和触摸程序。资料仓库的 `firmware/ESP32C5_2.12.9_0X0.bin` 是 C5 的合并镜像，要把 S1 拨向 C5 后烧到 `0x0`，不是 P4 的恢复镜像。

## 待确认的硬件约束

- 实物底板的 PCB 版本和核心板的版本。
- 核心板内部的 VDD_IO_5 供电、C5 与 P4 之间的接口和 GPIO54 的连接，资料里没有核心板原理图。
- flash 的具体型号。
- TF 卡检测脚在插卡时的电平。
- 共用 I2C 上 `0x28` 处有一个设备应答，型号和用途不明；它不是底板图纸上的器件。
- 屏幕规格书把触摸 I2C 写成 1.8V，转接板按 3.3V 上拉。

## 来源

- 用户提供的 OSPTEK 资料，仓库提交、校验值和裁图索引见“资料”一节。GPIO、供电和外围连接以底板 V1.5 原理图和转接板原理图为依据。
- 屏幕仓库的 IDF6 示例、ST7102 驱动和屏厂 GX09C 初始化文件，保存在 `refs/boards/osptek_p4c5/`。
- YDP430BT009-V1 规格书第 4、5、7、8 页：尺寸、FPC 定义、供电和背光参数。
- ST7102 数据手册 V0.22 的芯片概述和 DSI 数据类型表。
- SC2336 数据手册 V0.7 的 I2C 接口一节。
- [`espressif/esp_lcd_touch_st7123` 1.0.2](https://components.espressif.com/components/espressif/esp_lcd_touch_st7123/versions/1.0.2)，触摸读取和坐标范围。
- [`espressif/esp_codec_dev` 1.6.2](https://components.espressif.com/components/espressif/esp_codec_dev/versions/1.6.2)，ES8311 的控制接口。
- ESP-IDF v6.1：`components/esp_hw_support/port/esp32p4/Kconfig.hw_support`、`components/esp_psram/esp32p4/Kconfig.spiram`、`components/spi_flash/spi_flash_chip_boya.c`、`components/esp_hal_lcd/mipi_dsi_hal.c`；LDO 直通模式见 `components/esp_hal_pmu/esp32p4/include/hal/ldo_ll.h`。
- [ESP32-P4 数据手册](https://documentation.espressif.com/esp32-p4_datasheet_en.html)的管脚、电源域和 strapping 说明。

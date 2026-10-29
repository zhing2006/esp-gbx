# M5Stack Tab5

Tab5 是 M5Stack 的 5 寸 ESP32-P4 平板形态开发套件，SKU 是 C145。M5Stack 公开了原理图，出厂固件 M5Tab5-UserDemo 也开源；乐鑫的 esp-bsp 里有它的 BSP `m5stack_tab5`。板上没有游戏按键，只有一个电源键。

本文对应屏幕驱动 IC 是 ST7121 的 Tab5。引脚以原理图为准，并和 esp-bsp、出厂固件的源码核对过。

## 资料

原始资料在 `refs/boards/m5stack_tab5/` 下：

| 文件 | 内容 | 来源 |
|---|---|---|
| `schematic/` | 原理图：P4 和 C6、屏幕和 TF 卡、音频、接口、电源 | M5Stack 文档站的 `Tab5_Schematics_PDF.pdf` |
| `esp_lcd_st7121.c.txt` | M5Stack 写的 ST7121 驱动，初始化序列在 `vendor_specific_init_default` 里，和 esp-bsp 的 `disp_init_data_st7121.h` 逐字节相同 | M5Tab5-UserDemo 的 `platforms/tab5/components/esp_lcd_st7121/esp_lcd_st7121.c` |
| `m5stack_tab5.c.txt` | 出厂固件的 BSP，包括两片 IO 扩展器的初始化、三种屏幕的探测和时序、TF 卡和音频的设置 | M5Tab5-UserDemo 的 `platforms/tab5/components/m5stack_tab5/m5stack_tab5.c` |

两个源码文件取自 M5Tab5-UserDemo 的提交 `b4e356bc491ca070d54004718dad789c07d5fc93`。转图用的原理图 PDF 的 SHA-256 是 `13cf3fd9954d39aa198e57b1bb94ec56af7d49107c80a245acc5bd90ac8ea329`。

## 型号和版本

Tab5 的屏幕换过两次驱动 IC。按 M5Stack 文档页的更新记录，2025-05-09 首发时用 ILI9881C 显示、GT911 触摸，2025-10-14 起换成显示和触摸一体的 ST7123，2026-04-28 起又换成 ST7121。三版的时序和初始化序列都不同，软件要先认出是哪一版。esp-bsp 的 `bsp_get_board_version()` 和出厂固件的 `bsp_detect_display_type()` 用同一个办法：I2C 上 0x14 有应答的是 GT911，也就是 ILI9881C 那版；0x55 有应答时读触摸寄存器 0x0000 的固件版本，1 是 ST7121，3 是 ST7123。

手上这块板的触摸固件版本读出来是 1，所以是 ST7121。

## P4 芯片修订

原理图上 P4 的引脚 54 标为“NC(VDD_HP_1)”，没有连接，所以这块 PCB 按 v1.x 设计。手上这块板用 esptool v5.4.0 读出的芯片是 ESP32-P4 revision v1.3。

和 JC1060P470 一样，v1.x 在 ESP-IDF v6.1 里最高 360MHz。`boards/m5stack_tab5/sdkconfig.defaults` 选了 `CONFIG_ESP32P4_SELECTS_REV_LESS_V3`，最低修订是 v1.0。项目配置为 CPU 360MHz、32MB PSRAM、PSRAM 时钟 200MHz、X16 模式。flash 是 XM25QH128C，16MB。出厂固件的 bootloader 把 flash 切到 QIO 80MHz，本项目也用 QIO 80MHz。

## IC

| 位号 | 型号 | 用途 |
|---|---|---|
| U1 | ESP32-P4 | 主控，32MB PSRAM 在芯片封装内 |
| U3 | XM25QH128CHIQ | 16MB SPI flash |
| U2 | ESP32-C6-MINI-1U | Wi-Fi 6 和 BLE |
| | ST7121 | 屏幕的显示和触摸一体的驱动 IC，在屏幕模组上 |
| U6、U7 | PI4IOE5V6408 | I2C IO 扩展器，地址 0x43 和 0x44 |
| U8 | ME2212 | 背光升压 |
| U14 | ES8388 | 音频 codec，这块板上只用它播放 |
| U13 | ES7210 | 4 路 ADC，用来录音 |
| U15 | NS4150B | 单声道 D 类功放 |
| U16、U17 | MSM381A3729H9BPC | 模拟 MEMS 麦克风 |
| U19 | BMI270 | 六轴 IMU |
| U30 | RX8130CE | RTC |
| U31 | INA226 | 电池电压和电流监测 |
| U20 | IP2326 | 2 节锂电池充电 |
| U28 | PMS150G-U06 | 原理图注明“定制带程序芯片”，管电源键和开关机 |
| U21 | MP4560 | 把 SYS_VIN（6～24V）降到 5V |
| U24 | SY8113 | 把电池电压降到 5V |
| U4、U5、U22 | SY8089 | 降压，分别输出 SOC_3.3V、P4 的 VDD_HP 和 VBAT_OUT |
| U18 | SIT3088 | RS-485 收发器 |
| U32 | CH9102F | USB 转串口，原理图标为 NC，没有焊 |

摄像头是接在 J4 上的模组。M5Stack 的规格写的是 SC2356，esp-bsp 和出厂固件用的是 SC202CS 的驱动。

## 功能和引脚

### 屏幕

屏幕是 5 寸 720×1280 IPS，MIPI DSI 2 lane，接 J3（24P FPC）。面板本身是竖屏，720 是水平方向。屏幕复位 LCD_RST 不接 GPIO，接在 IO 扩展器 0x43 的 P4 上。出厂固件的注释写着：屏幕这一侧的 LCD_RST 没有 1.8V 上拉，扩展器不能推挽输出 3.3V，所以先把 P4 设成输出低电平复位，再改回输入，靠扩展器内部的上拉释放复位。esp-bsp 在提交 `73ee07b` 里也改成了用弱上拉保持 LCD_RST。

背光由 ME2212 升压驱动，EN 接 GPIO22。EN 上有 R24（5.1K）下拉，所以上电时背光是关的，调光是在 GPIO22 上输出 PWM，esp-bsp 用 5kHz 的 LEDC。DSI PHY 由 P4 片内的 LDO VO3 供电，esp-bsp 和出厂固件都把它设为 2500mV。

三版屏幕的时序来自 esp-bsp：

| 参数 | ILI9881C | ST7123 | ST7121 |
|---|---|---|---|
| 像素时钟 | 60MHz | 70MHz | 70MHz |
| HSYNC 宽度 | 40 | 2 | 2 |
| HBP | 140 | 40 | 40 |
| HFP | 40 | 40 | 40 |
| VSYNC 宽度 | 4 | 2 | 20 |
| VBP | 20 | 8 | 24 |
| VFP | 20 | 220 | 200 |
| lane 速率 | 1000Mbps | 1000Mbps | 965Mbps |
| 算出的帧率 | 约 48.21Hz | 约 57.80Hz | 约 57.27Hz |

出厂固件的 ST7123 也用 965Mbps，其余参数和 esp-bsp 相同。帧率按“像素时钟 ÷（水平总长 × 垂直总长）”算。

项目的 ST7121 配置为 2 lane、RGB565、VO3 2500mV，使用 `esp_lcd_st7121.c.txt` 的初始化序列，之前发送软复位。lane 速率请求保持 965Mbps；按 IDF v6.1 的 PHY 分频规则，实际名义值为 960Mbps。像素时钟设为 80MHz，保留 ST7121 的同步宽度和后肩，HFP 为 110，VFP 为 212，总周期为 `872 × 1536`。计算帧率约 59.7286Hz，接近 GB/GBA 的 59.7275Hz。

P4 的 DPI 时钟由 PLL_F240M 整数分频得到，ESP-IDF v6.1 对 70MHz 会取 3 分频，实际是 80MHz。`mipi_dsi_hal.c` 按请求的像素时钟和实际 lane 速率计算 DSI 主机每行的 lane 时钟数，并调整 bridge 一侧的 HFP；两个行计数都要取整。项目直接请求 80MHz，该配置不需要水平前肩补偿。

项目配置中，两条 lane 分频后的名义总速率为 1920Mbps，80MHz 下 RGB565 的原始像素数据率为 1280Mbps。这项比较不包含 DSI 包开销和状态切换，不能单凭它判断传输余量。M5Stack 只提供了 ST7123 的屏幕规格书，没有 ST7121 的，所以 ST7121 允许的时序范围不知道。

USB-C 口朝右横着拿时，帧缓冲的第 0 行在屏幕左边，第 0 列在屏幕下边，所以横屏显示要把画面旋转 90°。

### 触摸

ST7121 的触摸部分地址是 0x55，和其他器件共用 I2C，见“I2C”。INT 接 GPIO23，J3 第 8 脚，有 R92（5.1K）上拉到 3.3V。触摸复位 TP_RST 接 IO 扩展器 0x43 的 P5。

项目使用 `esp_lcd_touch_st7123`，触摸坐标范围为 720×1280，与帧缓冲坐标一致，不需要交换或翻转。

### 音频

ES8388 负责播放，ES7210 负责录音，两颗芯片共用一组 I2S：

| 信号 | GPIO |
|---|---|
| MCLK | 30 |
| BCLK | 27 |
| LRCK | 29 |
| P4 输出到 ES8388 的 DSDIN（播放） | 26 |
| ES7210 的 SDOUT1 输出到 P4（录音） | 28 |

ES8388 的 ASDOUT 没有接。

ES8388 的 LOUT1、ROUT1 经隔直电容接 3.5mm 耳机座 J5，是立体声。喇叭的功放 NS4150B 只接了 LOUT1：LOUT1 经 C88 接 INP，INN 经 C89 接 AGND，所以喇叭只放左声道，GB 的立体声要先混成单声道再送给喇叭。NS4150B 的使能 CTRL 接 IO 扩展器 0x43 的 P1（SPK_EN），有 R45（10K）下拉，默认关闭。插耳机时喇叭不会自动关，耳机座的检测脚 HP_DET 接 0x43 的 P7，要软件读它，再控制 SPK_EN。出厂固件把 P7 为 1 当作插入了耳机。

NS4150B 由 SYS_INT5V 供电，项目按 5V 设置 codec 的 `pa_voltage`；功放本身的增益从图纸上看不出来，没有计入。SPK_EN 由板子代码通过 IO 扩展器控制，不交给 esp_codec_dev。P7 设成输入，关掉内部上下拉，和出厂固件一样。音频输出每 20ms 读一次 P7，连续三次相同才切换：插着耳机时先关 SPK_EN，再把左右声道原样送出；拔掉耳机时先改成送混好的单声道，再打开 SPK_EN。开机时喇叭先关着，读到耳机的状态以后才打开。

两个麦克风 U16、U17 接 ES7210 的 MIC1 和 MIC2。MIC3 经 R41 接 LOUT1，是回声消除的参考信号。MIC4 接耳机座的麦克风。ES7210 和 ES8388 的电源 AUDIO_VDD 由 U12（ME6211A33）从 SYS_INT5V 降压得到。

### TF 卡

TF 卡用 SDMMC 4 位模式：D0～D3 是 GPIO39～42，CLK 是 GPIO43（经 R23 22Ω），CMD 是 GPIO44。卡的 VDD 和 5.1K 上拉都接 SOC_3.3V。P4 的引脚 85（VDDPST_5）是 GPIO39～48 的 IO 电源，这块板上也接 SOC_3.3V。所以和 JC1060P470 不同，软件不用打开 LDO 就能用卡，卡也切不到 1.8V 的 UHS-I 模式。卡座的 Detect 脚没有接，没有插卡检测。

esp-bsp 和出厂固件都用 `sd_pwr_ctrl_new_on_chip_ldo()` 打开 LDO 通道 4。按原理图，VO4（引脚 74）只接了电容 C29，打开它对 TF 卡没有作用。

GPIO39～44 是 SDMMC slot 0 的 IO MUX 引脚，项目把卡接在 slot 0 上。slot 1 留给 C6 的 SDIO，见“Wi-Fi 和 BLE”。

### 按键和扩展口

板上只有一个按键 S1，接 PMS150G（U28）的 PA4。PMS150G 的 PA3 控制主电源 MPWR_EN，PA5 接 GPIO35（BOOT），PA6 接绿色 LED 和唤醒信号。按 M5Stack 文档：关机时按一下开机，开机时双击关机；长按约 2 秒，等绿色 LED 快闪后松开，进入下载模式。经 USB-C 烧录时 esptool 能自动复位和进下载模式，不需要按键。GPIO35 上还有 R9（10K）上拉，也引到了 M5-Bus。

游戏按键只能接在扩展口上。背面的 M5-Bus（BUS1，2×15）按原理图：

| 脚号 | 信号 | 脚号 | 信号 |
|---|---|---|---|
| 1 | GND | 2 | GPIO16 |
| 3 | GND | 4 | GPIO17 |
| 5 | GND | 6 | CHIP_PU（SOC_RST） |
| 7 | GPIO18 | 8 | GPIO45 |
| 9 | GPIO19 | 10 | GPIO52 |
| 11 | GPIO5 | 12 | 3.3V |
| 13 | GPIO38（U0RXD） | 14 | GPIO37（U0TXD） |
| 15 | GPIO7 | 16 | GPIO6 |
| 17 | I2C SDA（GPIO31） | 18 | I2C SCL（GPIO32） |
| 19 | GPIO3 | 20 | GPIO4 |
| 21 | GPIO2 | 22 | GPIO48 |
| 23 | GPIO47 | 24 | GPIO35（BOOT） |
| 25、27、29 | SYS_VIN | 26 | GPIO51 |
| 28 | SYS_EXT5VO | 30 | VBAT_OUT |

侧面的 2×5 排针 J9：

| 脚号 | 信号 | 脚号 | 信号 |
|---|---|---|---|
| 1 | GND | 2 | SYS_VIN |
| 3 | GND | 4 | GND |
| 5 | SYS_EXT5VO | 6 | 3.3V |
| 7 | GPIO0 | 8 | GPIO1 |
| 9 | GPIO49 | 10 | GPIO50 |

Grove 口 J11：1 脚 GND，2 脚 SYS_EXT5VO，3 脚 GPIO53，4 脚 GPIO54。板上还有一组 STAMP 焊盘，引出 GPIO5、6、18、19、33、46、I2C 和电源。

M5-Bus 上的 GPIO2～7、16～19、45、47、48、51、52，J9 上的 GPIO0、1、49、50，加上 Grove 上的 GPIO53、54，一共 21 个空闲 GPIO。`docs/product-design.md` 里的按键一共 11 个，每个键单独占一个 GPIO 也够用。GPIO0 和 GPIO1 是 P4 的 32kHz 晶振脚，这块板上没有焊 32kHz 晶振。SYS_EXT5VO 由 U26 输出，使能是 IO 扩展器 0x43 的 P2（EXT5V_EN），扩展器复位以后是关的。

### I2C

GPIO31（SDA）和 GPIO32（SCL）是板上唯一一组 I2C，上拉是 R93、R76，2.2K 接 3.3V。总线还引到了 M5-Bus、RS-485 口 J7 和 STAMP 焊盘。摄像头经 TXS0102（U36）转成 1.8V 接到这组总线上，BMI270 经 Q13 接到它自己的 VDD_IMU 电压域。

| 器件 | 7 位地址 |
|---|---|
| ES8388 | 0x10。CE 接 AGND |
| RX8130CE | 0x32 |
| SC202CS（摄像头） | 0x36，esp-bsp 和出厂固件的驱动用这个地址 |
| ES7210 | 0x40。AD0、AD1 都接 AGND |
| INA226 | 0x41。A0 接 3.3V，A1 接 GND |
| PI4IOE5V6408（U6） | 0x43。ADDR 接 GND |
| PI4IOE5V6408（U7） | 0x44。ADDR 接 3.3V |
| ST7121 触摸 | 0x55 |
| BMI270 | 0x68 |

ES7210 和 ES8388 按单个寄存器读取。

### IO 扩展器

两片 PI4IOE5V6408 的引脚按原理图：

| 0x43（U6） | 信号 | 用途 |
|---|---|---|
| P0 | RF_PTH_L_INT_H_EXT | C6 的天线开关，低电平用内置天线，高电平用外接天线（M5Stack 文档） |
| P1 | SPK_EN | NS4150B 使能 |
| P2 | EXT5V_EN | SYS_EXT5VO 输出使能 |
| P3 | | 没有连接 |
| P4 | LCD_RST | 屏幕复位 |
| P5 | TP_RST | 触摸复位 |
| P6 | CAM_RST | 摄像头复位 |
| P7 | HP_DET | 耳机检测，输入 |

| 0x44（U7） | 信号 | 用途 |
|---|---|---|
| P0 | WLAN_PWR_EN | C6 的 3.3V 电源（U38）使能 |
| P1、P2 | | 没有连接 |
| P3 | USB5V_EN | USB-A 口的 5V（U27）使能 |
| P4 | PWROFF_PLUSE | 关机脉冲，出厂固件的 `bsp_generate_poweroff_signal()` 连续发三次 |
| P5 | nCHG_QC_EN | IP2326 的快充设置 |
| P6 | CHG_STAT | IP2326 的状态，输入。出厂固件把它当作 USB-C 插入检测 |
| P7 | CHG_EN | IP2326 充电使能 |

扩展器软件复位后，各脚处于输入、高阻、下拉状态，喇叭、C6、USB-A 供电和充电均未使能。具体寄存器配置见厂商 BSP。

项目用的 `esp_io_expander_pi4ioe5v6408` 1.0.1 在创建器件时会复位芯片，再把方向寄存器（03h）写成 0xFF、输出高阻寄存器 OUT_H_IM（07h）写成 0xFF，也就是所有脚都是输出，但处于高阻。通用驱动的 `esp_io_expander_set_dir()` 和 `set_level()` 都不改 OUT_H_IM，所以要驱动的脚必须先用 `esp_io_expander_set_output_mode(..., IO_EXPANDER_OUTPUT_MODE_PUSH_PULL)` 清掉高阻位，否则写 1 也不会输出高电平。SPK_EN 漏了这一步时，喇叭不出声。

两片扩展器的 nRESET 接 P4 的 CHIP_PU，但 USB-Serial/JTAG 复位不会清除扩展器状态。因此 `board::init()` 必须先软件复位扩展器，避免继承上一固件的配置。

复位以后，`board::init()` 按出厂固件的设置打开手柄要用的电源。编入蓝牙手柄时，它把 0x43 的 P0 拉低，选用内置天线，再打开 0x44 的 P0 给 C6 上电。编入 USB 手柄时，它打开 0x44 的 P3，给 USB-A 口供 5V。

### RTC

RX8130CE 的 VDD 接常开的 VDD_STBY，后备电源是 BT1，经 R38（1K）接到 VBAT。M5Stack 的规格写的是 70000µF/3.3V 的超级电容。它的 /IRQ 经 Q11、Q12 接到唤醒电路 E_TRG，没有接到 P4。

使用 RTC 时间前需要检查日期和 VLF。厂商初始化启用了 INIEN 和 CHGEN；按 [RX8130CE 应用手册](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1132/RX8130CE_cn-Register-Datasheet.pdf)第 4.1 节，这是可充电后备电源的设置。后备电源耗尽以后，这两位和 VLF 一样回到上电值，INIEN、CHGEN 变成 0，不再给超级电容充电，所以设置时间时要把它们重新置 1。

设置时间时要注意下面几点，节号都是应用手册的：

- 按 18.5，先置 1Eh 的 STOP（bit 6），写 10h～16h，再清 STOP，时钟从清 STOP 时开始走。1Eh 的 TEST（bit 7）每次都只能写 0（13.3.5）。
- 星期在 13h，是独热码，bit 0 是星期日，不随日期自动换算（14.1.2）。
- VLF 置位时，所有寄存器都要重新设置（14.5）。项目按 18.4 只设置时钟用到的部分：30h 写 00h，关掉数字微调；1Ch 写 04h；中断全部关闭。
- VLF 写 0 才清除。振荡器起振以后才能清掉，上电后最长要 1 秒（18.1，表 4），所以要清到读回 0 为止，并限制次数。1Dh 的其他标志写 1 时保持不变，清 VLF 写 FDh。
- 一次 I2C 传输要在 0.95 秒内结束，否则芯片复位接口（19.2）。地址在 10h～1Fh 之内自动加一并回绕（19.6）。

### 电池

电池是 NP-F550，2 节串联，7.4V、2000mAh，接 J12，经 Q5 到 SYS_BAT_PRE，再经采样电阻 R39（5mΩ）到 SYS_BAT_2S。INA226 的 VBUS 接 SYS_BAT_2S，IN+ 和 IN- 接 R39 两端，所以能测电池电压和充放电电流。手上这块板没有装电池，INA226 的读数不能当作有效电池电压。

IP2326 从 SYS_5VCHG 给电池充电。SYS_5VCHG 来自 USB-C 的 5V 或 MP4560 输出的 PRE_5V。充电使能 CHG_EN 接 IO 扩展器 0x44 的 P7，扩展器复位以后这个脚是下拉，固件打开之前不会充电，这和 M5Stack 文档里“开机初始化以后才能充电”的说法一致。出厂固件开机后会打开 CHG_EN 和快充。

### IMU 和摄像头

BMI270 的电源 VDD_IMU 是复位芯片 U39（CN809R）的 RESET 输出，U39 接 VDD_STBY。它的 INT1 经 Q8 接到唤醒电路 E_TRG，没有接到 P4。

摄像头接 J4（24P FPC），MIPI CSI 2 lane，1.8V 和 2.8V 由 U10、U11 从 SOC_3.3V 降压得到。MCLK 来自 24MHz 有源晶振 X2，经 U35 转成 1.8V。GPIO36 到 MCLK 那一路上的 R27、R109 都标为 NC，所以 M5Stack 文档里写的 CAM_MCLK（G36）在这块板上没有接通。复位 CAM_RST 接 IO 扩展器 0x43 的 P6。

### USB 和串口

| 原理图 | 连接 |
|---|---|
| J8（USB-C） | 经共模电感 FT1 接 GPIO24（D-）、GPIO25（D+），是 P4 的 USB-Serial/JTAG，全速。项目通过这个口烧录和输出日志 |
| J6（1.25mm 4P） | 和 USB-C 并联：1 脚 VBUS，2 脚 GND，3 脚 D-，4 脚 D+ |
| J10（USB-A） | 接 P4 的 USB 2.0 高速 OTG，用专用引脚。VBUS 由 U27（MT9700）输出，使能是 IO 扩展器 0x44 的 P3。项目在这个口上接 USB 手柄 |

M5Stack 文档把 USB-C 写成“USB 2.0 OTG”，但是按原理图它接的是全速 PHY。CH9102F 没有焊，UART0（GPIO37、38）只引到了 M5-Bus。所以 `boards/m5stack_tab5/sdkconfig.defaults` 把控制台设到了 USB-Serial/JTAG。

RS-485 口 J7（1.25mm 6P）：1 脚 GND，2 脚 SYS_VIN，3 脚 A，4 脚 B，5 脚 I2C SDA，6 脚 I2C SCL。SIT3088 的 DI 接 GPIO20，RO 接 GPIO21，DE 和 /RE 接 GPIO34。

### Wi-Fi 和 BLE

C6 通过 4 位 SDIO 接 P4，跑 ESP-Hosted：

| 信号 | P4 | C6 |
|---|---|---|
| CMD | GPIO13 | IO18 |
| CLK | GPIO12 | IO19 |
| D0 | GPIO11 | IO20 |
| D1 | GPIO10 | IO21 |
| D2 | GPIO9 | IO22 |
| D3 | GPIO8 | IO23 |
| 复位 | GPIO15 | EN |
| | GPIO14 | IO2 |

出厂固件的启动日志里打印的 SDIO 引脚和上表一致，频率是 40MHz。C6 的 EN 经 R4（1K）接 GPIO15，有 R6（10K）上拉和 C32（1µF）电容。C6 的 3.3V 由 U38 从 SOC_3.3V 得到，使能是 IO 扩展器 0x44 的 P0，所以固件不打开它时 C6 不上电。J1（6P）引出了 C6 的 3.3V、TXD0、RXD0、EN、IO9 和 GND，用来单独给 C6 烧录。天线由射频开关 U29 在内置天线和 MMCX 外接天线之间切换，控制脚是 0x43 的 P0。

蓝牙手柄经 C6 连接。P4 上的 ESP-Hosted 用 SDMMC slot 1 和 4 位总线，引脚按上表，用 GPIO15 复位 C6。ESP-Hosted 的 Kconfig 按板型锁定 SDIO 和复位引脚的取值，所以 `boards/m5stack_tab5/sdkconfig.defaults` 选了它的 Tab5 板型 `ESP32P4_TAB5_C6_BOARD`。不选时，引脚会落回乐鑫 P4 开发板的 GPIO14～19 和 GPIO54。

## GPIO 总表

| GPIO | 原理图网络名 | 连接 |
|---|---|---|
| 0、1 | STD_GPIO0、STD_GPIO1 | 扩展口 J9，没有 32kHz 晶振 |
| 2～4 | STD_GPIO2～STD_GPIO4 | M5-Bus |
| 5 | xSPI_SCK_GPIO5 | M5-Bus，STAMP 焊盘 |
| 6 | xMOD_RX_GPIO6 | M5-Bus，STAMP 焊盘 |
| 7 | STD_GPIO7 | M5-Bus |
| 8～11 | SDIO2_D3～SDIO2_D0 | SDIO 数据，接 C6 |
| 12 | SDIO2_CLK | SDIO 时钟，接 C6 |
| 13 | SDIO2_CMD | SDIO 命令，接 C6 |
| 14 | RF_C6_IO2 | C6 的 IO2 |
| 15 | SOC_EXTRF_RST | C6 的 EN，ESP-Hosted 用它复位 C6 |
| 16、17 | STD_GPIO16、STD_GPIO17 | M5-Bus |
| 18 | xSPI_MOSI_G18 | M5-Bus，STAMP 焊盘 |
| 19 | xSPI_MISO_G19 | M5-Bus，STAMP 焊盘 |
| 20 | RS485_TX_GPIO20 | SIT3088 的 DI |
| 21 | RS485_RX_GPIO21 | SIT3088 的 RO |
| 22 | LCD_BL_GPIO22 | 背光，ME2212 的 EN |
| 23 | TP_INT_GPIO23 | 触摸 INT，J3 第 8 脚 |
| 24 | USB_DEVICE_DM | USB-C 和 J6 的 D- |
| 25 | USB_DVICE_DP | USB-C 和 J6 的 D+ |
| 26 | I2S_DIN_MOSI_GPIO26 | I2S 数据，P4 输出到 ES8388 |
| 27 | I2S_SCLK_GPIO27 | I2S BCLK |
| 28 | I2S_ASDOUT_MISO_GPIO28 | I2S 数据，ES7210 输出到 P4 |
| 29 | I2S_LRCK_GPIO29 | I2S LRCK |
| 30 | I2S_MCLK_GPIO30 | I2S MCLK |
| 31 | xG31_SYS_SDA | I2C SDA |
| 32 | xG32_SYS_SCL | I2C SCL |
| 33 | MOD_SPI_CS_G33 | STAMP 焊盘 |
| 34 | RS485_DIR_GPIO34 | SIT3088 的 DE 和 /RE |
| 35 | BOOT_GPIO35 | PMS150G 的 PA5，M5-Bus，R9（10K）上拉 |
| 36 | CAM_MCLK_GPIO36 | R10（10K）上拉，另一端只接到没有焊的 R27 |
| 37 | U0TXD_GPIO37 | M5-Bus |
| 38 | U0RXD_GPIO38 | M5-Bus |
| 39～42 | SDIO1_D0～SDIO1_D3 | TF 卡 |
| 43 | SDIO1_SCK | TF 卡 |
| 44 | SDIO1_CMD | TF 卡 |
| 45 | STD_GPIO45 | M5-Bus |
| 46 | STD_GPIO46 | STAMP 焊盘 |
| 47、48 | STD_GPIO47、STD_GPIO48 | M5-Bus |
| 49、50 | STD_GPIO49、STD_GPIO50 | 扩展口 J9 |
| 51、52 | STD_GPIO51、STD_GPIO52 | M5-Bus |
| 53、54 | STD_GPIO53、STD_GPIO54 | Grove 口 J11 |

GPIO34～38 是 P4 的 strapping 脚（P4 数据手册“Strapping 管脚”一节）。这块板上 GPIO35 由 PMS150G 驱动，GPIO34 接 RS-485 收发器，接按键时不要用这几个脚。

## 电源

整板的 5V 有三个来源：USB-C 的 5V（USB_5VIN）；SYS_VIN，6～24V，从 M5-Bus、J9 或 RS-485 口输入，经 MP4560 降到 5V（PRE_5V）；电池，经 SY8113 降到 5V。USB_5VIN 和 PRE_5V 经两颗理想二极管 MAX40200 合成 SYS_5VBUS，再经 U23 得到整板的 SYS_INT5V。U23 和 SY8113 的使能都是 PMS150G 输出的 MPWR_EN。按 M5Stack 的框图，只有没有外部电源时才由电池供电。

SYS_INT5V 经 SY8089（U4）降到 SOC_3.3V。P4 的 IO 电源和片内 LDO 的输入、TF 卡、IO 扩展器和 I2C 上拉都接在这一路上，P4 的 VDD_HP 也由 U5 从它降压得到。SYS_INT5V 还直接给背光升压、NS4150B 和 AUDIO_VDD 的 LDO 供电。U25 从 SYS_5VBUS 或电池得到常开的 VDD_STBY（3.3V），给 PMS150G、RX8130CE、BMI270 和绿色 LED 供电。

P4 片内 LDO 的接法决定了软件要打开哪几路：

| LDO | 接到 | 说明 |
|---|---|---|
| VO1 | flash 的 VCC 和 VDDPST_3（引脚 30） | flash 供电 |
| VO2 | 引脚 59 和 67 | VDD_PSRAM，给封装内的 PSRAM 供电 |
| VO3 | VDD_MIPI_DPHY（引脚 41） | esp-bsp 和出厂固件设为 2500mV |
| VO4 | 只接了电容 C29 | 不用 |

其余 VDDPST 引脚都接 SOC_3.3V，所以 P4 的 GPIO 全部是 3.3V IO，不需要像 JC1060P470 那样先打开 LDO。

## 出厂固件

M5Tab5-UserDemo 在 GitHub Releases 上发布了从 0x0 开始的完整镜像：

| 文件 | 版本说明 | 大小 | SHA-256 |
|---|---|---|---|
| [`C145-Tab5-UserDemo-V0.3-20260325_0x0.bin`](https://github.com/m5stack/M5Tab5-UserDemo/releases/download/V0.3/C145-Tab5-UserDemo-V0.3-20260325_0x0.bin) | 加入 ST7121 支持 | 5817296 字节 | `a09a06813e3c7c9dfdaef7ca8a4d22d6996949398b6a8adb96a128bf78ddecca` |
| [`C145-Tab5-UserDemo-V0.4-20260602_0x0.bin`](https://github.com/m5stack/M5Tab5-UserDemo/releases/download/V0.4/C145-Tab5-UserDemo-V0.4-20260602_0x0.bin) | 修复更新 C6 固件以后 Wi-Fi 初始化崩溃 | 5813840 字节 | `2d30fa8918bac1e8d09b59eb2fc72ea40500d097efa4812e241ac875ee86e781` |

手上这块板的出厂镜像确认为 V0.3。

恢复出厂程序时，接 USB-C，把镜像写到 0x0：

```
eim run "esptool --chip esp32p4 -p <端口> -b 460800 write-flash 0x0 C145-Tab5-UserDemo-V0.3-20260325_0x0.bin" v6.1
```

本项目的固件只写 P4，不改动 C6。

## 来源

- [M5Stack Tab5 文档页](https://docs.m5stack.com/en/core/Tab5)，规格、更新记录、PI4IOE5V6408 的引脚分配、开关机和下载模式的操作。页面上还有 ST7123 的屏幕规格书和触摸协议，以及 RX8130CE、INA226、IP2326 等器件的数据手册。
- [Tab5 原理图 PDF](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1132/Tab5_Schematics_PDF.pdf)、[整机框图](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1132/Tab5_Overall_Design_Block_Diagram.pdf)。
- [esp-bsp 的 `bsp/m5stack_tab5`](https://github.com/espressif/esp-bsp/tree/f259a6abd5808019b1903a422092a4234ca95a51/bsp/m5stack_tab5)，提交 `f259a6abd5808019b1903a422092a4234ca95a51`，屏幕时序、板子版本的探测方法。
- [M5Tab5-UserDemo](https://github.com/m5stack/M5Tab5-UserDemo/tree/b4e356bc491ca070d54004718dad789c07d5fc93)，提交 `b4e356bc491ca070d54004718dad789c07d5fc93`，出厂固件的源码和发布的镜像。
- [`esp_lcd_touch_st7123` 1.0.2](https://components.espressif.com/components/espressif/esp_lcd_touch_st7123)，读触摸固件信息用的寄存器地址。
- [RX8130CE 应用手册](https://m5stack-doc.oss-cn-shenzhen.aliyuncs.com/1132/RX8130CE_cn-Register-Datasheet.pdf)，寄存器表、后备电源的接法和设置时间的步骤。文件名带 `_cn`，内容是 Epson 的英文版 ETM50E-09；Epson 网站上的 [ETM50E-10](https://download.epsondevice.com/td/pdf/app/RX8130CE_en.pdf) 只改了时间更新中断的说明，节号和页码相同。
- ESP-IDF v6.1 的 `components/esp_hal_lcd/mipi_dsi_hal.c`，DPI 时钟分频和 DSI 主机行时序的计算。
- [ESP32-P4 数据手册](https://documentation.espressif.com/esp32-p4_datasheet_en.html)、[ESP32-P4 v3.x 修订说明](https://documentation.espressif.com/esp32-p4-chip-revision-v3.x_user_guide_en.pdf)。

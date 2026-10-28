# Sila P4

项目代号是 `sila_p4`，板级名称是 Sila P4。资料包里的底板原理图标为 `ESP32P4_4IN_V3.1`，配套示例选择了 `sila-p4-3.95in-box`。它采用 ESP32-P4 模组和 ESP32-C6，无线连接通过 C6 完成。配套 MIPI DSI 屏幕在厂商示例中配置为 720×720。

项目固件已经接入屏幕、触摸和 SD 存储。下文说明硬件连接、初始化约束和项目使用的配置。

## 型号和版本

手上这块板经 CH340 串口读取，esptool v5.5.0 报告 ESP32-P4 revision v1.3、40MHz 晶振、16MB flash。原固件启动日志报告 32MB PSRAM、200MHz、X16 模式，PSRAM 厂商 ID 是 0x0d（AP），CPU 是 360MHz。

底板 PDF 的标题是 `ESP32P4_4IN_V3.1`，图纸日期为 2025-10-29，但标题栏的 VER 字段又写了 V1.0。模组 PDF 标题是 `ESP32P4模组`，版本 V1.2，日期为 2025-09-01。实物 PCB 丝印和屏幕排线型号尚未确认，不能把这些图纸版本当成实物识别结果。

原固件是 `xiaozhi` 2.2.4，使用 ESP-IDF v5.5.4，运行时 SKU 是 `sila-p4-3.95in-box`。它报告 ST7703 驱动 1.0.5，并读出了 GT911 的产品 ID `39 31 31`。资料包里的小智 2.03 示例则没有 Sila 触摸初始化，SD 初始化也只有一条日志，所以它不是这份原固件的完整对应源码。

## 资料

开发需要的图片和参考源码保存在 `refs/boards/sila_p4/`，不依赖原始下载或解压目录。底板资料来自用户提供的 `P4-4IN` 资料包，没有随包提供的底板上游仓库地址。

原理图按电路区域保存，保留完整连接与标号。

| 图片，均位于 `schematic/` | 原文件和裁剪内容 |
|---|---|
| `1_PWR.png` | 底板 `ESP32P4_4IN_V3.1.pdf` 第 1 页：主板与音频降压、指示灯、电池接口、充电和电量计、供电切换、开关机与插电检测 |
| `2_LCD.png` | 同页：FPC1、触摸上拉和背光驱动 |
| `3_LCD_PINOUT.png` | 同页左侧附带的 30Pin 屏幕引脚表，原始图片分辨率较低，右侧文字在原图中已有截断 |
| `4_CONN.png` | 同页：底板 U23 的模组连接、strapping 和复位上拉 |
| `5_IO.png` | 同页：Type-C、CH340、SD 卡和 GPIO47 按键 |
| `6_CODEC.png` | 同页：ES8311、ES7210、麦克风与 AEC、功放和输出接口 |
| `7_CAMERA.png` | 同页下方：摄像头接口、供电、复位、晶振和 I2C 电平转换 |
| `8_MODULE.png` | `ESP32P4模组V1.2.pdf` 第 1 页左侧：P4、C6、flash、晶振、LDO 和降压电路 |
| `9_MODULE_PINOUT.png` | 同页右上：模组 U3 的焊盘定义 |

下面的源码均直接复制到仓库，以 `.txt` 后缀作为参考保存，没有改写内容。小智示例来自 `esp32p4_4in_st7703_mipi-xiaozhi_2.03.zip`，camera-test 来自 `camera-test.zip`；表中路径相对于各自的工程根目录。它们不是已接入本项目的驱动，也不是完整的示例工程。

| 仓库中的参考文件 | 保留的内容 | 原路径 |
|---|---|---|
| `config.h.txt` | Sila 的音频、LCD、SD 和按键引脚，显示方向、分辨率和电源参数 | 小智 `main/boards/sila-p4-3.95in-box/config.h` |
| `sila-p4-3.95in-box.cc.txt` | 音频 I2C、MIPI 供电、46MHz 显示时序、屏幕初始化和板级调用顺序 | 小智 `main/boards/sila-p4-3.95in-box/sila-p4-3.95in-box.cc` |
| `box_audio_codec.cc.txt` | ES8311/ES7210 的配置、I2S/TDM 时钟与 slot、声道和功放设置 | 小智 `main/audio/codecs/box_audio_codec.cc` |
| `camera-app_lcd.c.txt` | camera-test 的 LCD 初始化、5kHz 背光 PWM 和上电顺序 | camera-test `main/app_lcd.c` |
| `camera-app_lcd.h.txt` | 上述代码用到的分辨率、像素格式和帧缓冲宏 | camera-test `main/app_lcd.h` |
| `esp_lcd_st7703.c.txt` | `waveshare/esp_lcd_st7703` 1.0.5 的完整初始化表 `vendor_specific_init_default`、复位和控制命令 | 组件 `esp_lcd_st7703.c` |
| `esp_lcd_st7703.h.txt` | 同一组件的初始化结构、lane 速率和默认时序宏 | 组件 `include/esp_lcd_st7703.h` |

ST7703 参考源码和 `camera-app_lcd.h.txt` 的许可为 Apache-2.0，`camera-app_lcd.c.txt` 的许可为 CC0-1.0，原文件头的版权和 SPDX 声明均保留。三个小智源文件没有单独的许可声明。

原始 PDF 和资料包的 SHA-256：

| 文件 | SHA-256 |
|---|---|
| `ESP32P4_4IN_V3.1.pdf` | `0546b0a37be8cdb1ed57cd6aac68c892e61687e31dbdff921ab6139113b380f5` |
| `ESP32P4模组V1.2.pdf` | `ff7b3701f5ca8e0905fda18775d71799eeff9eae830f83dcd70ac428751b8ebc` |
| `esp32p4_4in_st7703_mipi-xiaozhi_2.03.zip` | `a587df03229c2c107a54624ec7993d0c47cca6c78bd998829c9bfca9da1e6469` |
| `camera-test.zip` | `cf94bd0f9c35abbdfeb211dab7653ff012aec73dd298eabb4fe786581a66aae9` |

`camera-test.zip` 的 ZIP 注释是 `fc1c4f79396b6657b7a0ae7f7995426fff427618`，但没有对应仓库地址。完整示例、烧录工具、媒体素材和固件镜像都没有入库。显示驱动从组件仓库固定到 1.0.5；小智示例自身把这个依赖写成了 `'*'`，也没有 lock 文件，不能据此确定它当时实际编译的版本。

## 主控和存储

P4 v1.3 使用 ESP-IDF v6.1 的旧修订配置：`CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y`、最低修订 v1.0、CPU 360MHz。esptool 的 Features 字段虽然列出 400MHz，但不能代替 v1.x 的主频限制。模组图上 P4 引脚 54 标为 `NC(VDD_HP_1)`，也与 v1.x 的设计相符。v3.x 需要另选修订配置，不能沿用本板的 defaults。

手上板子的 flash JEDEC ID 是 `85 2018`，容量 16MB，与模组图标注的 W25Q128JV 不一致，具体装片型号尚未确认。项目使用 QIO 80MHz。PSRAM 使用 HEX 接口、200MHz，启用开机内存检查。配置以 `boards/sila_p4/sdkconfig.defaults` 为准。

## IC

部分底板电路只给了功能方框，没有展开元件位号，表里不补猜测。

| 位置 | 型号 | 用途 |
|---|---|---|
| 模组 U1 | ESP32-P4NRW32 | 主控，封装内 32MB PSRAM |
| 模组 U2 | ESP32-C6FH4 | 无线协处理器 |
| 模组 U4 | 图纸为 W25Q128JV，实物型号待核对 | 16MB SPI flash |
| 屏幕 | ST7703、GT911 | 分别负责显示和触摸，型号依据示例及原固件日志 |
| 底板 UART 方框 | CH340X | USB 转 UART，手上板子的 VID:PID 为 `1a86:7523` |
| 底板音频方框 | ES8311、ES7210 | DAC/codec 和多通道 ADC |
| 底板功放方框 | NS4150B | 单声道功放 |
| 底板背光方框 | SY7200 | 背光 LED 驱动 |
| 底板电源方框、摄像头 U27/U28 | TLV62569 | 主板、音频和摄像头的降压电源 |
| 模组 U5 | TLV62569DBVR 等候选型号 | P4 的 VDD_HP 降压，图纸列出多个替代料 |
| 底板充电方框 | TP4057 | 电池充电 |
| 底板电量方框 | CW2015CHBD | 电量计；图纸称为 BMS，但没有展开电池保护电路 |
| 底板 U22 | SAM8108 | 按键开关机，控制 PWR_EN |

## 屏幕和触摸

### 显示

厂商 Sila 示例的显示配置为 720×720，使用 P4 专用 MIPI DSI 引脚、2 lane。LCD_RST 接 GPIO24，背光 LCD_BL 接 GPIO29，高电平开启。底板图的屏幕接口是 FPC1；图中同时出现“4IN 720P”和“2.8IN 480P”标注，本文只采用小智 Sila 示例的 720×720 配置。

MIPI PHY 使用 LDO3，示例设为 2500mV。模组图把这一路叫 `LDO_VO3_1V8`，这是网络名与软件设置的冲突，不能照网络名把初始化电压改成 1.8V。

两个示例使用的时序并不相同，下面按 HSYNC/HBP/HFP、VSYNC/VBP/VFP 的顺序列出：

| 参数 | 小智 2.03 的 Sila 配置 | ST7703 1.0.5 默认宏，camera-test 调用此宏 |
|---|---|---|
| 分辨率 | 720×720 | 720×720 |
| 像素格式 | RGB565，RGB 顺序 | 由调用方指定 |
| 像素时钟设置 | 46MHz | 38MHz |
| 水平时序 | 20 / 80 / 80 | 20 / 50 / 50 |
| 垂直时序 | 4 / 12 / 30 | 4 / 20 / 20 |
| lane 速率 | 使用组件宏；1.0.5 为每 lane 480Mbps | 每 lane 480Mbps |
| 按时序推算的帧率 | 约 66.72Hz | 约 59.21Hz |

camera-test 使用 RGB565 和两个帧缓冲。上表帧率由时序推算，不能当作实测值；资料没有提供面板的完整规格书和允许时序范围。

项目以 GB/GBA 的 `4194304 / 70224 ≈ 59.7275 Hz` 为刷新目标，使用 40MHz 像素时钟，水平时序为 `20 / 80 / 52`，垂直时序为 `4 / 12 / 32`。总周期为 `872 × 768`，计算得到约 59.7286Hz，比目标高约 18ppm。40MHz 可由默认的 240MHz 时钟源整数分频得到，不需要 IDF 自动补偿水平前肩；时钟取整和补偿逻辑见 IDF v6.1 的 `esp_hal_lcd/mipi_dsi_hal.c`。lane 速率保持 480Mbps。

初始化和复位时序见 `esp_lcd_st7703.c.txt`。项目采用小智 Sila 示例的显示方向，不交换轴、不翻转。

### 触摸

按底板图，GT911 使用独立的 I2C：SDA GPIO26、SCL GPIO27，R50/R51 是 1kΩ 上拉到 3.3V。TP_RST 接 GPIO28，TP_INT 接 GPIO25。实物的 TP_RST 连接尚未确认。不要抄同一资料包里 `waveshare-p4-wifi6-touch-lcd-4b` 的触摸配置，那份代码使用了不同的引脚和总线。

GT911 的地址由复位期间的 INT 电平选择：低电平为 0x5d，高电平为 0x14。项目使用 `esp_lcd_touch_gt911` 1.2.1，触摸坐标与显示坐标一致。显式指定 0x5d，使驱动执行包含 INT 的地址选择流程；仅操作 RST 不能保证初始化成功。

## 音频

ES8311 和 ES7210 共用 I2C，SDA GPIO7、SCL GPIO8，R53/R54 是 1kΩ 上拉到 3.3V。I2S 连接如下：

| 信号 | GPIO |
|---|---|
| MCLK | 13 |
| BCLK | 12 |
| WS/LRCK | 10 |
| P4 输出到 ES8311 | 9 |
| ES7210 输出到 P4 | 11 |
| NS4150B 的 PA_CTRL | 53 |

ES8311 输出接 NS4150B，功放输出 VOP/VON 接喇叭接口 U17 和 PJ-342 接口 CN1。图纸没有给出立体声耳机接法，也没有引出耳机检测 GPIO，不能按常见的立体声耳机口处理 CN1。功放的 VIN 在底板图里是 `AU_+3.3V`，小智示例却把 `hw_gain.pa_voltage` 写成 5.0；配置增益时应核对实际供电。项目按 3.3V 设置 `pa_voltage`，codec 的输出电平受这个供电限制。NS4150B 本身的增益从图纸上看不出来，没有计入。

PA_CTRL（GPIO53）在板子初始化时设为输出低电平，codec 打开以后才拉高。喇叭是单声道，GB 的立体声先把左右声道平均再输出。

底板画了两个麦克风方框，ES7210 的第三路还接了 ES8311 OUTP/OUTN，作为 AEC 参考。原固件启用 MIC1～MIC4 和 TDM 的日志不表示有四只实体麦克风。播放和录音需要分别验证。

音频配置参考 `box_audio_codec.cc.txt`，播放和录音的参数需要分别核对。

## TF 卡、按键和 IO 电源

TF 卡使用 SDMMC 4 位模式，D0～D3 为 GPIO39～42，CLK 为 GPIO43，CMD 为 GPIO44。底板图把卡的 VDD 接到固定 `+3.3V`。模组图则把 P4 引脚 85（VDDPST_5，图中写作 VDD_IO_5）接到 LDO4 输出，给 GPIO39～48 供电。

SD 信号线是否装有外部上拉，现有图纸无法确认。平台启用了内部上拉辅助，但 ESP-IDF 的 SDMMC 说明要求外部上拉，内部上拉不能代替它。

**使用 SD 或 GPIO47 按键前需要打开 LDO4。** 3.3V IO 模式可通过 `esp_ldo_acquire_channel()` 请求 3300mV；在 P4 的 IDF 实现中这是输入电源直通模式。LDO4 不控制卡本身的 VDD，不能把它描述成 TF 卡电源开关，也不能仅凭这个连接就启用 1.8V UHS。切换 LDO4 会同时改变整个 GPIO39～48 电压域。`board::init()` 将它固定在 3300mV，供电不依赖 SD 卡是否挂载。

用户按键是 GPIO47 对地开关，示例的 `BOOT_BUTTON_GPIO` 指向它，但它不是 P4 的下载 strapping。GPIO35 才是 BOOT，GPIO35 和 GPIO36 都有 10kΩ 上拉。另一个按键接 SAM8108，控制开关机，不由 P4 直接采样。

图纸将 GPIO34 标为 TYPE_CHECK：Q3 在 USB_5V 存在时把它拉低，R105 为 10kΩ 上拉。这个信号用于 USB 供电检测，实物的有效电平尚待确认。

## 电池和供电

USB_5V 和 BAT+ 通过 D9/Q1 组成供电选择，输出网络叫 `+5V`。电池供电时，图中没有把电池升到 5V 的电路，因此不能由网络名推断实际电压是 5V。主板和音频各有一组 TLV62569 降到 3.3V，EN 都由 SAM8108 的 PWR_EN 控制。

TP4057 从 USB_5V 给电池充电。图纸将 CW2015CHBD 的 SS_SDA 接到 GPIO3、SS_SCL 接到 GPIO2，实物的装片和连接尚待核对。这里的 2 和 3 是 GPIO 号，不是 I2C 控制器编号。底板没有展开充电电流设定、电池保护和电量计外围电路，这些参数尚不明确。

片内 LDO 的连接来自模组图：

| LDO | 连接 |
|---|---|
| VO1 | flash VCC 和 flash IO 电源，网络名为 `LDO_VO1_3V3` |
| VO2 | 封装内 PSRAM，网络名为 `LDO_VO2_1V9` |
| VO3 | MIPI PHY，网络名写 1.8V，软件请求 2.5V |
| VO4 | GPIO39～48 的 VDDPST_5；底板的 VFB_VO4 模组焊盘没有接出 |

SY7200 方框的电源端口标为 `+5V`，连接的网络却标为 `+3.3V`，背光实际输入电源也需要核实。本文没有把这些网络名当成电压测量值。

背光调光用 GPIO29 上的 PWM。图纸只写了 SY7200，没有写后缀。按 Sila P4X 用的 SY7200A 的手册，EN/PWM 调光频率范围是 20kHz～1MHz（见 [Sila P4X 文档](sila_p4x.md)），所以固件用 25kHz、11 位的 LEDC，不用 camera-test 的 5kHz。

## C6、摄像头和扩展

模组内 C6 的 SDIO 接 P4 GPIO14～19，顺序为 D0=14、D1=15、D2=16、D3=17、CLK=18、CMD=19。C6 EN 经模组 R6 接 P4 GPIO54，WAKEUP 经 R5 接 GPIO6。项目不改写 C6。

摄像头接 FPC3，使用专用 MIPI CSI 2 lane。GPIO20 的 CSI_EN 控制 1.8V/2.8V 两组 TLV62569，GPIO21 经过 Q4 控制摄像头复位，GPIO22 为 I2C SCL、GPIO23 为 SDA。I2C 通过 BSS138 电平转换到 1.8V，24MHz MCLK 来自独立晶振 U29。

摄像头模组的排线标记为 `RH36-ESP32-P4-V0`，对应[商品图](https://gw.alicdn.com/imgextra/i4/461692664/O1CN01cDWSsZ1VYAAicjc3m_%21%21461692664.jpg)标注为 SC2336，与 camera-test 配置一致。实物传感器型号尚待核对。

底板图没有标出通用扩展排针。下表的未连接 GPIO 只是图纸上的模组焊盘状态，不表示能直接接游戏按键；需要先确认实物是否引出。

## GPIO 总表

| GPIO | 网络或用途 | 连接与限制 |
|---|---|---|
| 0、1 | 模组 GPIO0、GPIO1 | 底板未连接，P4 的 32kHz 晶振复用脚 |
| 2、3 | SS_SCL、SS_SDA | 电量计 |
| 4、5 | 模组 GPIO4、GPIO5 | 底板未连接 |
| 6 | 模组 C6_WAKEUP | 模组内部连 C6，底板未连接 |
| 7、8 | I2C_SDA、I2C_SCL | 音频控制总线 |
| 9～13 | I2S_DO、WS、DI、BCK、MCK | 音频数据和时钟 |
| 14～19 | SDIO_D0～D3、CLK、CMD | 模组内 C6 |
| 20、21 | CSI_EN、CSI_RESET_SET | 摄像头电源和复位控制 |
| 22、23 | ESP_I2C_SCL、ESP_I2C_SDA | 摄像头，经电平转换 |
| 24 | LCD_RST | 不能同时用作 USB-Serial/JTAG D- |
| 25 | TP_INT | 不能同时用作 USB-Serial/JTAG D+ |
| 26～28 | TP_SDA、TP_SCL、TP_RST | GT911 |
| 29 | LCD_BL | 背光使能/PWM |
| 30～33 | 模组 GPIO30～33 | 底板未连接 |
| 34 | TYPE_CHECK | USB 供电检测，strapping 脚 |
| 35 | IO35/BOOT | 下载 strapping，接 CH340 自动下载电路 |
| 36 | IO36 | 10kΩ 上拉，strapping 脚 |
| 37、38 | MCU_TXD、MCU_RXD | UART0，接 CH340；也属于 strapping 脚 |
| 39～44 | SD_D0～D3、SD_CLK、SD_CMD | SDMMC，依赖 LDO4 |
| 45、46 | 模组 GPIO45、GPIO46 | 底板未连接，属于 LDO4 电压域 |
| 47 | IO47 | 用户按键，依赖 LDO4 |
| 48 | 模组 GPIO48 | 底板未连接，属于 LDO4 电压域 |
| 49～52 | 模组 GPIO49～52 | 底板未连接 |
| 53 | PA_CTRL | 功放使能 |
| 54 | 模组 C6CHIP_PU | 模组内 C6 复位，底板未连接 |

## 烧录和恢复

烧录接口是 CH340 转 UART0，VID:PID 为 `1a86:7523`。板级控制台选择 `CONFIG_ESP_CONSOLE_UART_CUSTOM=y`，UART0 仍使用 GPIO37/38，波特率由公共配置设为 460800。`CONFIG_ESP_CONSOLE_SECONDARY_NONE=y` 关闭了默认的 USB-Serial/JTAG 辅助控制台，因为 GPIO24/25 用于屏幕和触摸。esptool 可以通过 DTR/RTS 自动进入下载模式和复位，无需按 GPIO47 用户键。

底板 Type-C 方框还画了另一组 USB_DP/DM，通向 P4 的专用高速 USB 引脚；SERIAL_DP/DN 才通向 CH340。图纸没有展开两个物理口的具体排列、CC 和 VBUS 保护。实物上有两个 Type-C 口，板子由串口供电时，插在串口旁边那个口上的 DualSense 不亮，P4 也检测不到设备，所以这个口按不对外输出 5V 处理，这块板子没有打开 `GBX_USB_GAMEPAD`。项目使用 CH340 链路烧录和读取日志。

使用 esptool 时应指定 `--chip esp32p4`，不能照资料包中 ESP32-S3 的通用教程操作。完整 flash 备份读取使用 230400 baud，备份文件需要读回校验。

手上板子的原固件是小智 2.2.4，完整备份保存在本地，不含 C6 flash。

它与资料包里四个 `.bin` 的对应前缀都不相同，不能把那些文件当作已经确认的原机恢复镜像。需要恢复这块板时，把完整备份写到 0x0：

```sh
eim run 'esptool --chip esp32p4 -p /dev/cu.usbserial-11320 -b 460800 write-flash 0x0 ~/Downloads/P4-4IN/local-backup/esp32p4-4in-before-gbx.bin' v6.1
```

## 来源

- 用户提供的底板和模组 PDF，版本、校验值见“资料”一节。GPIO、供电和外围连接以这两份图纸为依据。
- 用户提供的小智 2.03、camera-test 示例。所需源码保存在 `refs/boards/sila_p4/`，原路径和资料包校验值见“资料”一节。
- [`waveshare/esp_lcd_st7703` 1.0.5](https://components.espressif.com/components/waveshare/esp_lcd_st7703/versions/1.0.5)，初始化序列、复位时序和 DSI/DPI 宏。
- [`espressif/esp_lcd_touch_gt911` 1.1.3](https://components.espressif.com/components/espressif/esp_lcd_touch_gt911/versions/1.1.3)，地址选择和寄存器读取流程。
- [`espressif/esp_codec_dev` 1.4.0](https://components.espressif.com/components/espressif/esp_codec_dev/versions/1.4.0)，ES8311/ES7210 的控制接口。
- ESP-IDF v6.1：`components/esp_hw_support/port/esp32p4/Kconfig.hw_support`、`components/esp_system/port/soc/esp32p4/Kconfig.cpu`、`components/esp_psram/esp32p4/Kconfig.spiram`；LDO 直通模式见 `components/esp_hal_pmu/esp32p4/include/hal/ldo_ll.h`。
- [ESP32-P4 数据手册](https://documentation.espressif.com/esp32-p4_datasheet_en.html)的管脚、电源域和 strapping 说明；[P4 v3.x 修订说明](https://documentation.espressif.com/esp32-p4-chip-revision-v3.x_user_guide_en.pdf)的 v1.x/v3.x 差异。

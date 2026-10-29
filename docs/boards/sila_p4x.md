# Sila P4X AIBox

项目板名为 `sila_p4x`，对应用户确认的 `ESP32P4X-AiBox` 丝印。主控为 ESP32-P4 v3.2，Flash JEDEC ID 为 `EF 70 19`，Flash 和 PSRAM 容量均为 32MiB。用户提供的 PCB 预览图标为 `ESP32P4X_AiBox R3 2026-9-10`，原理图标题为 `P4X`、V1.0，主图更新日期为 2026-09-14。实物 PCB 的小版本仍需对照丝印。

本项目的屏幕是 6.2 英寸 AXS15260D 模组，452×1280，显示和触摸都由这颗 AXS15260D 负责。连接器、时序和触摸见“屏幕与触摸”。

## 资料

用户提供的 `SCH_P4X_2026-10-13.pdf` 共 11 页，SHA-256 为 `c078ca12eef2976b49a3e77402b0a6da3e35fe87c15c024efede1aed7e19ef80`。文件名日期不等于各图页的修订日期。图纸按电路区域裁为 216dpi PNG，保存在 `refs/boards/sila_p4x/schematic/`；`crops.json` 记录原文件校验值、PDF 页码和裁剪坐标，原 PDF 不入库。

| 裁图文件 | 原 PDF 页 | 内容 |
|---|---|---|
| `01-pmu.png` | 1 | AXP2101、供电分配、电池和 IRQ 电平转换 |
| `02-p4-wiring.png`、`19-i2c-pullups.png` | 1 | P4 网络分配及两路 I2C 上拉 |
| `03-c5.png` | 1 | C5 模组、SDIO、strapping、EN/BOOT 和 U14 |
| `04-display.png` | 1 | FPC1 全部引脚、触摸共用复位和屏幕电源 |
| `05-camera.png`、`06-sensors.png` | 1 | 摄像头 FPC3、24MHz 时钟、电平转换与 SC7A20HTR |
| `07-audio.png`、`08-usb-keys-expansion.png` | 1 | 音频连接、USB、按键、SD 和 CN1 扩展接口 |
| `09-p4-chip-power-flash.png`、`20-mipi-phy-power.png` | 2 | P4 芯片、电源域、40MHz 晶振、strapping、Flash 和 PHY 供电 |
| `10-backlight.png` | 3 | SY7200 背光驱动 |
| `11-left-amplifier.png`、`12-right-amplifier.png` | 4、5 | 两路 NS4150B 功放 |
| `13-microphone-1.png`、`14-microphone-2.png` | 6、7 | 两个模拟麦克风的外围电路 |
| `15-codec.png` | 8 | ES8389 全部连接和模拟外围 |
| `16-usb-connectors.png` | 9 | 两个 Type-C、CC 电阻和 USBLC6-2SC6 保护 |
| `17-sdcard.png` | 10 | SD 卡电源开关、卡检测和外部上拉 |
| `18-cpu-dcdc.png` | 11 | TLV62569 的 P4 1.2V 电源 |

上游代码固定到 [Ergou-/esp32-p4xc5-aibox](https://gitee.com/Ergou-/esp32-p4xc5-aibox/tree/8c6bc38bc7a082d9c4475e08c40778442a8dd8be) 的提交 `8c6bc38bc7a082d9c4475e08c40778442a8dd8be`。所需摘录保存在 `refs/boards/sila_p4x/`，每份记录原路径、行号和原文件 SHA-256：

| 文件 | 上游内容 |
|---|---|
| `upstream-config.txt` | `sdkconfig.defaults`，芯片修订、PSRAM、C5 SDIO 和外围引脚 |
| `upstream-display.c.txt` | `main/axs15260_lcd.c`，复位、背光、DSI PHY 供电和面板创建 |
| `upstream-axs15260.c.txt` | `components/esp_lcd_axs15260` 的头文件和源码，AXS15260D 时序、初始化序列和触摸报告格式 |
| `upstream-touch.c.txt` | `main/usb_screen/app_touch.c`、`main/cst826_touch.c`，共用复位、INT、I2C、读取节奏和坐标旋转 |
| `upstream-power.cpp.txt` | `main/pmu_axp2101.cpp`，PMU 地址、总线和电源配置边界 |
| `upstream-sd.c.txt` | `main/sd_scanner.h`、`main/sd_scanner.c`，SD 引脚、slot 和 LDO4 |
| `upstream-audio.c.txt` | `main/es8389_sound.c`、`main/Kconfig.projbuild`，音频和传感器配置 |

上游 README 开头称默认屏幕为 6.2 英寸 AXS15260D，`main/usb_screen/README.md` 把它列为可选面板，固定提交的配置实际选择 ST7701 2.1 英寸圆屏。本项目按 AXS15260D 配置，依据是上游的 `components/esp_lcd_axs15260`、`main/axs15260_lcd.c` 和 `main/usb_screen/app_touch.c`。

### PCB 预览图

[正面](../../refs/boards/sila_p4x/pcb/front.png)和[背面](../../refs/boards/sila_p4x/pcb/back.png)分别取自用户提供的 `tmp/pcb1.jpg`、`tmp/pcb2.jpg`，只保留拼板上半块，去掉工艺边和重复板。原图 SHA-256 分别为 `b856c16976e8814480ac6f75b39982eb5f254c55c13f6c85bcf188f703519d5e`、`381bf1e2dfb86f5d1e5c057c3969d719f9100b85464c52a65ea6327bd10c0613`。

图中 ES8389、SC7A20、AXP2101、C5 模组和接口位置与原理图对应。PCB 渲染图没有内层和网络名，不能单独用于确认逐脚连通。右侧扩展接口丝印写着 `+5V`，原理图 CN1 的对应网络却是 `VSYS`，电池供电时不能按固定 5V 使用。

## 主控与存储

图纸的 P4 为 `ESP32-P4XNRW32`，位号 `U4$CBB1`。封装脚 54 接 `+1.2V`，符合 v3.x 的 VDD_HP_1 接法；芯片修订仍以实读为准。项目选择 `CONFIG_ESP32P4_SELECTS_REV_LESS_V3=n`、最低修订 v3.1、CPU 400MHz，不能烧入旧 `sila_p4` 的 v1.x 固件。

Flash 图纸料号为 `W25Q256JVEIQ`，位号 `U3$CBB1`，容量 32MiB；完整实物料号尚未从芯片丝印核对。上游软件仍写 16MB，本项目按图纸及实读容量设为 32MB，QIO 80MHz，并开启 `CONFIG_SPI_FLASH_SUPPORT_WINBOND_CHIP=y`。IDF 的 Winbond 驱动根据厂商 ID `0xEF` 匹配，支持 16MiB 以上地址；generic 驱动只支持前 16MiB。公共 `partitions.csv` 仍只使用前 16MB，其余空间没有分配用途。

PSRAM 为 32MiB，使用 HEX/X16、250MHz，开机内存检查由构建类型决定。接口和频率采用 ESP-IDF v6.1 的 v3.x 配置。片内 LDO2 给封装内 PSRAM 供电，沿用 IDF 的 1.8V 默认配置。

USB-Serial/JTAG 控制台使用 GPIO24/25。bootloader 日志设为 warning，以适配 `0x2000` 至分区表 `0x8000` 之间的空间；应用日志级别仍由 debug/release 控制。

## 屏幕与触摸

屏幕为 6.2 英寸 AXS15260D 模组，原生竖屏 452×1280。AXS15260D 同时负责显示和触摸，两部分共用一根复位线。GPIO 依据[主控接线](../../refs/boards/sila_p4x/schematic/02-p4-wiring.png)、[屏幕原理图](../../refs/boards/sila_p4x/schematic/04-display.png)及上游代码核对：

| 信号 | `sila_p4x` |
|---|---|
| MIPI DSI | 专用引脚，2 lane |
| LCD_RST、TP_RST | 共用 GPIO26，低有效 |
| LCD_BL | GPIO30，高有效 |
| TP_SDA、TP_SCL | GPIO28、GPIO29，与 AXP2101 共用 I2C1 |
| TP_INT | GPIO27 |

旧 `sila_p4` 的 LCD_RST 和 TP_INT 用 GPIO24/25，这两个脚在新板上用于 USB；旧板的 LCD_BL 是 GPIO29，在新板上是 I2C SCL。所以不能只修改芯片修订就沿用旧板程序。

### FPC 编号与排线

FPC1 的型号为 `FPC-05FB-30PH20`。按 [P4X PCB 正面图](../../refs/boards/sila_p4x/pcb/front.png)的方向观察，即元件面朝向自己、USB 口在下方，FPC1 左侧的圆点标记 1 脚，30 个信号焊盘从左到右编号为 1～30，两侧的大固定焊盘不计入。原理图中的连接器符号可以旋转或镜像，实物方向以这个 PCB 标记为准。

| 功能 | FPC1 |
|---|---|
| LEDA | 1 |
| LEDK，两路在板上相连 | 2、3 |
| VCI、IOVCC，板上均接 3.3V | 4、5 |
| LCD_RST | 6 |
| TE、PWM，底板未接 | 7、8 |
| GND | 9、12、15、18、21、24 |
| D0P、D0N | 10、11 |
| D1P、D1N | 13、14 |
| CLKP、CLKN | 16、17 |
| D2、D3，底板未接 | 19、20、22、23 |
| TP_INT | 25 |
| TP_SDA、TP_SCL | 26、27 |
| TP_RST | 28 |
| TP_VCI、TP_IOVCC，板上均接 3.3V | 29、30 |

这一页原理图的标题是 `LCD-MIPI-4IN`，资料里也没有 AXS15260D 模组排线的引脚表。上游代码在这块板子上用同一组 GPIO 驱动 AXS15260D，但没有说明排线的接法，所以模组的 LEDA、LEDK、GND 和 3.3V 是否与 FPC1 一一对应，要对照模组资料或实物排线核对。连接前应断开 USB 和电池供电，确认 pin 1 和接触面的方向；不能仅凭连接器型号相同就把排线直接插入。

FPC1 的 4、5、29、30 脚直接接主板 `+3.3V`，与主控共用 AXP2101 DCDC1 输出，没有独立的屏幕电源开关。GPIO30 只控制背光，GPIO26 只控制复位，二者都不能切断这些供电脚。

SY7200AABC 的背光电路见 `10-backlight.png`。其输入在主图接 `VSYS`，子图端口名为 `+5V`；LEDK 经 R26 采样到 GND，图纸标注 R26 为 10Ω，BL_CTR 有 R27 10kΩ 下拉。输入电压和 LED 电流尚未实测，不能因为端口名写着 5V 就认定电池供电时也有 5V。

按 [SY7200A Rev.1.0 数据手册](https://www.silergy.com/download/downloadFile?ftype=note&id=4351&type=product)的“Pinout”和“Electrical Characteristics”，EN/PWM 调光频率范围为 20kHz～1MHz，5kHz 不在规定范围内。用作调光输入时，第一个脉冲要长于 200ns 才能让芯片启动。手册没有给出占空比与 LED 电流的关系。需要 PWM 调光时，频率选用 25kHz，LEDC 分辨率设为 11 位。芯片内部的 1MHz 开关频率与外部调光频率是两个参数。SY7200AABC 为 SOT23-6 封装，手册列出的顶标器件代码为 `HY`，实物装片仍需核对。

手册给出的稳态电流设定关系为 `I_LED = 0.2V / R26`，按图纸的 10Ω 计算约为 20mA。本板实物的 R26 用万用表量得约 1Ω，按公式满占空比时约为 200mA；手册的典型特性曲线也是按 300mA 测的。PCB 预览没有印位号，R26 的位置是按电路推断的：U6 右侧竖排两颗小电阻中靠下的一颗。靠上的一颗应是 R27，阻值 10kΩ，量一下电阻就能分清两颗。6.2 英寸模组的额定 LED 电流和排线引脚定义，资料里都没有。

**这块板子配 6.2 英寸模组时，背光不能开到满占空比。** GPIO30 直接拉高、背光常亮时，模组排线一端发烫，屏幕从这一端开始出现扩散的黑色区域，断电降温后恢复。完整固件运行时把背光开到满占空比，整板会掉电。上游代码直接把 GPIO30 拉高，在 R26 约 1Ω 的板子上就是满电流。R26 应按 `0.2V / 模组额定电流` 选取；额定电流不知道时，用图纸的 10Ω，约 20mA。

背光输入经 L2 和 D3 通向 LEDA，电路没有串联的输出隔离开关，因此关闭背光不等于 LEDA 已经断电。排线方向或供电连接异常需要先在断电状态下检查，不能用关闭背光代替断电。

### 显示配置

面板按 452×1280 竖屏扫描。帧缓冲和其他板子一样是 RGB565，DSI bridge 把它转成 RGB888 发给面板；面板按每像素 24 位初始化，COLMOD 为 `0x77`。上游用 RGB888 帧缓冲，面板收到的格式与上游相同。COLMOD 设为 `0x55`、DSI 直接发 RGB565 像素包时，面板显示的颜色不对。

上游头文件记录的厂商时序文件是 `MIPI 2lane_452x1280_LV_15260D+CTC6.198_60Hz_TP=3ms_gamma2.4_20250313.txt`，HSYNC/HBP/HFP 为 10/90/90，VSYNC/VBP/VFP 为 50/10/250。上游 `main/axs15260_lcd.c` 用 48MHz 像素时钟、每 lane 1000Mbps，按这组参数计算帧率约 47.0Hz。项目把 HFP 改为 79、VFP 改为 252，总周期为 631×1592，像素时钟 60MHz，计算帧率约 59.7281Hz，接近 GB/GBA 的 59.7275Hz。60MHz 由 PLL_F240M 四分频得到，没有取整误差。每 lane 1000Mbps 与上游相同；改用 960Mbps 时，面板上的画面会逐渐变暗并留下残影，不能保持显示。MIPI PHY 使用片内 LDO3、2500mV。

厂商文件名里的 `TP=3ms` 没有资料说明含义。按厂商参数在 60Hz 下计算，垂直消隐约 3.25ms；项目的时序约 3.28ms，没有缩短这段时间。

初始化序列使用 `waveshare/esp_lcd_axs15260d` 2.0.0 的默认序列。其中从 `BB` 解锁到 `BB` 加锁的 28 条厂商命令与上游 `components/esp_lcd_axs15260` 逐字节相同。之后的 `3A`、`11`、`29` 两边都发，但延时不同，Waveshare 的序列还在 `29` 之前多发一条 `13`（Normal Display Mode On）。视频流启动以后，项目和上游一样再发一次 `29`。驱动只经 DSI 发送命令，不读面板 ID，所以屏幕没有接上时，显示初始化不会因此失败。

GPIO26 同时复位显示和触摸，只在显示初始化时拉低一次：低电平保持 11ms，释放后等待 170ms 再发初始化序列。上游注释给出的要求是低电平至少 10ms，释放后 160ms（tRT1）以后才能发命令。项目不调用 `esp_lcd_panel_reset()`，因为不接复位 GPIO 时它会发软件复位命令，上游的流程没有这一步。

UI 是横屏的 1280×452，`display_init()` 返回 `Rotation::cw270`，UI 坐标 (x, y) 显示在面板的 (y, 1279 − x)。上游 USB 副屏的触摸换算是 `win_x = 1279 - panel_y`、`win_y = panel_x`，项目的方向与它相同。屏幕朝哪个方向拿时画面是正的，还没有上板确认。

固件在 GPIO30 上用 LEDC 输出 25kHz、11 位的 PWM 调光，频率按上面 SY7200A 手册的范围选取，占空比为 0 时背光关闭。板级初始化先关闭背光，应用在画面准备好以后开启。因为上面 R26 的问题，`board::init()` 把亮度 100% 对应的占空比设为 5%，换上合适的 R26 以后再改回 100%。这时 PWM 的高电平最长约 2µs，界面亮度在 10%～100% 之间变化不明显。5% 时排线一端仍有轻微温升，热量来自背光还是 AXS15260D 没有确认。

项目锁定 `waveshare/esp_lcd_axs15260d` 2.0.0 和 `espressif/esp_lcd_touch` 1.2.1。UI 布局见 [UI 布局](../ui-layouts/index.md)，触摸控件的命中范围按 6.2 英寸换算。

### 触摸

触摸使用 I2C1、100kHz、地址 `0x3B`，频率沿用上游驱动的默认值。初始化时先探测地址应答，再读取寄存器 `0x0C` 起的 2 字节固件版本；读不到版本时只记录警告，和上游一致。触摸驱动不操作 GPIO26，因为上游注释说明，触摸驱动发出的复位脉冲会让已经初始化的面板黑屏。以后如果需要复位触摸，必须同时重新初始化显示。

上游 README 说明，控制器空闲时直接读 I2C 会读到错位的数据（原文为“指针漫游乱帧”），所以上游的触摸任务空闲时只等 INT。项目把 GPIO27 设为带上拉的输入，下降沿中断只记下有新报告。平台每次轮询时，驱动只在 INT 下降过以后，或者手指按着、距上次读取已满 20ms 时才读 I2C，20ms 是上游的跟踪周期。

每次读取 32 字节。第 0 字节是手势，第 1 字节的低 4 位是触点数、高 4 位是状态标志，之后每个触点 6 字节，依次是事件和 X 的高 4 位、X 的低 8 位、track ID 和 Y 的高 4 位、Y 的低 8 位、压力、面积。按上游头文件，事件 0、1、2 分别表示按下、抬起和持续接触。手势大于 `0x0F`、触点数大于 5，或者状态标志不是 0、`0x4`、`0x8` 时，整帧丢弃，这些判据来自上游驱动。项目只保留事件为按下或持续接触、坐标在 452×1280 以内的触点。上游只读 8 字节，只解析第一个触点；32 字节和多点的格式来自 Waveshare 的 `esp_lcd_touch_axs15260d` 1.0.0，多点触摸还没有上板确认。

手指按着时如果来了一帧空报告或者被丢弃的帧，驱动保留上一次的触点，连续两帧才算抬起，对应上游的 `TOUCH_FILTER_RELEASE_CONFIRM_CNT`。上游的按下确认（连续两帧）、6 像素以内的抖动融合和 80 像素以上跳变的两帧确认没有移植，按下在第一帧报告时生效。这样会不会出现误触，还没有上板确认。

控制器上报面板坐标，范围是 452×1280，平台再按显示方向换算成 UI 坐标。Waveshare 的触摸组件用第一个触点的字节判断所有触点的事件，从 `user_data` 取 I2C 设备，读失败时也返回成功，所以项目没有用它，驱动写在 `boards/sila_p4x/axs15260_touch.cpp`。

## IC 与供电

| 图纸位号 | 型号 | 用途 |
|---|---|---|
| `U4$CBB1` | ESP32-P4XNRW32 | 主控，封装内 32MiB PSRAM |
| `U3$CBB1` | W25Q256JVEIQ | 32MiB Flash |
| U1 | ESP32-C5-MINI-1-N4 | 无线协处理器 |
| U1_4 | AXP2101 | 主电源、充电和电池管理 |
| `U1$CBB10` | TLV62569DBVR | P4 1.2V 电源，EN/FB 接 P4 对应专用信号 |
| U6 | SY7200AABC | 屏幕背光驱动 |
| `U1$CBB4` | ES8389 | 双声道 codec |
| `U7$CBB2`、`U7$CBB6` | NS4150B | 左右两个功放 |
| U5 | SC7A20HTR | 三轴加速度计；图上“Gyro”标题不代表有陀螺仪 |
| D1、D2 | USBLC6-2SC6 | 两个 USB 接口保护 |

AXP2101 的 VBUS 接 USB 供电，BAT 接电池，VSYS 给背光和两个功放供电。DCDC1 产生主板 `+3.3V`；ALDO2、ALDO3、ALDO4 分别接摄像头 `C1V8`、`C1V5`、`C3V3`。未使用的电源输出和 TS 10kΩ 对地下拉见 `01-pmu.png`。PMU 输出默认状态、充电参数和电池供电行为尚未实测，项目不改写这些电源配置。

图纸中的 `3V2` 指示灯经 R8 5.1kΩ 接在 `+3.3V` 与 GND 之间；PCB 预览对应位置标为 `3V3`。`BAT2` 指示灯经 R10 接 VSYS，由 AXP2101 的 CHGLED 驱动，用于充电状态。两者都不由 P4 应用程序控制，BAT 灯状态不能代替主板 3.3V 供电判断。

片内 LDO1 的 VDDO_FLASH 接 Flash VCC，LDO2 接 VDD_PSRAM_0/1，LDO3 接 VDD_MIPI_DPHY，LDO4 的 VDDO_4 接 P4 封装脚 85 VDD_IO_5，为 GPIO39～48 供电。`board::init()` 把 LDO4 固定为 3.3V；LDO3 在显示初始化时请求 2.5V。P4 芯片的 HP 电源脚 26、54、76、91 接独立的 1.2V 网络。

AXP2101 使用 I2C1 地址 `0x34`，芯片 ID 寄存器 `0x03` 为 `0x4a`。IRQ 经 Q1 HL3400 从 VRTC 域转换到 3.3V，再接 GPIO6。两个 I2C 总线都有外部 1kΩ 上拉：I2C0 为 R43/R44，I2C1 为 R46/R47。项目创建 I2C1 给触摸使用，保留将来 PMU 共用同一总线的条件。

## SD、音频、传感器和摄像头

TF 卡使用 slot 0，CLK=43、CMD=44、DAT0～3=39～42。卡的 VDD 从固定 3.3V 经 Q1$CBB8 AO3401A 供电，GPIO51 的 `SD_PWRn` 低电平开启；栅极有 10kΩ 下拉，项目在初始化时也明确拉低。GPIO52 接卡检测 `SD_CD`，平台暂未使用该检测脚。CMD 和 DAT0～3 各有 51kΩ 外部上拉，CD 也有 51kΩ 上拉。卡电源开关和 LDO4 的 IO 供电是两件事，项目不启用 1.8V UHS。

ES8389 与 SC7A20、摄像头控制共用 I2C0（SDA=7、SCL=8）。音频 I2S 的 P4 输出、WS、P4 输入、BCLK、MCLK 分别是 GPIO9、10、11、12、13，GPIO31 同时控制两个 NS4150B。两只麦克风接 ES8389 的 MIC1/MIC2，左右声道分别经功放送到 U19/U2。功放差分输出不能当作对地耳机输出。

ES8389 的 DVDD、PVDD、AVDD 都接 `A3V3`，这条电源经 R52/R53 的 0Ω 电阻接主板 `+3.3V`，连接见[音频主图](../../refs/boards/sila_p4x/schematic/07-audio.png)与 [codec 原理图](../../refs/boards/sila_p4x/schematic/15-codec.png)。音频与屏幕的 3.3V 供电没有隔离。GPIO31 关闭的是两个功放，不会切断 ES8389 的供电。

原理图明确连接了 GPIO13 MCLK 和 GPIO11 DIN，但上游播放代码将 MCLK/DIN 设为 unused，并在注释中称 MCLK 未连接。后续驱动以图纸接线为依据，保留这处来源冲突，不能用播放代码推断录音线路不存在。项目的播放按图纸使用 GPIO13 的 MCLK，这一点还没有上板验证。

两个 NS4150B 由 VSYS 供电，不插 USB 时就是电池电压，所以项目按 3.3V 设置 codec 的 `pa_voltage`，让输出电平在电池电压较低时也不超出功放的供电；功放本身的增益从图纸上看不出来，没有计入。GPIO31 在板子初始化时设为输出低电平，codec 打开以后才拉高。左右声道各接一个功放和喇叭，GB 的立体声原样输出。ES8389 的 AD0 有 100kΩ 下拉，7 位 I2C 地址为 `0x10`，芯片识别寄存器 `0xfd`、`0xfe` 分别为 `0x83`、`0x89`。

SC7A20HTR 的 INT1 接 GPIO32，INT2 未接，SDO 和 CS 接 3.3V。7 位 I2C 地址为 `0x19`，`WHO_AM_I`（寄存器 `0x0f`）为 `0x11`。本项目尚未接入其驱动。

摄像头 FPC3 为 24 pin，图纸标注 OV2710，使用两条专用 CSI lane，24MHz XCLK 来自 X1。摄像头 SDA/SCL 经 BSS138PS 转换到 I2C0；1.8V 一侧有 2.2kΩ 上拉。CSI_EN 由 10kΩ 上拉和 100nF 电容保持，不是旧板 GPIO20/21 的电源、复位接法。摄像头电源来自 PMU ALDO2～4，本项目尚未初始化。

## GPIO 总表

| GPIO | 网络或连接 | 限制 |
|---|---|---|
| 0 | 主图未接 | 不能据此认定已引出 |
| 1～5 | CN1 的 GPIO1～GPIO5 | 排针同时有 GND、3.3V、VSYS |
| 6 | AXP_IRQ | 经 HL3400 转换到 3.3V |
| 7、8 | I2C_SDA0、I2C_SCL0 | codec、SC7A20、摄像头共用 |
| 9～13 | I2S_DO、WS、DI、BCK、MCK | 11 为录音输入，13 为 MCLK |
| 14～19 | C5 D0～D3、CLK、CMD | SDMMC slot 1，项目只用 1-bit 总线 |
| 20～23 | 主图未接 | 不套用旧板摄像头配置 |
| 24、25 | SERIAL_DN、SERIAL_DP | 原生 USB-Serial/JTAG |
| 26 | LCD_RST、TP_RST | 共用低有效复位 |
| 27 | TP_INT | 带上拉的输入，下降沿中断 |
| 28、29 | TP_SDA、TP_SCL | 与 AXP2101 共用 I2C1 |
| 30 | LCD_BL | 高有效 |
| 31 | PA_CTRL | 两个功放共用，初始化时关闭 |
| 32 | SC7_INT | 加速度计 INT1 |
| 33、34 | 主图未接 | GPIO34 为 strapping 脚 |
| 35 | IO35/BOOT，EN2 按键 | 下载 strapping，10kΩ 上拉 |
| 36 | 10kΩ 上拉，主图未接外设 | strapping 脚 |
| 37、38 | MCU_TXD、MCU_RXD，主图未接 | 默认 UART0；不接板上 Type-C |
| 39～42 | SD_D0～D3 | LDO4 电压域 |
| 43、44 | SD_CLK、SD_CMD | LDO4 电压域 |
| 45～50 | 主图未接 | 45～48 仍属 LDO4 电压域 |
| 51 | SD_PWRn | 低电平给卡供电 |
| 52 | SD_CD | 卡检测，暂未使用 |
| 53 | C5_BOOT，接 C5 GPIO28 | 不是旧板功放使能 |
| 54 | C5_EN | ESP-Hosted 用它复位 C5 |

C5 侧 D0～D3、CLK、CMD 分别接 GPIO8、7、14、13、9、10。上游使用 1-bit SDIO，报告其板上的 4-bit D2/D3 存在问题，这不是本项目的实测结论。项目同样只用 1-bit 总线：D0 传数据，D1 作中断线，P4 的 GPIO16、17 不用。图纸 GPIO26/27 各有 10kΩ 下拉、BOOT 有 5.1kΩ 上拉、EN 有 10kΩ 上拉和 1µF 电容。U14 引出 3.3V、GND、C5 TXD0、RXD0、EN、BOOT，不用于 P4 烧录。

## 烧录、恢复与项目边界

板上标为 UART 的 Type-C 实际通过 SERIAL_DN/DP 接 P4 GPIO24/25，是原生 USB-Serial/JTAG，没有 CH340。另一个 USB 口接 P4 专用 USB_DM/DP。两个口均有 USBLC6-2SC6 和各自的 CC 5.1kΩ 下拉，图纸中的 VBUS 共网；不要把两个口当作带电源隔离的独立输入。

本机接口枚举为 `303a:1001`、COM5，控制台选择 USB-Serial/JTAG，测试协议用同一接口。esptool 可以自动进入下载模式；需要手动进入时按住接 GPIO35 的 Boot 键上电，不能把 AXP 电源键或 U14 的 C5 BOOT 当作 P4 BOOT。原理图没有独立的 P4 RESET 按键。

恢复完整 Flash 镜像需要使用与这块板子对应的 32MiB 备份文件。

COM 号可能随重插变化。上游固件包含 C5 首刷功能，本项目没有接入这段流程，只使用 C5 上已有的固件。板级组件提供 SD 电源与 IO 供电、屏幕、触摸和 USB 控制台，音频、PMU 状态、电池、SC7A20 和摄像头尚未接入。

C5 跑 ESP-Hosted，作为蓝牙手柄的 Controller。P4 上的 ESP-Hosted 3.0.10 报告 C5 固件的主版本号和自己不同，具体版本没有读出；上游配置的注释写的是 ESP-Hosted-MCU 2.12.9。P4 上的 ESP-Hosted 要设成 C5，否则会因为芯片 ID 不符丢掉 C5 的启动事件。

## 来源

- 用户提供的原理图、PCB 预览，校验值和裁图索引见“资料”；连线以原理图为主，与上游代码对照。
- [P4X AIBox 固定提交](https://gitee.com/Ergou-/esp32-p4xc5-aibox/tree/8c6bc38bc7a082d9c4475e08c40778442a8dd8be)，板级配置和初始化流程。
- [ESP32-P4 数据手册](https://documentation.espressif.com/esp32-p4_datasheet_en.html)，管脚复用、strapping 和电源域。
- ESP-IDF v6.1 的 `components/esp_psram/esp32p4/Kconfig.spiram`、`components/esp_hw_support/port/esp32p4/Kconfig.hw_support`、`components/esp_system/port/soc/esp32p4/Kconfig.cpu` 与 `components/spi_flash/spi_flash_chip_winbond.c`。
- [esp_lcd_axs15260d 2.0.0](https://components.espressif.com/components/waveshare/esp_lcd_axs15260d/versions/2.0.0)，本项目使用的显示驱动和初始化序列；[esp_lcd_touch_axs15260d 1.0.0](https://components.espressif.com/components/waveshare/esp_lcd_touch_axs15260d/versions/1.0.0)，多点触摸报告的读取长度。
- ESP-IDF v6.1 的 `components/esp_lcd/dsi/esp_lcd_panel_dpi.c` 与 `components/esp_hal_lcd/mipi_dsi_hal.c`，DPI 分频、DSI PHY PLL 和主机水平时序的取整。

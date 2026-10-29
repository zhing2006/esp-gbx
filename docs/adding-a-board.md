# 加板指南

给项目添加一块板子，需要准备板级代码、硬件文档和原始资料，并把板子接入构建。本文按现有的 [JC1060P470](boards/jc1060p470.md) 和 [M5Stack Tab5](boards/m5stack_tab5.md) 的做法说明流程，模块边界见[架构文档](architecture.md)。

下文的 `example_board` 是占位名，使用时换成板子型号的小写。除了链接，文中的文件路径都相对于仓库根目录。

## 1. 确认手上的板子

先确认具体型号、PCB 版本、芯片修订和屏幕版本，再找对应资料。同一个商品名可能换过 PCB、屏幕或触摸 IC，不能只凭商品名套用现有配置。

需要核对的内容包括：

- 板子的完整型号、PCB 丝印和版本，以及屏幕模组的标记。
- 主控型号和修订。用 esptool 读取手上芯片的信息，并记录工具版本；原理图可以用来核对 PCB 的设计，但不能代替芯片实测。
- flash 和 PSRAM 的容量、接口和工作频率。
- 哪个接口用于烧录和日志，接的是 USB 转串口、USB-Serial/JTAG 还是 USB OTG，以及进入下载模式的方法。
- 供电方式、IO 电压、外设电源和复位的控制方式。

第一次烧录前，确认恢复方法并保存适用的出厂镜像或本地备份。文档记录恢复入口和适用版本，镜像校验及备份细节随本地文件保存。不要混用不同硬件的镜像，也不要默认改写 C6 等协处理器。

现有板子的版本差异可以作为核对资料的例子：JC1060P470 文档区分了不同 PCB 和新旧屏；Tab5 文档区分了 ILI9881C、ST7123 和 ST7121 屏幕。具体识别依据见各自的“型号和版本”一节。

## 2. 收集原始资料

整理后的板子资料放在 `refs/boards/example_board/`，随仓库提交。原始下载或解压目录只是整理时的输入，不会长期保留。后续开发需要的引脚定义、上电和复位流程、初始化序列、时序等内容都要直接保存在仓库里，不能只写一个指向原目录的文件清单或摘要。原始 PDF、整个资料包、示例工程和固件镜像不入库，板子资料目录也不另放独立的 LICENSE 文件。

整理时保留以下内容：

- 固定版本的来源、原文件路径，以及必要的版本或校验标识。
- 按电路区域裁剪的清晰原理图，保留完整连接、网络名和元件标号，去掉无关边框与空白。
- 开发所需的引脚表、电气要求、时序、厂商配置和初始化源码。参考代码用 `.c.txt`、`.h.txt` 等后缀，保留依赖的宏和版权许可声明。

整理结果应当脱离原下载目录仍可查阅，不能只留下页码或“去原文件查看”的提示。

在 `refs/README.md` 的“板子”表格里增加一行，列出目录、内容和来源，并指向 `docs/boards/example_board.md`。表格里的 `boards/example_board/` 是相对于 `refs/` 的路径。

**板子资料不经过 `refs/fetch.sh` 和 `refs/fetch.ps1`。** `.gitignore` 已经允许提交 `refs/boards/`，通常不用修改。如果另外添加了需要下载、但不入库的参考资料，才需要在两个 fetch 脚本中同时固定版本，并登记到 `refs/README.md` 对应的分类。

## 3. 整理硬件文档

创建 `docs/boards/example_board.md`。可以参照现有板子文档的章节组织，不需要照抄另一块板子的所有外设。文档应当让后续写驱动的人能找到引脚、供电和初始化的依据。

| 内容 | 需要写清楚的事项 |
|---|---|
| 适用范围 | 完整型号、PCB 和屏幕版本、手上板子的识别结果 |
| 资料 | 本地文件、文件内容、上游原路径、固定版本或校验值 |
| 主控与存储 | 芯片修订、flash、PSRAM、配置选择及其依据 |
| IC | 型号、位号、用途；不明确的型号直接注明 |
| 外设 | 接口、引脚、I2C 的 7 位地址、中断、复位、使能和初始化要求 |
| GPIO 总表 | 原理图网络名、实际连接、可用扩展引脚、strapping 和复用限制 |
| 电源 | 电源来源、外设电压、LDO 接法、上电依赖和默认状态 |
| 烧录与恢复 | 接口、下载模式、日志通道、出厂镜像和恢复方法 |
| 待确认的硬件约束 | 缺少的资料、型号与连接歧义，以及影响初始化的未知条件 |
| 来源 | 厂商文档、数据手册、BSP 和其他引用的具体版本或章节 |

引脚以对应 PCB 的原理图为依据，并与厂商 BSP 对照。有冲突时保留来源和未确定之处。尤其要确认 IO 电源域、复位电平和协处理器依赖，不能只复制 GPIO 表。

屏幕需要记录分辨率、像素格式、lane 数和速率、像素时钟、前后肩、同步宽度、初始化序列来源和显示方向。触摸要记录地址、复位方式、坐标范围，以及是否需要交换、翻转或缩放坐标。显示方向和触摸坐标都要上板确认，不能只抄配置。

板子文档注明厂商标称值和按参数算出的值。比如用时序算出的刷新率不能写成实测刷新率，I2C 有应答也不能写成外设功能通过。缺少资料或连接尚未确认时，在相应硬件说明中写清楚。

## 4. 添加板级组件

最小目录与现有板子一致：

```text
boards/example_board/
├── CMakeLists.txt
├── board.cpp
├── idf_component.yml
└── sdkconfig.defaults
```

### CMakeLists.txt

参考现有板子的 `CMakeLists.txt`，通过 `board` 组件提供接口，并只声明实际使用的硬件依赖。

`PRIV_REQUIRES` 按板子实际用到的组件写。板子启用了 PSRAM 时一定要列出 `esp_psram`，否则测试固件的依赖可能没有包含这个组件，生成配置时 `CONFIG_SPIRAM` 会被当作未知选项忽略，虽然构建仍然能成功。

根目录的 `CMakeLists.txt` 会根据 `GBX_BOARD` 加入 `boards/<板子>/`。`main/` 和 `tests/device/` 通过同一个构建属性依赖所选板子，所以按此结构加板时，不需要给这两个入口增加分支，也不需要在根 CMake 中维护一份板子名单。

### board.cpp

板级接口声明在 `components/board/include/gbx/esp/board.h`，正式固件使用名称、初始化、显示和触摸接口，测试固件使用 `name()`、`init()` 和 `sdmmc()`。各函数的职责写在头文件的注释里。可以参照 `boards/sila_p4/board.cpp`：它的屏幕和触摸都直接接在 GPIO 上，不经过 IO 扩展器，是现有板子里最简单的。MIPI DSI 的公共步骤用 `components/board/include/gbx/esp/board_support.h` 里的函数，它们只在有 MIPI DSI 的芯片上编译。RGB 并口屏由板子自己创建 RGB 面板，`display_init()` 返回 `PanelDriver::rgb`，可以参照 `boards/wks_s31_cb/board.cpp`。SPI 屏由板子创建 esp_lcd 的 SPI IO 和面板，`display_init()` 返回 `PanelDriver::spi` 和这个 IO，平台从 IO 的传输完成事件知道什么时候可以再改帧缓冲。平台按低字节在前的 RGB565 发送像素，面板要设成同样的字节序，可以参照 `boards/waveshare_c5_lcd28/board.cpp`。面板不旋转时，平台在片内 RAM 里另外分配两段缓冲，游戏画面从这里发送，不经过帧缓冲；分配不到时退回帧缓冲。分段发送直接发 CASET、RASET、RAMWR 和 RAMWRC，所以面板控制器要支持 MIPI DCS 的 write_memory_continue（3Ch），板子也不能用 `esp_lcd_panel_set_gap()` 给面板设偏移。

如果 UI 的方向和面板的扫描方向不同，在 `display_init()` 返回的旋转方向里写明，由 `components/platform/src/esp/` 旋转画面并换算触摸坐标。触摸坐标范围和面板分辨率不同时，`touch_init()` 返回控制器的坐标范围，平台按比例换算。这两项都要上板确认。屏幕的背光只要有电就亮、板子控制不了时，`display_init()` 返回的 `backlight` 设为 false，设置里的 BRIGHTNESS 就不可用。

`name()` 返回日志里显示的板子名。正式固件和测试固件都会在启动时调用一次 `init()`，它只打开电源和共用的总线，初始化失败时返回 `false`，调用方不能继续访问依赖它们的外设。还没有驱动的外设在文档里说明现状，代码里用 TODO 标出，不要让占位代码看起来像已经完成了初始化。

`sdmmc()` 提供 SDMMC slot、CLK、CMD 和四根数据线。如果 IO 电压域由片内 LDO 供电，由 `board::init()` 打开，不依赖 SD 挂载；平台以固定 3.3V 挂载，不启用 UHS。需要确认 LDO 给卡本身还是只给 IO 供电，以及同一电压域还有哪些引脚。还要核对 CMD 和 DAT 线上的外部上拉，不能以内部上拉或一次读卡成功代替硬件确认。

芯片没有 SDMMC 控制器、TF 卡和屏共用 SPI 总线时，`sdmmc()` 返回 `Sdmmc::Bus::spi`、SPI host 和卡的片选，由 `init()` 初始化总线。卡上电以后处于 SD 模式，会把总线上发给屏的数据当成命令，所以 `init()` 要先发 CMD0 让卡进入 SPI 模式，再让屏使用总线，做法见 `boards/waveshare_c5_lcd28/board.cpp`。

板子的引脚、外设型号、供电和复位要求放在板级实现中；芯片共用的能力放在 `components/platform/src/esp/`，可复用的外设驱动放在 `drivers/`。`drivers/` 目前还没有建立，新增组件时还需要让根 CMake 能发现它，并声明实际依赖。不要在 `components/` 下的 `app/`、`ui/`、`core/` 或 `jit/` 里加入板子型号判断或 ESP-IDF 头文件。

### idf_component.yml

屏幕和触摸的驱动优先用乐鑫组件仓库里现成的组件。在板子目录的 `idf_component.yml` 中声明，版本用 `==` 固定，参照现有板子。有的组件的配置宏不能在 gnu++23 下编译，这时按宏的内容逐个字段填写结构体。

### sdkconfig.defaults

这个文件只放板子相关配置，例如 target、芯片修订、CPU 主频、flash、PSRAM 和控制台通道。可以从最接近的板子开始，但必须逐项核对硬件。

P4 v1.x 与 v3.x 必须使用对应的芯片修订配置，主频和存储频率以硬件能力为准。控制台需匹配实际连接的 UART 或 USB-Serial/JTAG。不要把一块板子的配置当作 P4 或 S31 的通用模板。片内 RAM 少的芯片，比如 C5，要在这里用 `GBX_GB_JIT_POOL_KB` 调小 GB JIT 的代码池，否则测试固件的 JIT 后端检查会因为片内放不下代码池而失败。

默认配置按以下顺序合并，后面的覆盖前面的：

1. 根目录 `sdkconfig.defaults`：所有板子共用的配置。
2. 根目录 `sdkconfig.defaults.debug` 或 `.release`：优化、断言、日志和 PSRAM 自检等构建类型配置。性能分析版的 preset 接着加载 `sdkconfig.defaults.profile`。
3. `boards/example_board/sdkconfig.defaults`：板子配置。

临时配置可以用 `idf.py --preset example_board-debug menuconfig` 修改，也要通过 EIM 运行。需要保留的选项写回对应的 defaults 文件，不要提交生成的 `sdkconfig`。已有 build 目录中的配置可能保留旧值，所以修改 defaults 后，还要用干净的 build 目录检查最终生成的配置。

## 5. 添加四个 preset

在 `CMakePresets.json` 中为正式固件和测试固件各添加 debug、release 两个 preset，参考现有板子。

`GBX_BOARD` 必须与目录名一致，`GBX_BUILD_TYPE` 为 `debug` 或 `release`，`GBX_APP` 为 `main` 或 `device`。每个 preset 使用独立构建目录。ESP-IDF v6.1 的 `idf.py` 不支持 preset 继承，四个配置需完整填写。

## 6. 编译和检查启动

固件使用 EIM 安装的 ESP-IDF v6.1，命令从仓库根目录执行。不要为了编译把 IDF 环境全局激活，以免替换日常使用的 Python 和 CMake。

macOS、zsh：

```sh
eim run 'idf.py --preset example_board-debug build' v6.1
eim run 'idf.py --preset example_board-release build' v6.1
eim run 'idf.py --preset example_board-test-debug build' v6.1
eim run 'idf.py --preset example_board-test-release build' v6.1
```

Windows、PowerShell 使用相同的四个 preset，但每条传给 EIM 的命令末尾要加 `exit $LASTEXITCODE`，否则构建失败时 EIM 仍可能返回 0。例如：

```powershell
eim run 'idf.py --preset example_board-debug build; exit $LASTEXITCODE' v6.1
```

检查生成的 sdkconfig 和构建日志，确认 target、芯片修订、主频、flash、PSRAM 和控制台都与预期一致。如果此前用 `menuconfig` 改过配置，保留需要的修改后，把对应的 build 目录移走，再从 defaults 构建一次，避免本地旧配置掩盖遗漏。

ESP-IDF v6.1 把 S31 作为 preview target。每个 preset 的构建目录第一次构建以前，要先用 `idf.py --preset example_board-debug --preview set-target esp32s31` 选择 target，同样通过 EIM 执行；target 设置好以后，普通 build 不需要 `--preview`。现有的 S31 板子是[慧勤智远 ESP32-S31 CB](boards/wks_s31_cb.md)和[乐鑫 ESP32-S31-Korvo-1](boards/esp32_s31_korvo_1.md)，它们的配置只能参考，不能当作 S31 的通用模板。

接好文档里确认的烧录接口，以 debug 固件检查启动。把 `PORT` 换成实际端口；下面两组命令按操作系统选用。

macOS、zsh：

```sh
eim run 'idf.py --preset example_board-debug -p PORT -b 460800 flash monitor' v6.1
eim run 'idf.py --preset example_board-test-debug -p PORT -b 460800 flash' v6.1
eim run 'python tests/runner/transport.py PORT' v6.1
```

Windows、PowerShell：

```powershell
eim run 'idf.py --preset example_board-debug -p PORT -b 460800 flash monitor; exit $LASTEXITCODE' v6.1
eim run 'idf.py --preset example_board-test-debug -p PORT -b 460800 flash; exit $LASTEXITCODE' v6.1
eim run 'python tests/runner/transport.py PORT; exit $LASTEXITCODE' v6.1
```

先确认正式固件的板子、芯片和内存信息，再退出 monitor，烧录测试固件并运行连接诊断。诊断需要独占串口；握手和 PING/PONG 的含义见[共用二进制传输](hardware-testing.md#共用的二进制传输)。release 默认只输出 warning 和 error，不能以缺少 info 日志判断启动失败。

编译成功和上述日志只能证明最小组件能够构建、启动，不能证明屏幕、音频或模拟器测试通过。没有板子时，可以先记录构建结果，但启动和外设验证必须标为“未实测”。

## 7. 按外设验证硬件

硬件验证可以早于正式驱动接入。板子文档只更新由此确定的型号、连接、初始化约束和项目配置。

按供电依赖逐项检查，不要一次打开所有外设。对板子实际具备、项目会用到的功能，可以参照以下检查内容：

| 功能 | 检查内容 |
|---|---|
| 电源、复位、IO 扩展器 | 初始电平、使能顺序、冷启动与软件复位后的状态，避免继承出厂固件留下的配置 |
| I2C | 地址、器件 ID、复位或供电变化前后的应答；应答只证明总线上有设备 |
| 屏幕 | 显示带方向标记的测试图，确认方向、边框和颜色正常；记录时序 |
| 触摸 | 点按屏幕四角附近和中心的目标，检查读出的坐标是否与目标位置一致 |
| 音频 | 实际播放、左右声道、喇叭混音、功放使能、耳机检测；录音单独验证 |
| TF 卡 | 供电、总线宽度、挂载、文件读写，记录卡和测试条件，避免覆盖已有数据 |
| 按键 | 每个输入的有效电平、复位时的状态，以及是否占用 strapping 或其他外设引脚 |
| RTC | 时间递增、日期有效性、掉电标志，以及有后备电源时断电后的保持情况 |
| 电池与充电 | 装电池和未装电池时的区别、采样换算和充电控制，不能把无电池时的读数当成电池电压 |

显示方向和触摸坐标必须由持板者在固定方向下确认，覆盖屏幕边缘和中心。

这些检查是板子硬件验证。模拟核心的正确性测试另按[架构文档的“测试”一节](architecture.md#测试)执行：在板子上分别运行解释器和 JIT，以测试套件自己的判据判断，两者都要通过且结果一致。当前支持范围见[真机测试计划](hardware-testing.md)。

## 8. 添加 UI 素材

游戏页的控件和 VFD 字体各板共用，每块板子需要自己的底图和布局。步骤见 [UI 布局的“添加板子”一节](ui-layouts/index.md#添加板子)。正式固件构建时要打包这块板子的 UI 素材，所以没有素材时只能构建测试固件。打包脚本 `tools/ui/pack_assets.py` 的 `PANEL_INCHES` 里还要加上屏幕的对角线尺寸，用来换算按键命中区域的毫米数。

## 提交前检查

检查板级组件、四个 preset、UI 素材、硬件文档和 `refs/README.md` 是否齐全，并同步架构中的板子列表。

- [ ] 型号、硬件版本、资料来源及未确定之处明确。
- [ ] 引脚、供电、复位和初始化依据完整可查。
- [ ] 四个 preset 的配置正确，构建结果已经确认。
- [ ] 启动、显示方向和触摸完成板上验证，未测功能如实标注。
- [ ] 没有提交构建产物、原始资料包、本地备份或游戏 ROM。

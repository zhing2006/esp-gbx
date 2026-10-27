# 任务 1-2：自写测试 ROM 和参考帧

这是[路线图](../../roadmap.md#gba) GBA 段里程碑 1 的第二个任务：补上第三方套件没有覆盖的测试，并给需要比对画面的 ROM 生成参考帧，加进 1-1 的测试数据。做完以后，卡上的 GBA 测试数据不再改动。这个任务依赖 1-1，也不写模拟器代码。

开工时在[任务状态](status.md)里把这个任务标为“进行中”。

## 先读

- 路线图 GBA 段开头的几段和里程碑 2、5 的完成标准，[产品设计](../../product-design.md)的“正确性验证”“GBA 存档类型识别”和“ROM 加载”。
- AGENTS.md 的 Tests 一节，尤其是只能和套件自带的参考图比较画面、不能把模拟器自己的输出录成标准答案这两条。
- `refs/docs/gbatek.htm` 中 “GBA LCD Video Controller” 下的各节、“GBA Cartridge ROM”“GBA Cart Backup EEPROM”“GBA Cart I/O Port (GPIO)” 和 “GBA Cart Real-Time Clock (RTC)”。
- 1-1 的生成脚本和“用例选择”一节，以及 jsmolka 的 `lib/macros.inc`：`m_test_eval` 显示和报告结果的约定。

## 范围

参考模拟器：

- 在 NanoBoyAdvance 和 mGBA 中选一个。先用 1-1 名单中的 nba-hw-test 和 jsmolka 用例比较两者，记下各自通过和不通过的用例。另外的条件是：能在没有界面的情况下，按确定的帧数取出 240×160 的原始 BGR555 帧；能关闭色彩校正和帧间混合；能使用用户的同一份 BIOS 转储并跳过开机流程；同样的输入运行两次，得到的帧相同。比较的结果和选择的理由交给用户确认。
- 参考模拟器使用固定的发布版本或固定的提交，只在 PC 上运行，用来生成参考帧和检查自写的 ROM。它的源码不放进仓库，也不由 fetch 脚本下载，写模拟器时不阅读它的实现。版本、获取方式和运行参数写进生成参考帧的脚本和 `tools/README.md`。取帧优先用它自带的命令行或脚本接口；需要修改它，或者写程序链接它的代码时，先和用户商量。

自写的测试 ROM：

- 源码放进仓库，用固定版本的 Arm GNU Toolchain（`arm-none-eabi`）构建，macOS 和 Windows 上都能构建。目录由你决定，写进架构文档的“目录”一节。生成的 ROM 不提交，由生成脚本构建以后放进卡上的数据。ROM 头里的 Nintendo logo 是任天堂的数据，不放进仓库；我们的模拟器和参考模拟器都跳过 BIOS 的开机流程，不检查 logo。
- PPU 测试 ROM 按主题分成多个小 ROM，每个 ROM 画出一幅稳定不变的画面，靠参考帧判定。覆盖模式 0 至 5 的背景（16 色和 256 色图块、各种尺寸、滚动、优先级）、仿射背景（参考点、溢出和回绕）、位图模式和帧选择、OBJ（普通和仿射、double size、1D 和 2D 映射、和 BG 的优先级、半透明、OBJ 窗口、每行的 OBJ 绘制周期上限）、窗口、alpha 混合和亮度调节、mosaic、强制空白，以及用 HBlank 中断和 HBlank DMA 在一行中途修改滚动、仿射参数、混合和窗口寄存器。每个 ROM 的源码注明它对应 GBATEK 的哪些小节。
- 自己判定的测试 ROM 沿用 jsmolka 的约定，在 idle 循环用 `r12` 报告结果，0 表示通过，否则是第一个失败的测试编号，这样 2-1 的判据可以直接使用：
  - EEPROM：512B 和 8KiB 两种容量，检查 DMA 位流的读和写、地址位数、写入以后忙碌期间和就绪以后读到的状态位。每种容量从同一份源码构建带 `EEPROM_V` 和不带 ID 字符串的两个版本，分别检查按字符串判断和按第一次访问判断。
  - 没有 ID 字符串的 SRAM 和 64KiB Flash：jsmolka 的 `sram`、`flash64` 和 `flash128` 都带 ID 字符串，`none` 只检查没有存档芯片的情况，所以另写两个不带字符串的 ROM，第一次访问分别按字节写 SRAM、发 Flash 命令序列，再检查读回的数据和 Flash 的 ID、擦除和写入。
  - RTC：GPIO 的方向、数据和读使能，S-3511 的复位、状态寄存器、日期时间和时间的读写、12 和 24 小时制、BCD 进位，以及用定时器量出整秒以后时间的推进。模拟器按游戏代码判断卡带带不带 RTC，所以这个 ROM 的头里用一个不会和商业游戏冲突的游戏代码，5-2 把它登记进带 RTC 的游戏记录。
  - 32MiB ROM 的读取：ROM 填满按地址算出的数据，覆盖 16MiB 以上的地址。拆成两个 ROM，因为 DMA 在 2-2 才实现：一个用 ARM 和 Thumb 的字节、半字和字读取，并从高地址执行代码，2-1 用它验收；另一个用 DMA 从 ROM 读取，2-2 用它验收。
- 期望值按 GBATEK 计算，每项检查注明依据。自写的 ROM 在参考模拟器上必须全部通过；参考模拟器和 GBATEK 不一致时，先查清楚原因，和用户确认以后再定期望值，不直接照着任何一边改。

参考帧：

- 生成参考帧的 ROM 包括：自写的 PPU 测试 ROM，jsmolka 的 `ppu`，alyosha 的 `ppu/stripes_x_offset`、`ppu/Sprite_Disable_Midline` 和 `ppu/Sprite_Disable_on_VRAM_Access`，以及 nba-hw-test `ppu` 下附 `expected.png` 的 5 个测试。alyosha 的两个 `Sprite_Disable_*` 先对照源码注释说明的现象，确认参考帧显示的是测试期望的结果，再交给用户确认。
- 每个 ROM 从开机算起运行规定的帧数以后取帧，并且确认画面已经稳定：之后的若干帧都和这一帧相同。帧数和稳定的帧数写进生成参考帧的脚本，2-1 和 2-3 的 runner 用同样的帧数取帧。
- nba-hw-test 的 5 个测试，先把参考帧和附带的照片对照目测，结论交给用户确认以后，参考帧才能用于比对。
- 参考帧和自写的 ROM 由生成脚本加进 `tests/gba/`，清单记录参考模拟器的版本和生成参考帧时仓库的提交。

规则：

- 修改 AGENTS.md 的 Tests 一节：GBA 的画面还可以和固定版本的参考模拟器生成的参考帧比较，条件按本任务的做法写；其他系统仍然只和套件自带的参考图比较。同步修改产品设计的“正确性验证”，写明选定的参考模拟器，以及真机测试计划中参考帧的来源和判据。

## 不做

- 模拟器代码、测试固件和 runner 的执行。
- 只能目测的 jsmolka `unsafe`，以及没有预期结果的 nba-hw-test `ram-access-timing`。

## 约束

- 测试只检查能发现实际功能错误的行为，不写防御性的测试。
- 参考帧只用来比较，以后比较不一致时，先查 GBATEK 和硬件测试确认哪一边正确，不直接照着参考模拟器改。
- 生成的 ROM 和第三方套件一样留在本地，不提交，也不重新分发。参考帧提交在 `tests/gba/frames/`。
- 1-1 定下的用例包格式如果需要修改，同步修改 1-1 的脚本和说明。做完这个任务以后不再修改。

## 由你决定

PPU 测试 ROM 的拆分和画面内容、自写 ROM 的源码目录和构建方式、参考帧的存放格式，以及生成参考帧的脚本。选择的理由写在脚本和源码的注释里。

## 完成标准

- 参考模拟器已经选定，用户已经确认，版本和运行参数写进 `tools/README.md`。
- 自写的 ROM 在 macOS 上从源码构建成功，Windows 的构建步骤写明；Windows 上没有验证时如实说明。
- 自己判定的 ROM 在参考模拟器上全部通过，和 GBATEK 不一致的地方已经和用户确认。
- 上面列出的 ROM 都有参考帧，nba-hw-test 的 5 个参考帧已经和照片对照，用户已经确认。
- 测试数据重新生成，复制到卡上，读回以后清单校验一致。从这时起，卡上的 GBA 测试数据不再改动。
- AGENTS.md、产品设计、真机测试计划和 `tools/README.md` 已经更新。
- 这个任务不改固件，不需要上板运行，也不需要构建固件。

## 收尾

- 满足全部完成标准以后，在[任务状态](status.md)里把这个任务标为“已完成”。
- 向用户汇报参考模拟器的选择和理由、自写 ROM 的清单和覆盖范围，以及和 GBATEK 不一致的地方。

## 核对结果

### 参考模拟器

比较的是 NanoBoyAdvance v1.8.3（tag `v1.8.3`，提交 `8c3b159c`）、mGBA 0.10.5（tag `0.10.5`，提交 `26b7884b`）和 mGBA 的 master（提交 `fd9dc5c2`）。三者都在 macOS 上从源码构建，用链接核心库的小程序运行。这个程序和 NanoBoyAdvance、mGBA 0.10.5 的源码放在本地的 `refs/emulators/` 下，被 git 忽略。用例是 1-1 名单中能自动判定的 129 个，按 1-1 的判据判定。比较时运行正版 BIOS 的开机流程，300 帧以后再按 1-1 的上限计时，因为标“开机相位”的 ROM 在跳过开机时，结果取决于各模拟器自己设置的开机状态。

| 套件 | NanoBoyAdvance v1.8.3 | mGBA 0.10.5 | mGBA master |
|---|---|---|---|
| jsmolka | 9 / 9 | 4 / 9 | 5 / 9 |
| alyosha | 59 / 105 | 18 / 105 | 19 / 105 |
| nba-hw-test | 12 / 15 | 1 / 15 | 2 / 15 |
| 合计 | 80 / 129 | 23 / 129 | 26 / 129 |

NanoBoyAdvance 没通过的 nba-hw-test 是 `128kb-boundary`（18 项通过 9 项，失败的都是递减源地址的 DMA）、`burst-into-tears`（3 项都失败）和 `haltcnt` 的 `HALTCNT CPUSET DMA`（4151，期望 4154）。alyosha 没通过的 46 个分布在 Bus 3 个、DMA 2 个、Interactions 9 个、Serial 8 个、fifo_dma 6 个、irq 3 个、prefetcher 7 个、timer 2 个、timing 2 个、ppu 3 个（`Sprite_Last_VRAM_Access_Free`、`start_up_vbl_irq`、`start_up_vbl_irq_halt`）和 `arm/Coprocessor_14`。mGBA 0.10.5 在 jsmolka 上失败的是 `arm` 第 235 项（写 PC 的错误 CMP 不应该清空流水线）、`thumb` 第 102 项（ADD 的溢出标志）和 `sram`、`flash64`、`flash128` 第 6 项（存档区的半字写入），nba-hw-test 只通过了 `ppu/vram-mirror`。

NanoBoyAdvance 也满足其余条件：核心库按 V-Blank 交出 240×160 的帧；色彩校正和帧间混合在前端的着色器里，核心库交出的是原始颜色；可以用同一份 BIOS 转储并跳过开机流程；32 个参考帧的 ROM 各运行两次，90 帧完全相同。用户确认选 NanoBoyAdvance，mGBA 0.10.5 只用来对照。

### 自写的测试 ROM

源码在 `tests/gba/roms/`，`tools/gba_test_roms.py` 用 Arm GNU Toolchain 15.3.Rel2 构建，同样的源码和工具链得到的 ROM 字节相同。macOS（Apple silicon）上构建过，Windows 的下载文件名写在脚本里，Windows 上没有验证。ROM 头里没有 Nintendo logo，游戏代码以数字开头。

PPU 有 22 个场景，各画一幅不变的画面，靠参考帧判定，2-3 验收：

| ROM | 内容 |
|---|---|
| `ppu/bg_text_4bpp`、`bg_text_8bpp` | 模式 0 的文本背景：4 种画面尺寸、跨屏幕块的滚动和回绕、翻转、调色板、背景之间的优先级，16 色和 256 色图块 |
| `ppu/bg_affine_mode1`、`bg_affine_mode2` | 模式 1 和 2 的仿射背景：旋转缩放、参考点、溢出回绕和不回绕 |
| `ppu/bitmap_mode3`、`bitmap_mode4`、`bitmap_mode5` | 位图模式、帧选择，以及经仿射参数缩放的位图 |
| `ppu/obj_1d`、`obj_2d` | 12 种形状和尺寸的 OBJ，1D 和 2D 映射，翻转，16 色和 256 色 |
| `ppu/obj_affine` | 仿射 OBJ 和 double size，共用参数组 |
| `ppu/obj_priority` | OBJ 和背景之间的优先级，OBJ 之间按 OAM 序号 |
| `ppu/obj_cycles`、`obj_cycles_hblank_free` | 每行的 OBJ 绘制周期上限，1210 和 954 两种 |
| `ppu/window`、`window_garbage` | 窗口 0 和 1、OBJ 窗口、窗口外，以及 X1>X2、Y1>Y2 和 X2>240 |
| `ppu/blend_alpha`、`blend_brightness_up`、`blend_brightness_down` | alpha 混合、亮度增减、半透明 OBJ、系数饱和 |
| `ppu/mosaic` | 文本和仿射背景的 mosaic，普通和仿射 OBJ 的 mosaic |
| `ppu/forced_blank` | 逐行打开强制空白 |
| `ppu/raster_dma`、`raster_irq` | 分别用 HBlank DMA 和 HBlank 中断逐行修改 BG0HOFS、BG2 的仿射参数和参考点、BLDALPHA 和 WIN0H |

寄存器只在 HBlank 里写，从下一行起生效，这是和用户商定的。每个场景的源码注明它依据的 GBATEK 小节。`ppu/model.py` 只按 GBATEK 算出每个场景的画面，场景用到 GBATEK 没写清楚的设置时，它拒绝生成。22 个场景的参考帧和模型的结果逐像素相同，mGBA 0.10.5 的画面按每个分量的高 5 位比较也相同。

自己判定的 ROM 沿用 jsmolka 的约定：运行到 idle 循环（跳转到自身的指令）时，`r12` 为 0 表示通过，否则是第一个失败的检查编号。每项检查的依据写在源码里。

| ROM | 检查 | 验收 | 模拟时间上限 |
|---|---|---|---|
| `save/eeprom512_id`、`eeprom512_noid`、`eeprom8k_id`、`eeprom8k_noid` | 第一次访问是读请求；写入以后先忙碌再就绪，读回；第一块和最后一块、覆盖写、每个地址位 | 2-4 | 10 秒 |
| `save/sram_noid` | 第一次访问是字节写；32KiB 写两遍并读回 | 2-4 | 10 秒 |
| `save/flash64_noid` | 64KiB 器件的 ID；整片擦除、首尾扇区写入、没有命令的写入无效、扇区擦除（Atmel 是重写扇区） | 2-4 | 10 秒 |
| `rtc/rtc` | GPIO 的读使能和方向；强制复位后的状态；24 小时制；日期时间和时间的读写；BCD 进位、年末、闰年和平年的 2 月、小月月末；12 小时制的上下午；用定时器量出一秒 | 5-2 | 30 秒 |
| `rom/rom32_cpu` | 32MiB ROM 在三个镜像区的 ARM 和 Thumb 字节、半字、字读取，以及从 16MiB 以上执行 ARM 和 Thumb 代码 | 2-1 | 10 秒 |
| `rom/rom32_dma` | DMA1 至 DMA3 在三个镜像区跨过 16MiB 的 16 位和 32 位读取 | 2-2 | 10 秒 |

两个 EEPROM 的 `noid` 版本和 SRAM、Flash 要按第一次访问判断存档类型，其余按 ID 字符串。RTC ROM 的游戏代码是 `0RTC`，5-2 登记进带 RTC 的游戏记录。上限按 GBATEK 给的最长耗时估算：EEPROM 每次写入最多 10 毫秒，Flash 的整片擦除最多 2 秒；RTC 正常通过时要等 9 次秒数变化，不到 10 秒；每次等待在 3 秒后超时，30 秒的上限让它来得及自己报告失败的编号。

在参考模拟器上，存档和 32MiB ROM 的 8 个 ROM 都通过：NanoBoyAdvance 在第 2 至 29 帧到达 idle 循环，mGBA 0.10.5 在第 2 至 29 帧。取帧程序里的 NanoBoyAdvance 只按 ID 字符串判断存档类型，不带 ID 的 ROM 要指定类型；mGBA 能按第一次访问判断。RTC ROM 在两边都不通过，见下一节。

### 和 GBATEK 不一致或 GBATEK 没写清楚的地方

- 混合和亮度的取整：GBATEK 只给了公式，没写怎么取整，也没有找到硬件测量。NanoBoyAdvance 接近四舍五入，但是绿色分量的进位阈值和红、蓝不同；mGBA 在 8 位色深上计算。用户确认场景只用不需要取整的颜色和系数，取整方式不测，记为未知。
- 窗口的 X1>X2、Y1>Y2：GBATEK 说按 X2=240、Y2=160 处理，Tonc 13.3.1 节和 gbadoc 的 “Windowing” 说会回绕，NanoBoyAdvance 和 mGBA 也都回绕。用户确认按回绕定期望值。
- RTC：期望值按 GBATEK，用户确认不要求参考模拟器通过。NanoBoyAdvance 在第 2 项失败，读回的 GPIO 方向寄存器和写入的相反（写 5 读出 0xA），输出脚读出 0；跳过第 2、3 项以后它在第 5 项失败，因为它读主机时钟，强制复位和写入都不起作用。mGBA 0.10.5 在第 5 项失败，它同样忽略强制复位和写入，读出的是取帧程序设定的固定起始时间（`RTC_FAKE_EPOCH`）。
- 场景避开了下面这些 GBATEK 没写清楚、两个模拟器又可能不同的情况，所以它们都没有测：2D 映射时跨过 32 图块一行右端的 OBJ 图块；绘制周期刚好用完的那个 OBJ 画出多少（NanoBoyAdvance 画出的比按 GBATEK 周期算的多），`obj_cycles` 只检查离边界较远的 OBJ；跨过 OBJ 边缘或从透明像素开始的 OBJ mosaic 块；OBJ 没选为第一目标时，半透明 OBJ 受不受亮度效果影响（NanoBoyAdvance 和 mGBA 不同）；PB、PD 不为 0 时逐行写参考点；源地址递减或固定、从 ROM 读的 DMA。

### 参考帧

生成参考帧的 ROM 有 32 个：自写的 22 个 PPU 场景，jsmolka 的 `ppu/hello`、`shades`、`stripes`，alyosha 的 `ppu/Sprite_Disable_Midline` 和 `Sprite_Disable_on_VRAM_Access`，nba-hw-test `ppu` 下的 `bgpd`、`bgx`、`dispcnt-latch`、`greenswap` 和 `sprite-hmosaic`。每个 ROM 从开机运行，取第 60 帧，第 30 至 90 帧都和它相同；在 NanoBoyAdvance 上，画面最晚从第 11 帧起不再变化。`frames.tsv` 记录每个 ROM 的 SHA-256、画面开始不变的帧、和模型以及和 mGBA 是否一致。

- jsmolka 的 3 个：mGBA 的画面相同。
- alyosha 的 `Sprite_Disable_*`：两个文件完全相同。源码注释只说测的是 OBJ 取图的最后一个 VRAM 周期还能不能画出像素，没有描述期望的画面。NanoBoyAdvance 的画面是：第 11 至 70 行 OBJ 区宽 76 像素，第 71 行 11 像素，第 72 行 44 像素，第 73 行 76 像素，第 75 至 135 行 12 像素；mGBA 有 827 个像素不同。用户确认用 NanoBoyAdvance 的画面，仍然只记入报告。
- alyosha 的 `stripes_x_offset`：它轮询 VCOUNT 来卡写入时机，在 NanoBoyAdvance 上画面每帧都变，循环出现 7 种，用户确认不生成参考帧。
- nba-hw-test 的 5 个和附带的 `expected.png` 目测对照过：`bgpd` 和 `greenswap` 的附图是实机照片，NanoBoyAdvance 的画面和照片的结构一致；`dispcnt-latch` 的画面和附图一致；`bgx` 的白线在附图里是第 129 列，NanoBoyAdvance 画在第 130 列，附图是缩放过的截图，来源不明；`sprite-hmosaic` 的附图是 NanoBoyAdvance 1.8.2 的窗口截图，不是实机。mGBA 在 5 个上都和 NanoBoyAdvance 不同。用户确认 `dispcnt-latch` 必过，其余 4 个只记入报告：`bgx`、`bgpd`、`greenswap` 测一行之内写入生效的时刻，按扫描线渲染时不保证；`sprite-hmosaic` 没有实机依据。

### 测试数据

卡上的 GBA 测试数据共有 168 个 ROM：1-1 的 137 个和自写的 31 个，加上 32 个参考帧，连同清单共 216 个文件。清单记录了工具链版本、参考模拟器的版本，以及生成参考帧和打包时仓库的提交。

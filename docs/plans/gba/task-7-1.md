# 任务 7-1：S31 上 GB 和 GBC 跑到 2 倍速，修好画面错位

ESP32-S31 不跑 GBA。S31 的构建关掉 GBA，片内 RAM 留给 GB 和 GBC。这个任务的目标是 GB 和 GBC 游戏在 S31 的 2X 档跑到 200%，并且画面不再错位。

开工时在[任务状态](status.md)里把这个任务标为“进行中”。

## 已知的数据

以下是在[慧勤智远 ESP32-S31 CB](../../boards/wks_s31_cb.md)上用 profile 固件和 JIT 测的。模组是 N16R16V，有 16MB PSRAM，片内 SRAM 能用的是 492KB。

- 片内 RAM 的分配：IDF 和驱动的静态部分约 82KB，运行时的栈和驱动按空闲量推算约 56KB，JIT 代码池 208KB，GBA 的执行核心约 134KB，载入游戏以后只剩约 11KB。GB 的执行核心在 PSRAM，`gb::System`（24KB）和 GB JIT 的块表（80KB）也分不到片内 RAM。
- S31 访问 PSRAM 只经过一级缓存：I-cache 16KB，D-cache 64KB，没有 L2。精灵宝可梦 黄在 1X 档只有 21.5%，核 1 的时间大多花在 PSRAM 上的设备代码里。临时把 GB 的热代码（约 82KB）放进片内、把 GBA 的执行核心移出以后，速度到了 99.9%，核 1 空闲 75.9%，这时还有 48KB 的块表在 PSRAM。
- 游戏到原速以后，LCD 报 underrun。S31 的 RGB 驱动在 underrun 以后不重新对齐帧的起点（`esp_lcd_rgb_panel_restart()` 只支持 ESP32-S3），画面一直错位到复位。现在的时序是 PCLK 31.7MHz、59.73Hz，LCD 平均每秒从 PSRAM 读 45.9MB。
- GBA：黄金太阳 2 要把代码池缩到 128KB、ROM cache 缩到 5MB 才能载入，开场只有 42.5%，核 1 有 46.5% 的时间在重新编译。项目只在 P4 上启用 PSRAM 里的第二级（`components/platform/src/esp/executable.cpp`），S31 上代码池换出的块只能重新编译。GBA 的执行核心和运行时数据合计约 245KB，关掉 GB 也腾不出片内 RAM。

## 范围

- 先验证 S31 能不能执行 PSRAM 里的 JIT 代码，做法和 [GB 5-2](../gb/task-5-2.md) 在 Sila P4 上的核对相同：在 PSRAM 里写入代码，写回数据缓存、作废指令缓存、执行 `fence.i` 以后执行；在同一位置改写，再发布一次，执行的应该是新代码。能用的话，S31 也打开 PSRAM 里的第二级（`components/platform/src/esp/executable.cpp` 现在只在 P4 上允许），再按实测缩小片内的代码池。
- S31 的构建关掉 GBA，不编入 GBA 的核心和 JIT。片内 RAM 给 GB 的热代码和运行时数据，热代码的位置仍由 `components/core/linker.lf` 和 `tools/link_core_layout.py` 控制。两者合计约 186KB，比现在能分出的约 146KB 多 40KB。补这 40KB 的办法，在上一项的结果出来以后，和减少 GB JIT 的块数一起比较，按实测选。
- 关掉 GBA 以后，SETTINGS 里和 GBA 有关的选项、说明要调整；自动搜索 ROM 时不再收 `.gba`，空列表的提示也不再提 `.gba`。
- 修画面错位，按 2X 档实测的 underrun 选做法。候选有两种：把消隐区缩到规格书的下限，帧率不变，PCLK 降到约 27.2MHz；或者用 10 行的 bounce buffer，要约 31KB 片内 RAM，中断里的拷贝还要占核 0。
- GBA 的 ROM cache 预算现在固定是 14MB（`components/app/src/game.cpp` 的 `kGbaRomCache`），要一整块连续的 PSRAM，16MB PSRAM 的板子分不出来。改成宏，每块板子按自己的 PSRAM 设值。

## 约束

- ESP32-P4 的构建、布局和测试不变。
- 固件和测试构建共用同一套布局规则。
- JIT 优先：保护 JIT 的速度、片内 RAM 和热代码的布局。

## 完成标准

- 开工时和用户确认 GB 和 GBC 的测试场景，这些场景在 S31 的 2X 档整个统计区间的 `SPEED` 都不低于 200%。
- 测试场景里没有 LCD underrun，画面不错位。
- GB 和 GBC 的相关测试在 S31 上两个后端都通过；用户确认以后再决定是否跑全量回归。

## 记录

### 测试场景和判据

开工时和用户确认的场景：V-Rally 锦标赛版的槽位 1 和槽位 2、史前人的槽位 1、精灵宝可梦 黄的槽位 2。存档是用户这次重新存的，和 P4 上测过的位置不同，所以不和 P4 比较。四个都是 `.gb` 游戏，没有跑在 CGB 双速模式下的 GBC 游戏。判据和用户商定为：JIT 2X 档每个 2 秒的 `stat` 窗口都不低于 199.5%；解释器不要求 2X，1X 最好到原速。验收用 `wks_s31_cb-release`，profile 固件只用来分析。

### 结果

慧勤智远 ESP32-S31 CB（ESP32-S31 v0.0、320MHz、16MB PSRAM），`wks_s31_cb-release`，只编入 GB。每个场景载入 ROM、读槽位、切换速度，等 4 秒以后每 2 秒取一次 `stat`，共 7 个窗口。frm 是每个模拟帧的平均耗时，核 1 是模拟器所在的核。

| 场景 | 后端和速度 | `SPEED` | frm 平均 | 核 1 空闲 |
|---|---|---|---|---|
| V-Rally 槽位 1 | JIT 2X | 199.9%～200.3% | 2.07ms | 76% |
| | 解释器 1X | 99.7%～100.3% | 5.52ms | 66%～67% |
| | 解释器 2X | 199.9%～200.3% | 5.38ms | 35% |
| V-Rally 槽位 2 | JIT 2X | 199.9%～200.1% | 3.19ms | 62% |
| | 解释器 1X | 99.7%～100.3% | 6.39ms | 61%～62% |
| | 解释器 2X | 199.9%～200.1% | 6.18ms | 26% |
| 史前人 槽位 1 | JIT 2X | 199.9%～200.1% | 1.84ms | 79% |
| | 解释器 1X | 99.6%～100.3% | 4.04ms | 76% |
| | 解释器 2X | 199.9%～200.1% | 3.80ms | 55% |
| 精灵宝可梦 黄 槽位 2 | JIT 2X | 199.9%～200.1% | 1.57ms | 81% |
| | 解释器 1X | 99.7%～100.4% | 2.67ms | 83%～84% |
| | 解释器 2X | 199.9%～200.1% | 2.59ms | 69% |

V-Rally 槽位 1 第一次试跑时有一个窗口是 199.7%，也在判据以内。12 组测试里 LCD 和音频都没有 underrun。游戏运行时片内 RAM 还剩 17KB（JIT）或 57KB（解释器），PSRAM 还剩 4.82MB～5.86MB（JIT）或 7.37MB～8.40MB（解释器）。余量已经够了，用户决定不再做性能优化。

解释器 1X 跑 V-Rally 时，`drw` 只有 55.8～59.0，比模拟帧率 59.5～59.9 低；JIT 1X 也出现过 56.8。2X 档的 `drw` 是 59.5～60.4。原因没有查。

这组数据是在下面“Review 后的改动”之前测的，改动以后没有重测。

### 做法

- 编入哪些系统改成 `components/core/Kconfig` 的 `GBX_SYSTEMS`，S31 选只编入 GB。GB 的执行核心放进片内 RAM，代码池和 GB JIT 的块数都没有缩小。只编入 GBA 的选项也加了，`sila_p4` 的正式固件和测试固件编译通过，没有上板运行。
- S31 能执行 PSRAM 里的 JIT 代码：完整构建的测试固件上 `run_jit` 179 项全过，其中有 PSRAM 代码首次执行和改写以后再执行的检查；只编入 GB 的测试固件 172 项全过，ARM7TDMI 和 GBA 的检查不在固件里。S31 因此也打开了第二级。第二级按 P4 的 8MiB 分配时，PSRAM 不够即时存档用，载入游戏以后报 “No memory for the instant saves”，所以第二级和 GBA ROM cache 的大小改成各板设定的 `GBX_GB_JIT_STORE_KB` 和 `GBX_GBA_ROM_CACHE_KB`，S31 设成 2048KB 和 5120KB。
- 只编入 GB 时，ROM 列表只收 `.gb` 和 `.gbc`，空列表的提示不提 `.gba`；卡上的 `.gba` 显示 “This firmware runs GB and GBC games only.”。SETTINGS 里没有只属于 GBA 的选项，提到 GBA 的说明只在运行 GBA 游戏时出现，不用改。
- 画面错位：先把 PCLK 从 31.70MHz 降到 27.85MHz（907×514），V-Rally 里每 30 秒还有 8～10 次 underrun，和后端、速度都无关，菜单界面 60 秒里没有。PPA 和 2D-DMA 的 burst 从 128 降到 64 没有变化，缩放改成 NEAREST 降到 5 次。underrun 的间隔大多接近 1 秒的整数倍，和 VFD 每秒刷新一次读数对得上。游戏运行时停掉 UI 的刷新，就不再 underrun；UI 照常刷新、present 任务画游戏帧时也拿着 LVGL 锁，也不再 underrun，用户玩了一段时间确认画面不错位。这把锁做成了 `CONFIG_GBX_DISPLAY_LOCK_GAME_DRAW`，只在 `sdkconfig.defaults.esp32s31` 里打开；PCLK 和时序恢复原样。
- VFD 的 `MEM` 格在 S31 和 OSPTEK ESP32-P4C5 上加了 PSRAM 空闲量，按用户的要求。

### Review 后的改动

- 只有应用或测试调用的 GB 入口留在 flash。改之前 release 和 test-release 的热区里函数不一样，`update_deadline()` 分别在 0x2f015fd8 和 0x2f016010；改完以后两种构建的函数一样，只差全局指针附近的两条查表指令。
- JIT 的编译、安装和块的撤销移到 `Sm83Jit::install()` 和 `retire_containing()`，留在 flash。片内的 `Sm83Jit::execute()` 从 11216 字节缩到 4752 字节。
- 热代码查的表移到全局指针能直接寻址的范围下面，`ppu.cpp` 的 `kEdges` 改成编译期常量，release 的热区里不再有相对全局指针的访问。这一项改完以后只编了 S31 的 release，test-release 没有重编，两种构建的地址没有再核对。Mixer 用的浮点常量被链接器合并进了 IDF 的常量池，地址仍然取决于 IDF 和应用的数据，离全局指针约 5.6KB，release 里没有被缩短。

### 测试

ESP32-P4 的 `sila_p4-release` 和 `sila_p4x-release` 改完以后编译通过，热区的边界和改之前相同，没有上板运行。GB 和 GBC 的测试 ROM 和单步测试没有在 S31 上跑，Review 后的改动以后 `run_jit` 也没有重跑，用户决定不跑，按现状完成。

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

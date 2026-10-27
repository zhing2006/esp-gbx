# ESP32-P4 影响模拟性能的特性

本文整理 ESP32-P4 上影响模拟器性能的芯片特性，写代码、做优化时参考。芯片的数据出自乐鑫的 [ESP32-P4 v1.3 技术参考手册](https://documentation.espressif.com/esp32-p4-chip-revision-v1.3_technical_reference_manual_en.html)（下称 TRM）和 ESP-IDF [`fff9895`](https://github.com/espressif/esp-idf/tree/fff9895c82d744c7237be8847347bdd1b07c6643)。实测数据都在 Sila P4（ESP32-P4 v1.3、360MHz、32MB PSRAM、200MHz X16）上测得，详细的数据和条件见所引的任务文档。

## CPU 流水线

HP CPU 有两个核，每个核都是 5 级、顺序执行、单发射的流水线，指令集是 RV32IMAFC 加 Zc 和乐鑫的自定义指令（TRM 1.1、1.2）。v1.x 没有 Zba、Zbb、Zbs，这些只在 v3.x 上有。

因为是单发射，每条指令至少占一个周期。生成的代码多一条指令，就多一个周期；访存等待和分支预测失败的周期另外再加。

## 分支预测

TRM 1.12 描述了分支预测，每个核各有一套：

- BHT 是 512×16 位，按最近 13 次条件分支的结果（全局历史）索引，不看分支所在的地址。不同地址的分支只要历史相同，就共用同一个计数器。
- BTB 只有 16 项，按 PC 的低 16 位索引。条件分支、JAL、J、C.J 的目标从这里预测；目标不在 BTB 里时，要等指令执行以后才知道去哪里。
- RAS 有 4 项。`rd` 是 `ra` 的 JAL、JALR 压入下一条指令的地址；`rs1` 是 `ra`、`rd` 不是 `ra` 的 JALR 弹出，并且从弹出的地址取指。弹出的地址和 `ra` 不同时，流水线冲刷。
- 文中没有提到 JALR 的目标预测。除了上面的返回以外，`jalr` 跳到寄存器里的地址都要等到执行时才确定。

这对生成的代码有几点影响：

- 经过 `ra` 返回、但是不回到 `ra` 本身的写法，比如 stub 跳过调用后面的数据字、用 `jalr zero, ra, 4` 返回，每次都和 RAS 弹出的地址不符，会冲刷流水线。而且弹出的地址指向数据字时，这个字会被当成指令取进来：在 P4 v1.3 上测过，它恰好是硬件循环指令（比如 0x880150ab，`esp.lp.setupi`）时，冲刷以后 PC 就错了。所以后面跟着数据字的调用不经过 `ra` 链接，而是用 `jal t6`，stub 再经过 t6 返回。GBA 的块调 gateway 全都这样做；GB 的块只有这类调用这样做，其余调用仍然经过 `ra`，好让 RAS 预测它们的返回。经过 t6 的 `jalr` 不用 RAS，也要等到执行时才知道目标。
- 块之间经由寄存器跳转（链接字、分发表）都不能预测。
- 热循环里跳转的地点很多，BTB 的 16 项装不下，多数跳转都要等到执行才知道目标。

`mhcr`（0x7C1）的 RS、BPE、BTB 三位分别打开 RAS、条件分支预测和 BTB。IDF 在启动时调用 `esp_cpu_branch_prediction_enable()` 把它们都打开（`components/esp_system/port/cpu_start.c`）。

## 性能计数器

HP 核在 M 模式下可以读 `mcycle` 和 `minstret`，两者之比就是 IPC。另外有 3 个事件计数器，每个只能计一种事件，要先写对应的 `mhpmevent` 选择事件（TRM 寄存器 1.23 至 1.25）：

| 计数器 | 事件选择 | 计的事件 |
|---|---|---|
| `mhpmcounter8` | `mhpmevent8` = 0x6 | 条件分支预测失败 |
| `mhpmcounter9` | `mhpmevent9` = 0x7 | 条件分支指令 |
| `mhpmcounter13` | `mhpmevent13` = 0xB | store 指令 |

HP 核自己没有缓存缺失的计数器。TRM 第 3 章那张包含访存等待周期的计数器表，说的是 LP CPU，不适用于 HP 核。

缓存控制器另有一组访问计数器，TRM 里没有描述，寄存器定义在 IDF 的 `components/soc/esp32p4/register/hw_ver1/soc/cache_reg.h`。L1 的 `CACHE_L1_CACHE_ACS_CNT_CTRL_REG` 和 L2 的 `CACHE_L2_CACHE_ACS_CNT_CTRL_REG` 按总线打开和清零计数，IBUS0、IBUS1 是两个核取指的总线，DBUS0、DBUS1 是两个核访问数据的总线。每条总线有 hit、miss、conflict、next-level read 几个计数，数据总线还有 next-level write。性能分析版固件的 `cache <ms>` 命令读出这些计数。

寄存器的注释只说计的是“次数”。在核 1 上用已知的读取模式核对过（性能分析版固件的 `cacheprobe` 命令，另一个核基本空闲）：

| 读取模式 | 每次读取的周期 | L1 的计数，每次读取 |
|---|---|---|
| 片内 8KB，顺序读字 | 8.0 | hit 1 |
| 片内 128KB，每次跨 64 字节 | 12.1 | next read 1，miss 约 7 |
| PSRAM 32KB，每次跨 64 字节 | 8.1 | hit 1 |
| PSRAM 4MB，每次跨 64 字节 | 192.5 | next read 1，miss 约 186；L2 的 next read 1，miss 约 91 |

8 个周期是循环本身的开销。由此看来，next read 是从下一级填充的行数，miss 接近等待缺失的周期数，而不是缺失的次数。片内 RAM 的一次 L1 缺失只多花约 4 个周期，PSRAM 的一次缺失（L1 和 L2 都不命中）约 185 个周期。片内 RAM 不经过 L2，所以 L2 的计数只包括 PSRAM 和 flash 的访问。conflict 在两个核同时访问数据缓存时很大，可能是两个核争用共用的 L1 数据缓存而等待的周期，这一点还没有用读取模式单独核对。

## 缓存和内存

TRM 7.3.3.2 给出的缓存是两级：

| 缓存 | 容量 | 行大小 | 组相联 |
|---|---|---|---|
| L1 指令缓存 | 16KB | 64B | 4 路 |
| L1 数据缓存 | 64KB | 64B | 2 路，可写回或写直达 |
| L2 缓存 | 128KB、256KB 或 512KB | 64B 或 128B | 8 路 |

- IDF 的 `components/hal/esp32p4/include/hal/cache_ll.h` 里，L1 指令缓存有 ICACHE0、ICACHE1 两组寄存器，数据缓存只有一组 DCACHE，所以指令缓存是每个核一份，数据缓存由两个核共用。TRM 的文字没有直接这样写，这一条是从寄存器推断的。
- 片内的 HP L2MEM 共 768KB（0x4FF0_0000 起），L2 缓存从里面划出，项目默认配 128KB。片内 RAM 也要经过 L1 缓存访问（IDF `soc_caps.h` 的 `SOC_CACHE_INTERNAL_MEM_VIA_L1CACHE`）。
- PSRAM 和 flash 经 MMU 映射，CPU 通过 L1 和 L2 访问，PSRAM 在 0x4800_0000 起。L2 缓存由两个核和片内 RAM 以外的全部数据、代码共用。
- LP SRAM（0x5010_8000 起，32KB）也在 IDF 的堆里（`CONFIG_ESP_SYSTEM_ALLOW_RTC_FAST_MEM_AS_HEAP`，默认打开），能力是 `MALLOC_CAP_INTERNAL`，但是没有 `MALLOC_CAP_DMA`（`components/heap/port/esp32p4/memory_layout.c`）。片内 L2MEM 不够时，`MALLOC_CAP_INTERNAL` 的分配会落到这里。HP 核访问 LP SRAM 很慢：GBA JIT 的分发表落在 LP SRAM 时，查一次表要约 770 个周期；同一张表放在 PSRAM，经过缓存只要约 75 个周期。所以要求片内的分配都加上 `MALLOC_CAP_DMA`，只用 L2MEM，见 `platform::allocate_internal()`。普通的 `malloc` 和 `new` 也可能落在 LP SRAM，热数据不要这样分配。
- 缓存支持手动和自动预取，以及按地址锁定（TRM 7.3.3.3，ROM 函数 `Cache_Lock_Addr`）。把 JIT 第二级里那份 gateway（约 7KB）锁在 L2 缓存里，测不出差别。
- DMA（PPA、2D-DMA、显示）直接读写 L2MEM 和 PSRAM，不经过 CPU 的缓存。CPU 写过的数据交给 DMA 之前，要先把缓存写回，PPA 的驱动自己做了这一步。

## 实测到的影响

下面几条在 GBA 任务 [6-1](plans/gba/task-6-1.md) 中测得，场景是 V-Rally 3（欧版）比赛中的赛道画面，GBA JIT，1X：

- 核 1 模拟时 IPC 只有 0.38，平均每条 GBA 指令约 27 条主机指令。指令多，等待也多。
- 两个核共用数据缓存、L2 缓存和 PSRAM，所以一个核的访存会拖慢另一个核。核 0 只复制 VRAM 页、不绘制时，核 1 的速度是 89%，IPC 0.46；核 0 正常绘制和呈现时是 72% 至 74%。
- 呈现任务用 CPU 把 77KB 的 BGR555 帧转成 RGB565 再交给 PPA 时，核 1 的速度比渲染器直接写 RGB565、PPA 直接读的做法低约 4 个百分点。CPU 成块读写 PSRAM 的代价不只在做这件事的核上。
- L2 缓存从 128KB 改成 256KB（JIT 代码池相应地从 192KB 减到 64KB），只快了约 2 个百分点。
- 用缓存控制器的计数器看核 1（`SPEED` 约 82%）每秒的 3.6 亿个周期：L1 指令缓存的 miss 约 7300 万，数据缓存的 miss 约 4100 万、conflict 约 3900 万，条件分支预测失败约 320 万次，退休约 1.7 亿条指令（IPC 0.48）。按上面的核对，三项等待加起来占四成以上。
- 在 gateway 的访存 stub 里用 `mcycle` 计时（含 2 至 3 个周期的计时开销），读一次 IWRAM（片内）平均 8.1 个周期，EWRAM（PSRAM）18.4 个，VRAM（PSRAM）46.5 个。
- GBA 解释器按表分派，每条指令由分派循环用 `jalr` 调用一个处理函数，处理函数用 `ret` 返回。试过让处理函数结尾不返回，而是各自用 `j` 跳到同一个分派函数，由它直接跳进下一个处理函数，四个场景反而慢了 10% 到 17%。`ret` 的目标由 RAS 预测；那条 `j` 在一百多个处理函数里各有一份，地址都不同，16 项的 BTB 很可能装不下，要等到执行才知道目标。这个原因是推断的，没有用计数器单独核对。

GBA 任务 [6-1](plans/gba/task-6-1.md) 试过让 GBA ROM 的生成代码都在 PSRAM 里执行（片内代码池只留 48KB 给 RAM 里的代码），腾出的片内 RAM 放 VRAM：2X 档下四个场景都变慢了，马里奥高尔夫从 192.9% 降到 136.3%，V-Rally 3 从 106.0% 降到 100.7%。

GB 段 [5-2](plans/gb/task-5-2.md) 测过全部生成代码都放在 PSRAM 执行的配置：代码约 204KB 的场景，模拟任务占核的比例比放在片内代码池时多约 25%；代码约 64KB 的场景反而略快，那次的 L2 缓存是 256KB。

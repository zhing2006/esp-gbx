# 架构

本文说明项目的分层、目录、各模块的职责和边界，以及构建和测试的方式。项目要做什么、做到什么程度，见[产品设计](product-design.md)。参数、数据格式和时序的依据写在代码和注释里，本文只写在哪个文件。

## 分层

代码按职责分层，上层可以调用下层，下层不知道上层的存在：

| 层 | 目录 | 职责 | 能依赖 |
|---|---|---|---|
| 应用 | `components/app/` | 菜单、选游戏、存档写回、音画同步 | 模拟核心、界面、平台接口 |
| 界面 | `components/ui/` | 游戏页、菜单绘制、资源包读取、触摸命中判断、VFD 文字绘制 | 平台接口、LVGL |
| 模拟核心 | `components/core/` | 各原机的 CPU 解释器、设备模型和 JIT 前端 | JIT 后端、平台接口 |
| JIT 后端 | `components/jit/` | RV32 指令编码、寄存器分配、代码缓存、块链接 | 平台接口 |
| 平台实现 | `components/platform/src/esp/`、`components/platform/src/host/`、`components/board/`、`boards/`、`drivers/` | 实现平台接口：显示、音频、按键、存储、时钟、外部内存分配、可执行内存和缓存同步 | ESP-IDF 或 PC 的系统库 |

`components/` 下的 `core/`、`jit/`、`app/` 和 `ui/` 只能 include `components/platform/include/gbx/` 下的平台接口，不能 include ESP-IDF 的头文件，所以同一份代码既能编进固件，也能编进 PC 版。`ui/` 的游戏页依赖 LVGL，固件和 PC 的 GUI 构建共用这一页，无窗口构建不依赖 LVGL。

## 目录

```
esp-gbx/
├── CMakeLists.txt      ESP-IDF 工程入口
├── CMakePresets.json   固件的 preset：板子 × 构建类型 × 正式固件或测试固件
├── sdkconfig.defaults  所有板子共用的配置；.debug 和 .release 是构建类型的配置，.profile 用于性能分析版
├── partitions.csv      flash 分区表，所有板子共用
├── main/               固件入口和串口控制台，初始化以后交给 app
├── components/
│   ├── core/           模拟核心，include/gbx/ 和 src/ 下都按下面三个目录划分
│   │   ├── common/     调度器、模拟时间线、ROM 访问和固定容量的追踪缓冲
│   │   ├── gb/         GB 和 GBC：SM83 解释器、PPU、APU、定时器、MBC；jit/ 是 SM83 前端
│   │   └── gba/        GBA：ARM7TDMI 解释器、PPU、声音、设备、存档芯片、GPIO 和 RTC；jit/ 是 ARM 前端
│   ├── jit/            RV32 后端
│   ├── app/            菜单、ROM 扫描、设置、存档和游戏运行流程
│   ├── ui/             游戏页、菜单视图、资源包读取和触摸命中判断
│   ├── platform/
│   │   ├── include/gbx/  平台接口
│   │   └── src/
│   │       ├── esp/    ESP-IDF 上的实现：显示呈现、触摸、资源分区；P4、S31 和 C5 的差异在这里处理
│   │       └── host/   PC 上的实现
│   └── board/          板级接口和各板子共用的步骤，只编进固件
├── boards/
│   ├── esp32_s31_korvo_1/ 一块板子一个目录
│   ├── jc1060p470/
│   ├── m5stack_tab5/
│   ├── osptek_p4c5/
│   ├── sila_p4/
│   ├── sila_p4x/
│   ├── waveshare_c5_lcd28/
│   └── wks_s31_cb/
├── drivers/            板上外设的驱动，可以被多块板子共用
├── host/               PC 版的 CMake 工程
├── tests/
│   ├── runner/         电脑上的测试程序
│   ├── protocol/       二进制传输的帧格式和编解码
│   ├── device/         板子上的测试固件，是根工程里的一个组件
│   ├── jit/            RV32 编码与外部汇编器对照的输入
│   └── gba/
│       ├── roms/       自写的 GBA 测试 ROM 的源码
│       └── frames/     GBA 参考帧
├── assets/
│   └── ui/             UI 素材：common/ 是各板共用的控件和 VFD 字体，板子目录是底图和布局
├── raws/
│   └── ui/             UI 素材的原始图、提示词和各板的 spec.json，目录划分和 assets/ui/ 相同
├── tools/              开发用的脚本，用途见 tools/README.md
├── docs/
│   ├── boards/         各板子的资料整理
│   ├── plans/          按开发阶段分目录的任务计划
│   │   ├── gb/         GB 阶段路线图各里程碑拆出的任务和任务状态
│   │   ├── gbc/        GBC 阶段的任务和任务状态
│   │   └── gba/        GBA 阶段的任务和任务状态
│   └── ui-layouts/     UI 设计说明和效果图
└── refs/
    └── boards/         各板子的原始资料，随仓库提交
```

`components/` 下每个目录是一个组件，公开头文件放在 `include/gbx/`，源文件放在 `src/`，依赖由各组件的 CMakeLists 声明。板级接口只对固件的平台实现和入口开放，应用和 UI 通过平台接口使用硬件。顶层的 `drivers/` 还没有建立。

## 模拟器

GB 和 GBC 共用 `components/core/` 下的 `gb/`，GBC 是同一个系统的另一种机型。GBA 复用调度器和 JIT 后端，设备模型和 JIT 前端另外实现。本节的路径都相对于 `components/core/` 的 `include/gbx/` 或 `src/`。

### 机型和运行模式

系统有三种运行模式，在加载 ROM 时选定，到下一次加载以前不变：GB 模式模拟 DMG-C，CGB 模式和 DMG 兼容模式（Pan Docs 的 Non-CGB Mode）模拟 CPU CGB E。卡带插在 GB 还是 GBC 上由调用者决定，`gb/mode.h` 的 `run_mode()` 再按卡带头选出运行模式，规则见[产品设计](product-design.md#gbc)。

系统不运行 boot ROM，复位时直接设置 boot ROM 交出控制时的状态，各项的来源写在实现的注释里。

CGB 的支持不能拖慢 GB 模式。CGB 的状态不改变 GB 模式下机器状态的布局，CGB 的判断只加在 IO 寄存器、bank 切换、VRAM 写入和 PPU 事件这些本来就要分支的路径上。

### 时间线和事件

所有设备共用 `common/scheduler.h` 的模拟时间线，时间单位能表示 GBC 倍速下 CPU 和 PPU 的不同周期。设备不逐周期推进。调度器为设备事件保留固定的槽，设备只在程序可能观察到变化的时刻安排事件，比如定时器溢出、串口传输完成和可能升起 STAT 中断线的 PPU 阶段；其余的变化等 CPU 读写设备时，按经过的时间追到这次访问的时刻再补算。APU 和 MBC3 的 RTC 不占事件槽，只在被访问时追赶。

设备事件必须在准确的模拟时刻生效。总线在每次访问之前派发已经到期的事件；事件落在一条指令中间时，这条指令后面的访存要看到生效以后的状态。

PPU 的模式、LY、STAT 中断线和显示期间的访问限制都在 CPU 所在的核上计算，不等渲染结果。中断响应、PPU 各阶段和 KEY1 速度切换的时刻都按测试 ROM 确定，依据分别写在 `System::boundary()`、`gb/ppu.cpp` 和 `Memory::switch_speed()`。

CGB 的 VRAM DMA 和速度切换让 CPU 停下，设备照常运行，停顿计入模拟时间。KEY1 倍速下 CPU、定时器、DIV、串口和 OAM DMA 变快，PPU、HBlank DMA 和声音的时间不变。

调度器按事件槽的个数做成模板，GB 和 GBA 各用自己的一份。GBA 的时间单位是 CPU 周期，每秒 2^24 个（`gba/memory.h`）。GBA 的 CPU 把剩余的周期朝期限倒数，期限取下一个事件和这次运行的目标中较早的一个。每次总线访问和内部周期都先看剩余的周期，用完了才进入 C++ 派发到期的事件，所以事件落在指令中间时，这条指令后面的访问也能看到它。PPU、定时器和 DMA 只在中断、DMA 或渲染任务要用到的时刻安排事件，比如只有 DMA 在等 HBlank 时才有 HBlank 的事件。

### 内存映射和卡带

`gb/memory.h` 是 DMG 和 CGB 共用的内存映射，按页决定一次访问是直接读写还是经过处理函数。IO、VRAM 和 OAM 不直接映射，因为它们的访问结果取决于设备当时的状态，VRAM 的写入还要记录下来交给渲染核。事件可能改变页的访问权限，所以解释器和 JIT 处理完事件以后都要重新读取权限。OAM DMA 进行期间，被占用的总线撤销直接访问权限。

`common/rom.h` 提供只读的 ROM 窗口，数据由调用者持有。GB 的整个 ROM 常驻内存，CPU 直接读窗口；GBA 的 ROM 经过分页缓存，见“GBA 的内存和设备”。`gb/cartridge.h` 按卡带头选择 MBC，管理 bank 寄存器和卡带 RAM，不推进时间。

卡带 RAM，以及 CGB 多出来的 WRAM、VRAM bank 和调色板 RAM，都通过平台接口分配在外部内存，固件上是 PSRAM。它们在 JIT 启动之前分配，放在片内会挤掉 JIT 的代码池。

MBC3 的 RTC（`gb/rtc.h`）按模拟时间计数。模拟核心不读宿主时间，系统没有运行的那段时间由调用者通过系统接口加到 RTC 上。

### SM83 解释器

`gb/sm83.h` 的同一份指令语义接两种总线：`gb/bus.h` 的平坦总线给单步套件用，记录每个 M-cycle 的访问；整机使用 `gb/memory.h`。连续执行循环按 CPU 的速度编译成两份，GB 模式只用原速的那份，所以倍速没有给 GB 模式的指令路径加上判断。

中断分派、HALT 和 STOP 由 `gb/system.cpp` 处理，两个后端共用，不算作解释器回退。

### SM83 JIT

接口在 `gb/jit/sm83_jit.h`，代码生成和寄存器约定写在 `gb/jit/sm83_jit.cpp`，运行时的约束写在 `gb/jit/sm83_jit_runtime.cpp`。单步测试和整机用同一个翻译器，单步测试换掉总线实现，并把块长限制为一条指令。

翻译。前端按基本块把指令翻译成 RV32 代码，guest 寄存器和本地累计的周期数常驻在宿主寄存器里。H、N、C 每条指令都完整算出，因为块随时可能从慢路径离开原生代码，那时 F 必须完整；Z 到需要读取时才算。访存、出口和大部分标志运算调用初始化时生成的共用代码（gateway），不在每个块里展开，这样代码池能装下游戏更多的工作集。

时序。生成的代码只在确定下一个设备事件之前做得完时才直接访问内存，其余访问经过 C++ 的总线处理。事件落在指令中间时，块可以做完当前指令，但不能开始下一条。

块标识和失效。块标识包含入口地址、代码所在页的 bank 映射和入口的 EI 状态，所以 bank 切换以后不会执行另一个 bank 的翻译结果，原来 bank 的块也不用丢掉。ROM 里的代码字节是常量；入口在卡带 RAM、WRAM 或 HRAM 的块在入口核对代码字节，发现改写就回收，反复被改写的入口改为逐条核对。

成批运行。`System` 让原生代码成批运行，相连的块直接互相跳转，guest 寄存器一直留在宿主寄存器里。只改变 IO、OAM 和 VRAM 访问结果的设备事件可以推迟派发：原生代码访问这些地址时都要先进入 C++ 派发到期的事件，所以程序观察不到推迟。推迟的期限和例外写在 `sm83_jit_runtime.cpp`。到达目标时间、一帧结束或出现能响应的中断时，批次在指令边界停下。

块间连接。直接跳转的目标块存在以后，出口改成直接跳到目标，动态目标查分发表。只有 bank 切换不会改变目标时才建立直接连接，所以切换 bank 时不用逐块拆除连接。

代码缓存。片内的代码池按 CLOCK 淘汰。ROM 里的块另外在 PSRAM 的第二级存一份可以原地执行的副本，被代码池淘汰以后在第二级执行，常用的再拷回代码池。两级都没有的块按退避规则交给解释器执行，避免反复编译。编译或发布失败要记录原因，回退不能算作 JIT 测试通过。

### ARM7TDMI 解释器

`gba/arm7.h` 是 CPU 的状态和入口，解释器和 JIT 共用这份状态。当前模式的寄存器放在一起，切换模式时再搬动分组寄存器。解释器从任意状态开始执行一条指令就返回，单步测试和 JIT 回退都这样调用。IRQ 的进入也在这里，整机在中断线到达 CPU 以后的指令边界上调用它。

CPU 和总线的边界在 `gba/bus.h`。每个总线周期调用一次，取指、读写和内部周期都算，带着宽度、N/S 和 SWP 的 Lock；地址是 CPU 送出的原地址，对齐和等待周期由总线处理。函数指针形式的总线只给单步测试用，`TestBus` 按用例记录的访问返回数据，并记下实际的访问。指令语义在 `src/gba/executor.h`，是按总线类型展开的模板：平坦总线的一份在 `arm7_step.cpp`，留在 flash；整机的总线是 `src/gba/mapped_bus.h` 的 `MappedBus`，它的一份连同连续执行的循环都在 `arm7_mapped.cpp`，放在片内 RAM，所以游戏的访存既没有间接调用，也不做记录。每种总线只能有一个文件展开模板，因为两个文件各展开一份时，链接器可能留下放在 flash 的那份。两份共用的函数在 `arm7.cpp`，放在片内 RAM。

译码按表分派：ARM 用操作码的位 27–20 和 7–4，Thumb 用位 15–6，查一张字节表得到处理函数的序号。处理函数按决定操作的位特化（数据处理的操作和 S、读写的宽度和方向等），其余的位运行时译码，罕见的指令共用通用的函数。运行循环按 ARM 和 Thumb 状态各有一个，状态不变时一直留在同一个循环里，每条指令只走一个跳转的分支，因为 P4 的 BTB 只有 16 项。ARM 指令的条件不是 AL 时，不用分支判断是否通过，失败就选中跳过指令的处理函数，因为条件取决于标志，分支预测很难猜中。处理函数以 `Arm7State`、总线和操作码为参数，运行循环把前两个留在寄存器里。处理函数里只内联最常见的路径，其余的部分经过 `out()` 调用单独的函数，这些函数同样以状态和总线为参数，所以处理函数不用在栈上建对象。

常见的处理函数不建栈帧，慢路径都是尾调用：

- 只有取指令一个总线周期的指令（数据处理、Thumb 的移位、加减和 ALU 运算等）先写寄存器，最后取指令。总线在取指令时可能追事件、跑 DMA、读 open bus，这些只看 R15、流水线和 CPSR 的 I、T，这些指令在取指令之前都没有改它们，所以先后看不出来。
- 跳转交给一个单独的函数取指令、refill 和检查等待循环。
- 单个读写和 LDM、STM 在取指令和访存都能直接完成时一次做完：取指令在 IWRAM、BIOS 或宏打开时的 ROM，数据在 IWRAM 或 EWRAM，而且剩余周期够取指令、访存和读的内部周期用，途中不会追事件。否则整条指令交给原来逐个周期检查的代码。

P4 没有 Zbb，`std::countr_zero()` 和 `std::popcount()` 会调用 libgcc，热代码里改用 `gba/bits.h` 的乘法查表和移位相加。整机那份解释器和 CPU 所在核上的其他执行代码放在片内 RAM 的高段，和 JIT 的代码池共用同一段堆，GB 的 JIT 代码池也在这里：执行代码变大，两种 JIT 预留的连续空间就可能分不出来，所以加特化之前要先看代码的大小。

等待循环。解释器跳回循环开头时，`arm7_spin.h` 判断从开头到这条跳转是否构成一个只读内存的循环，每一圈的结果只取决于这一圈里写过的寄存器和循环外不变的值。是的话，`Memory::spin_loop()` 记下这一圈开头的 CPU 状态、预取缓冲相对当前时刻的状态和卡带总线的地址；连续两圈的周期数和开头的状态都相同时，把到下一个事件之前能走完的整圈一次跳过，预取缓冲的时刻一起后移。记录按循环开头和跳转指令的地址区分。事件重新安排、读 I/O、从慢速区域取指令，以及 B 和条件跳转以外的写 R15，都会让记录作废，所以两圈之间只要离开过循环再回来，就不会跳过：回来要么经过另一条往回的跳转，要么经过会作废记录的写 R15。单步入口（JIT 的调度器和 lockstep 用它）每一步都作废记录，一次只执行一条指令。判断本身在同一个循环上保留：循环在 BIOS 或 ROM 里时两种结论都保留，在 RAM 里时只保留“不是”，因为代码可能被改写，保留“不是”最多少跳过一些圈。

### ARM7TDMI JIT

接口在 `gba/jit/arm7_jit.h`。编译器分成 `gba/jit/arm7_compiler*.cpp` 几个文件，各文件的分工和翻译方式写在 `arm7_compiler.h`；gateway 在 `arm7_gateway.cpp`。分发循环和块调用的运行时 helper 在 `arm7_jit_runtime.cpp`，放在片内 RAM，排在解释器后面，编译器和测试总线留在 flash。寄存器约定、gateway 的栈帧和出口的格式写在 `arm7_jit_runtime.h`。

寄存器。R0–R14 在整个运行期间常驻宿主寄存器，R15 是每条指令的常量。N、Z、C、V 各占一个寄存器，另有一个寄存器记剩余的总线周期，三个寄存器可以跨指令保留标志位的来源。R0–R8 和 R12–R14 占 callee-saved 寄存器，调用 C++ helper 时不用保存；R9–R11 和上面这些寄存器是 caller-saved，由 helper 的 stub 保存和恢复。CPSR 的其余位、分组寄存器和 SPSR 留在 `Arm7State`，只有 MSR、MRS、异常和用户模式寄存器的传送访问它们。

标志位。设置标志位的指令只记下来源，比如结果寄存器、加减法的操作数或者移位器移出的那一位，等到有指令读取、来源要被改写或者出块时才计算；CMP 后面的条件分支直接比较两个操作数。块可以在任何指令边界退出，每个边界的出口描述还没算出的标志位，由 C++ 算完写回 CPSR，所以块外看到的 CPSR 和逐条执行的相同，异常保存的 SPSR 也一样。

块。块从入口执行到写 R15、进入异常、切换模式或者达到长度上限的那条指令为止。条件分支在两条路径上都结束块，其他条件指令的两条路径在块内重新汇合。单步测试的块标识由入口的 R15、T 位、CPU 模式和长度上限组成；整机的块标识由入口的 R15、T 位、CPU 模式和代码所在区域的等待周期组成，因为块把取指的周期编进了代码，改了 WAITCNT 就要用另一个块。每条指令开始前检查剩余周期，用完就从这个边界退出，所以设备事件可以让块停在任意指令边界。

代码。块装在 `jit::CodeCache` 的代码池里，gateway 放在代码池前面的共用代码里，块用 `jal` 调用它的 stub。整机的块在调用后面放一个数据字，存着 stub 走慢路径时要用的地址和请求，所以一条指令的取指、访存和边界检查在块里只占几个字节，代码池能装下游戏更多的工作集。stub 里的等待周期是立即数：WAITCNT 或 EWRAM 的等待周期改变时，按新的周期重新生成 gateway，布局不变，第二级各段开头的副本也一起改写。块的标识里有它所在区域的等待周期，所以运行着的块总是和当前的 stub 一致。I/O 的读写大多经过 C++，几类常见的在 stub 里完成：读只读回写入值的寄存器时直接读 `io_`，按 `Memory::io_reads` 的掩码处理；IF 在 `io_` 里保留一份副本，但是副本里已经有本周期刚请求的位，而 CPU 要到下一个周期才读得到，所以只在降低精度的宏打开时直接读；读 VCOUNT 时，当前时刻和 deadline 都在 `Memory::vcount_from` 开始的那一行里才直接读，这样跳过的等待循环每一圈读到的值相同；写显示寄存器 08h-57h（参考点除外）时，如果这一行的任务已经不再记录带时刻的写入（`Memory::display_quiet`），只改 `io_`。单步测试的块不进代码池：每个用例编译成一条指令的块，写进共用代码后面的固定位置再发布，下一个用例覆盖它。编译失败或装不下时，这条指令由解释器执行，并按原因计数。

总线。单步测试的块的每个总线周期都经过 gateway 调用平坦总线的 `Bus`，每条指令的预取也一样，顺序和解释器相同。整机的块按 `MappedBus` 的时序运行，翻译方式写在 `arm7_compiler_mapped.cpp` 开头：剩余周期就是 `Memory` 倒数的那个值，用完时在 C++ 里追到当前时刻再继续。读写调用 gateway 里按宽度生成的访存 stub，IWRAM、EWRAM 和 VRAM 在 stub 里完成；预取缓冲停着、由 `Memory` 持有时，对 ROM 已经驻留的页的读取也在 stub 里完成，RAM 和 BIOS 里的代码读 ROM 就是这种情况。其余访问经过 C++ 的慢路径。BIOS、IWRAM 和 EWRAM 的取指周期在编译时算好，IWRAM 和 EWRAM 的块跳到这两个区域时，目标在运行时才知道的 refill 由 gateway 的 stub 计时；ROM 的取指，以及从 ROM 的块跳到同一组等待周期的 ROM 的 refill，按预取缓冲计时：运行期间缓冲的状态借给生成的代码，由 gateway 的取指和 refill stub 推算，到 128KiB 边界附近或者预取关闭时才交还 C++。

降低精度的宏 `GBX_GBA_RELAXED_TIMING`（正式固件默认打开，测试固件默认关闭）让两个后端从 ROM 取指令都不模拟预取缓冲：预取打开时每次取指 1 个周期，关闭时按 S 或 N 访问计时；refill 的两次取指按 N 加 S 计时。解释器从不启动缓冲，JIT 的原生代码运行时缓冲也保持停止。JIT 从 ROM 的块跳到自己的 ROM 区域、IWRAM 或 EWRAM 时，refill 在生成的代码里计时；解释器在 `MappedBus` 里计时，`refill()` 给第二次取指也带上 `kRefill`。宏打开时，编译器还认出只读内存的等待循环（规则和解释器共用 `arm7_spin.h`）：块开头记下剩余周期，连续两圈的周期数相同时，把剩余周期里能走完的整圈一次跳过，只留最后一圈正常执行。gateway 每次调用 C++ 都清掉记录，循环退出时也清掉。宏打开时还简化了三处：中断线不模拟 IE&IF 和 IME 到 CPU 的 2、3 个周期延迟，改动当时就决定，CPU 在下一个指令边界进入中断，在 HALT 里请求的中断在唤醒以后进入；JIT 的块只在写内存的指令后面检查剩余周期，因为写 HALTCNT、中断寄存器或者写到有代码的行都要马上停下，其余指令之间到期的事件推迟到下一次访存或块尾，MSR 和异常返回本来就结束块；混音的采样点固定为 32768Hz，振幅分辨率仍按 SOUNDBIAS。所以宏打开时两个后端进入中断的位置可能不同。宏关闭时两个后端的结果和测试都和不降低精度时相同。

成批运行的做法和 SM83 JIT 相同：相连的块直接跳转，动态目标查分发表。中断这类在块中间停下的运行，可以从块内标志位都已算出的边界接着执行。条件指令两条路径之后的 S 不同时，块的出口也能连接，出口把请求的 S 写回 `Arm7State`。

代码缓存。BIOS、ROM 的块和 RAM 块的副本都在 PSRAM 的第二级留一份可以原地执行的副本。被代码池淘汰的块留在第二级执行，不马上搬回；第二级的块按入口次数计数，用完以后在 C++ 里搬回代码池，每帧最多搬 16 个，搬回一次以后要进入更多次才会再搬。第二级的 RAM 块和代码池里的一样监视写入。

RAM 中的代码。IWRAM 和 EWRAM 按 64 字节的行记录哪里有翻译过的代码，写到这些行时由 C++ 撤销对应的块；写入的值和原来相同时不撤销。RAM 里的块在第二级按标识最多留 4 个版本，连同编译时的代码字节，同一段代码再次出现在 RAM 里时拷回代码池，不重新编译，因为游戏常把几段程序轮流拷进 IWRAM。VRAM、调色板 RAM 和 OAM 中的代码不翻译，由解释器执行，按“区域”计入回退，这样 VRAM 的写入不用检查代码。

### GBA 的内存和设备

`gba/memory.h` 的 `Memory` 是 GBA 的内存映射、IO 寄存器和设备，归 CPU 所在的任务所有。解释器的总线把 IWRAM、EWRAM、BIOS 的访问和经过预取缓冲的 ROM 取指展开在 `src/gba/mapped_bus.h` 里，处理函数一次做完取指令和访存的快路径也在这里，其余访问调用 `Memory` 的慢路径。JIT 的 helper 用的是同一套慢路径，所以两个后端的设备行为只有一份实现。各区域的总线宽度、未对齐访问、open bus 和 BIOS 的读保护按 GBATEK 处理，依据写在 `gba_memory.cpp`。

等待周期和预取。WAITCNT 改变时，`Memory` 重算各区域 N 访问和 S 访问的周期表，CPU 和 DMA 都查这张表。卡带的预取缓冲不逐周期推进：它记下开始取指的时刻，CPU 来取时再按经过的时间算出缓冲里有哪些半字，规则和依据的测试 ROM 写在 `rom_fetch_slow()` 前面。

ROM。`gba/rom_cache.h` 的分页缓存按 32KiB 一页把 ROM 从 SD 卡读进 PSRAM，CPU 的取指和读取、DMA、JIT 的编译和快速路径都经过同一张页表。ROM 不超过缓存的预算时，打开时整个读进来，不会缺页。缺页时读这一页，同时让另一个核上的读卡线程预读下一页。缺页只花墙钟时间，不改变模拟的结果。

设备。中断、HALT 和 STOP、4 个定时器、4 个 DMA 通道、Direct Sound 的 FIFO、串口和按键都在 `Memory` 里，按设备分在 `gba_irq.cpp`、`gba_timer.cpp`、`gba_dma.cpp`、`gba_io.cpp` 等文件。DMA 在启动它的访问或者事件里一次做完，CPU 等它结束。两头都是没有副作用的内存、期间没有事件到期时，连续的单元成批传送；从卡带读取时，只有 S 访问才成批传送。串口只模拟没有对端的传输。

BIOS 和开机。CPU 照常执行 BIOS 的代码，所以中断入口和 SWI 与原机相同。`reset()` 不运行开机流程，直接设置 BIOS 交出控制时的 CPU、IO 寄存器和设备的相位；`reset_to_bios()` 从上电开始运行 BIOS，用来核对这些值。

卡带存档。`gba/backup.h` 模拟 SRAM、Flash 和 EEPROM，类型的判断见[产品设计](product-design.md#gba-存档类型识别)。

卡带的 GPIO 和 RTC（`gba/rtc.h`）。带 RTC 的游戏按 ROM 头的游戏代码登记在 `gba_rtc.cpp` 里，每一条都写着来源。只有载入登记过的游戏时，`Memory` 才模拟 080000C4h 至 080000C9h 的 GPIO 和接在上面的 S-3511。GPIO 的读使能打开以后，ROM 缓存的第 0 页换成一份副本，副本的这几个地址放着 GPIO 寄存器的值，所以解释器和 JIT 的 ROM 快速路径照常读这一页，不用为 GPIO 加检查；写入本来就走 ROM 区的慢路径，在那里按地址交给 GPIO。RTC 按模拟时间计时，只在被访问时追到当前时刻，不安排事件，也不读宿主时间。RTC 的中断没有模拟，原因写在 `rtc.h`。

### 双核分工

CPU、调度器、设备时序和 JIT 由一个固定的核独占。每个系统在另一个核上有一个工作线程（`gb/offload.h`），通过两个固定容量的单生产者单消费者队列分别接收 PPU 的扫描线任务和 APU 的声音任务，一种任务积压时不会挡住另一种。队列满时 CPU 所在的核等待，不丢弃任务，交接不加锁。

GBA 用同一个工作线程类，也分扫描线任务和声音任务两个队列。扫描线任务的数据放在一个环形缓冲里，CPU 所在的核只在要覆盖的数据还没有被渲染时才等待（`gba/ppu.h`）。渲染器跟得上 CPU 时工作线程几乎不停，同一个核上的空闲任务得不到运行，任务看门狗会报错，所以工作线程每画完一帧，离上次让出满 1 秒就让出 1ms（`gba_ppu.cpp`）。GBA 的工作线程优先级是 1，低于呈现任务、LVGL、读卡任务和存档任务，渲染器占满它的核时这些任务仍然能运行；它的环形缓冲能放下一帧以上的行，这些任务运行的几毫秒里 CPU 所在的核不用等。GB 的工作线程仍然是 5，因为它的队列只有 16 行。

平台接口 `gbx/worker.h` 提供工作线程和核间通知。ESP 实现把工作线程固定在 CPU 所在核以外的核上，PC 实现使用普通线程，任务格式、渲染和混音代码共用。

ESP32-C5 只有一个核，固件用 `CONFIG_FREERTOS_UNICORE` 构建，所有任务都在 core 0 上。这时没有工作线程（`GBX_SINGLE_CORE`，见 `gb/offload.h`）：模拟任务交出扫描线任务或声音任务时就地处理掉，PPU 的行队列和 APU 的声音队列各只留 2 项。单核上没有另一个核并行，工作线程只会多出任务切换，它的队列还要占 PSRAM。模拟任务的优先级降到 1，低于 LVGL、呈现任务和读写卡任务，所以模拟跟不上时触摸、屏幕和读卡仍然能运行。优先级和理由写在 `components/app/src/game.h`。

### PPU 渲染

扫描线任务带着这一行用到的寄存器、变化过的显存页和 OAM，以及这一行里带时刻的修改。渲染只读任务和工作线程自己持有的副本，不能读取 CPU 正在修改的状态。

PPU 按扫描线渲染。没有中途修改的行一次画完；有修改时，按修改发生的 dot 在取图和像素输出的各个阶段分别采样，不逐点运行像素 FIFO。各寄存器从哪个阶段开始生效，依据写在 `gb/renderer.h`。mode 3 的长度由 CPU 所在的核按公式计算；mode 3 中途移动了窗口或 OBJ 取图时，用渲染时的同一份时间线（`make_timeline()`）修正，所以 mode 0 的时刻和画面一致。

GB 模式输出灰阶等级，CGB 的两种模式输出 BGR555。

GBA 的 PPU（`gba/ppu.h`）在 CPU 所在的核上计算 VCOUNT、DISPSTAT、中断和 DMA 的时刻。每个可见行的任务在这一行的 HBlank 交出，带着行首的寄存器、行首以前写过的 VRAM、调色板和 OAM 的页，以及这一行可见部分里带时刻的寄存器写入。VRAM 和 OAM 按 512 字节一页记录和复制，调色板按 32 字节（16 色）一页，因为游戏常在行间改调色板；即时存档里仍然按 512 字节一页保存，保存和恢复时转换。位图模式 4 和 5 里，没有显示的那一帧写过的页先留着，等到显示它的第一行再交出，因为游戏往不显示的那一帧作画时，同一页会在很多行里被反复写到。渲染器（`src/gba/renderer.h`）先建好这一行的 OBJ，按优先级各记一张位图，半透明 OBJ 和 OBJ 窗口的像素另记位图，一行里只在写了 DISPCNT 或 MOSAIC 以后才重建剩下的部分。然后按段处理一行，每段都按优先级从后往前画，BG 一次处理一个 tile 的一行：一段里没有颜色特效时，直接画进输出的行。有特效时，能在画的过程中算出结果的也直接画：亮度变化只看最上面一层；只有一个第一目标 BG、其余的层都是第二目标时，第一目标下面一定是第二目标；第一目标和半透明 OBJ 都不是第二目标时，它们混合的对象一定是原色，这时每个像素只记下当前最上面一层是不是第二目标，画到要混合的层时就混合。其余情况（包括 OBJ 窗口逐像素决定效果时），每个像素记下最上面两层的颜色和下面一层是不是第二目标，画完再统一做 alpha 混合或亮度变化。开着 WIN0 或 WIN1 时，按窗口的左右边界把一段分成几个区段，每个区段只画窗口显示的层，因为区段里显示的内容不变；这一行有 OBJ 窗口的像素时才逐像素判断窗口。一行中途写入的寄存器从写入时的那个点开始生效。有显示的缓冲时，渲染器把每一行转成屏幕的 RGB565 直接写进显示的缓冲，只写这一次，并记下每一行最新的内容在哪个缓冲里；显示的缓冲在 PSRAM，画一行的同时用数据缓存的预取（`platform::preload_external()`）把下一行的位置先取进缓存。即时存档、暂停画面和测试读 BGR555 的帧时，先把只在显示缓冲里的行转回来。

### APU

`gb/apu.h` 把 CPU 能观察到的声音状态留在 CPU 所在的核上，比如 NR52 的声道位、length timer 和 sweep，CPU 访问时追到当前时刻。波形、envelope、混音和重采样由另一个核上的混音器（`gb/mixer.h`）按声音任务完成。混音结果不回流到 CPU 所在的核，所以 PCM 只由写入和写入的时刻决定，和任务怎样划分、另一个核什么时候处理无关。CGB 的 PCM12 和 PCM34 只有混音器算得出来，CPU 读它们时要等混音器处理完已经交出的任务。

GBA 的声音按同样的方式划分（`gba_sound.cpp` 和 `src/gba/mixer.h`）。CPU 所在的核保留 CPU 能观察到的状态：SOUNDCNT_X 的声道位、length timer、sweep 和 512Hz 的步进，Direct Sound 的 FIFO，以及 FIFO 向 DMA 发出的请求。影响声音的寄存器写入和 FIFO 取出的样本连同时刻放进声音任务，另一个核上的混音器据此合成 4 个 PSG 声道，按 SOUNDCNT_L、SOUNDCNT_H 和 SOUNDBIAS 混音，再重采样成调用者要的采样率。GBA 的混音器是单独写的，因为它的寄存器和波形 RAM 和 GB 不同，还多了 FIFO 和 PWM 输出这一级；分开写也让 GB 的 PCM 和速度保持不变。

### 系统接口

系统接口在 `gb/system.h`，提供加载、复位、选择后端，以及运行到指定时间或下一个帧边界。按时间运行会做完跨过目标时刻的那条指令。ROM 由调用者持有，在系统使用期间一直有效。

- 按键：由 CPU 的拥有者提交。
- 串口：只支持内部时钟、没有对端的传输。
- 画面：设置了 `FrameSink` 时，工作线程直接交付 BGR555 帧，`FrameSink::shades()` 为真时 GB 模式的帧按灰阶交付；否则由读取方取最近一帧。CGB 的帧直接画进 `FrameSink::drawing_buffer()` 给的显示缓冲，交付时不用再复制一遍。那块缓冲里还放着最近完成、没有交出去的一帧时，渲染器就画进自己的缓冲，交付时再复制。即时存档和暂停画面读的都是最近完成的一帧，不管它在哪块缓冲里。
- 声音：按调用者给定的采样率返回 16 位立体声 PCM，每秒生成的帧数可以微调，供按音频水位控制节奏的调用者使用。
- RTC：取出或写回完整状态，或者加上一段经过的时间。
- 卡带存档：取出、写回卡带 RAM，并报告上次查询以来可能写过的 bank 和时钟寄存器。
- 即时存档：`capture()` 和 `restore()`，见下一节。

GBA 的系统接口在 `gba/system.h`，提供初始化、载入 BIOS 和打开的 ROM 缓存、复位、选择后端，以及运行到指定时间或下一次 VBlank。按键、画面、声音、RTC 和即时存档的接口和 GB 的用法相同。卡带存档按 8KiB 一段报告上次查询以来可能写过的部分；存档类型可能要等程序第一次访问才确定，所以调用者查询时也取类型和大小。BIOS 和 ROM 缓存由调用者持有。

### 即时存档的状态

`gb/snapshot.h` 的 `Snapshot` 保存一个指令边界上的完整机器状态，包括调度器的事件、各块内存、卡带，以及另一个核上渲染线程和混音器的状态，从这个状态接着运行的结果和不中断运行相同。状态里没有后端的东西，所以任何一个后端都能恢复另一个后端存的状态。

模拟所在的核只做复制：`capture()` 和 `restore()` 先等工作线程处理完已经交出的任务，再在预先分配的外部内存里复制状态；`encode()` 和 `decode()` 可以在别的任务上运行，可以一次处理整份编码，也可以经一块缓冲分段进行，这样内存里不必放下整份编码。编码带格式版本、系统、ROM 标识和校验，字段布局写在 `gb/snapshot.cpp` 开头。字段有任何变化都要升版本，其他版本的状态一律拒绝。恢复以后，JIT 丢掉 RAM 里代码的块，ROM 里代码的块保留。

GBA 的状态在 `gba/snapshot.h`，做法和 GB 的相同。状态包括 CPU 各模式的寄存器和流水线、预取缓冲、调度器的事件、各个设备、各块内存、存档芯片、GPIO 和 RTC，以及另一个核上渲染器和混音器的状态，正在画的那一行的任务也在里面。BIOS、ROM 和 ROM 缓存不在状态里，所以恢复时 ROM 标识要和当前的游戏相同。编码和 GB 的用同一个文件头，靠系统字段区分，字段布局写在 `gba_snapshot.cpp`。恢复以后，JIT 丢掉 IWRAM 和 EWRAM 里代码的块，BIOS 和 ROM 里代码的块保留。

### 追踪

详细追踪使用 `common/trace.h` 的固定容量单生产者单消费者缓冲。解释器换一套总线实现来采集，JIT 重新生成带探针的代码，块划分、寄存器分配和快速访存都和正常运行相同。模拟核心只写定长记录，由另一个核消费和比较。丢过记录的执行不能判为通过。

## JIT 后端

`components/jit/` 不解释原机指令，只提供 RV32 的编码、代码缓存和可执行内存的管理。`assembler.h` 在调用者提供的缓冲区里生成代码，容量不足或编码无效时不发布半个块。生成的代码在有 16 位形式的地方都用压缩指令。

扩展指令按芯片修订启用。Zba、Zbb、Zbs 只在 P4 v3.x 的构建里使用，开关是 `rv32::kZb`，因为 v1.3 执行它们会触发非法指令异常。核对见[任务 5-2](plans/gb/task-5-2.md#扩展指令的核对)。

`cache.h` 管理片内的代码池和块索引，块标识和退出原因由前端定义。代码池前面可以留出一段放前端的共用代码，保证块用 `jal` 调得到。回收只在块外进行，不移动存活的代码。`store.h` 是 PSRAM 里的第二级。PSRAM 离片内 RAM 超出了 `jal` 的范围，所以第二级按 1MiB 分段，每段开头放一份共用代码，块不跨段。

外部可调用的入口遵守 RISC-V psABI。前端在块内可以用自己的约定，但必须通过公共入口和出口保存、恢复宿主状态，约定写在 `cache.h`。PC 版能编译后端并检查编码，但不运行 RV32 代码。

可执行内存的接口在 `gbx/executable.h`。ESP 实现默认把代码池放在片内 RAM，第二级放在 PSRAM；`CONFIG_GBX_JIT_CODE_PSRAM` 把代码池也放进 PSRAM，腾出的片内 RAM 可以配给更大的 L2 缓存，默认不打开。公共 `sdkconfig.defaults` 关闭了 `CONFIG_ESP_SYSTEM_MEMPROT`，否则 JIT 不能写入并执行片内代码池。这会取消 IDF 默认的代码区、数据区权限划分，依据见 [IDF 配置说明](https://github.com/espressif/esp-idf/blob/fff9895c82d744c7237be8847347bdd1b07c6643/components/esp_system/Kconfig)和 [P4 保护实现](https://github.com/espressif/esp-idf/blob/fff9895c82d744c7237be8847347bdd1b07c6643/components/esp_hw_support/port/esp32p4/cpu_region_protect.c)。

P4 上发布代码时先写回 L1 数据缓存，再使 L1 指令缓存失效，并执行本核的 `fence.i`。S31 的片内 SRAM 不经过缓存，只做指令流同步；PSRAM 里的代码和 P4 一样先写回、再失效。S31 没有 L2，取指令在 L1 未命中时直接读 PSRAM，所以写回到 PSRAM 就够了。C5 的片内 SRAM 也不经过缓存。它只有一个指令和数据共用的缓存，取指令读到的就是写进缓存的内容，所以 PSRAM 里的第二级也能直接执行。实现和加锁的约定写在 `components/platform/src/esp/executable.cpp`。代码生成、发布和执行必须由固定在同一核的任务负责，执行期间不能改写指令，发布期间不能并发执行 flash 操作，见 [ESP-IDF 缓存同步接口](https://github.com/espressif/esp-idf/blob/fff9895c82d744c7237be8847347bdd1b07c6643/components/esp_mm/include/esp_cache.h)。双核之间交接代码还没有实现，不能把缓存失效当成代码生命周期的同步。

## 板子

添加板子的步骤见[加板指南](adding-a-board.md)。现有板子包括 [JC1060P470](boards/jc1060p470.md)、[M5Stack Tab5](boards/m5stack_tab5.md)、[OSPTEK ESP32-P4C5-Module](boards/osptek_p4c5.md)、[Sila P4](boards/sila_p4.md)、[Sila P4X AIBox](boards/sila_p4x.md) 、ESP32-S31 的[慧勤智远 ESP32-S31 CB](boards/wks_s31_cb.md)、[乐鑫 ESP32-S31-Korvo-1](boards/esp32_s31_korvo_1.md)和 ESP32-C5 的 [Waveshare ESP32-C5-Touch-LCD-2.8](boards/waveshare_c5_lcd28.md)。

芯片的差异和板子的差异分开处理。芯片的差异放在 `components/platform/src/esp/`，按 ESP-IDF 的 SOC 能力宏处理，包括可执行内存和缓存同步、双核任务和显示提交。P4 和 S31 都有 PPA 与 2D-DMA，不能用是否有 PPA 区分这两种芯片。

显示后端在构建时按 SoC 选择，开关是 `CONFIG_GBX_DISPLAY_ACCEL`，配置在 `sdkconfig.defaults.<target>`。ESP32-P4 和 ESP32-S31 用 PPA 和 2D-DMA 的加速后端（`display_accel.cpp`），默认缩放为 SMOOTH；ESP32-C5 这类没有它们的目标用 CPU 后端（`display_cpu.cpp`），只有 NEAREST，构建里不能引入 PPA 和 2D-DMA 的依赖。两种后端不会一起编入固件。C5 的 `sdkconfig.defaults.esp32c5` 还打开了 `GBX_UI_STATIC_SHELL`，因为 UI 和模拟在同一个核上：游戏页的控件开机画好以后不再随触摸改变外观，只有速度拨杆会动，也不画 VFD。S31 的 RGB 屏一边扫描一边由 DMA 从 PSRAM 读帧缓冲，LVGL 画界面和 PPA 画游戏帧同时进行时，DMA 会取数不及，所以 `sdkconfig.defaults.esp32s31` 打开 `CONFIG_GBX_DISPLAY_LOCK_GAME_DRAW`：present 任务画游戏帧时也拿着 LVGL 锁。

板子的差异放在 `boards/<板子>/`，描述板上焊了什么：

- 引脚、屏幕和初始化序列、触摸、codec 和 RTC 的型号、按键的接法、电池 ADC 和 LDO 通道。接口声明在 `components/board/include/gbx/esp/board.h`，各板子相同的步骤在 `components/board/src/board_support.cpp`，其中 MIPI DSI 的部分只在有 DSI 的芯片上编译。显示呈现、触摸坐标换算和 SD 挂载由平台实现统一处理。SD 卡接在 SDMMC 控制器上，或者和 SPI 屏共用一条 SPI 总线，由 `board::sdmmc()` 说明；共用总线时，板子在 `init()` 里初始化总线，平台只在上面加一个设备。
- `sdkconfig.defaults`，写芯片、芯片修订、PSRAM、flash 和主频。P4 v1.x 和 v3.x 要用不同的芯片修订配置编译，所以这些配置跟着板子走。USB host 口能给手柄供 5V、手柄也在这个口上用过的板子，在这里打开 `GBX_USB_GAMEPAD`，见“显示和触摸”。
- 用到的乐鑫组件在 `idf_component.yml` 里列出并锁定版本。

几块板子都可能用到的外设驱动放在 `drivers/`，可以使用乐鑫的组件，因为 AGENTS.md 要求从零编写的只是模拟核心、解释器、JIT 和设备模型。板子的资料整理成 `docs/boards/<板子>.md`，原始资料放在 `refs/boards/<板子>/`。

PC 版也按一块板子处理，平台实现是 `components/platform/src/host/`。

## 显示和触摸

界面用 LVGL 绘制，游戏画面不经过 LVGL，两者分开刷新。

屏幕的帧缓冲由 `components/platform/src/esp/` 的呈现任务独占，其他代码不能直接写帧缓冲或调用 `esp_lcd_panel_draw_bitmap()`。LVGL 在 PSRAM 里画整屏的 UI，呈现任务只把变化的区域复制进后台缓冲，需要时同时旋转，再在帧之间切换缓冲，所以画面不撕裂，也不用每次重传整屏。MIPI DSI 屏用 ESP-IDF 的 DPI 驱动，RGB 并口屏用 RGB 驱动，`board::display_init()` 返回的 `PanelDriver` 告诉呈现任务从哪个驱动取帧缓冲和换帧事件，两种屏的呈现流程相同。SPI 屏自己的显存保存画面，所以呈现任务在 PSRAM 里只留一个帧缓冲，照样更新变化的区域，再把变化的行发给面板，等传输读完这些行才继续改帧缓冲。面板没有接 TE 信号时，可能显示更新到一半的画面。不旋转的 SPI 屏上，游戏画面不经过帧缓冲：呈现任务在片内 RAM 的两段缓冲里逐段转换画面的行，一段在发送时转换另一段。因为 ESP32-C5 的 PSRAM 是四线的，画面先写进 PSRAM 里的帧缓冲、写回缓存，再由 DMA 读出来，会多占很多 PSRAM 带宽。整幅画面的行先用 CASET 和 RASET 定好窗口，第一段用 RAMWR 发送，后面的段用 RAMWRC 接着写（ST7789V2 数据手册 9.1.33），每段只有一次传输。所以面板控制器要支持 MIPI DCS 的 write_memory_continue，板子也不能给面板设偏移（gap）。`GBX_DISPLAY_GAME_HALF_RATE` 让呈现任务每 25ms 最多取一帧游戏画面，原速时只显示一半的帧；游戏在每帧开始时用 `game_frame_due()` 估计这一帧交出时会不会被取走，交出的时刻按最近一个画了的帧花的时间估算，不会被取走的帧整帧不画。画了的帧都交出，交得早了就等呈现任务到时间再取。C5 打开了这一项，理由写在 `components/platform/Kconfig`。

游戏画面通过 `display.h` 的游戏层交给呈现任务。生产者发布最新的一帧，呈现任务只取最新的，旧帧跳过。游戏层的三个帧缓冲在显示启动时按编入的系统里最大的画面分配，有 GBA 时是 240×160，只编入 GB 时是 160×144，以后不再释放，因为生产者随时可能拿着其中一个；如果等到第一个游戏才分配，它们会落在这个游戏的块中间，游戏退出以后把 PSRAM 切开。呈现任务把 BGR555 转成面板格式，按整数倍放大写进游戏窗口。不旋转的 SPI 屏分段发送时也接受 GB 模式的灰阶（`PixelFormat::shades`，每像素一字节），呈现任务按当前调色板的四种颜色转换，渲染器就不用先把灰阶转成 BGR555 写进 PSRAM；其他显示不接受，`game_shades_supported()` 返回 false。`NEAREST` 和 `SMOOTH` 的窗口、尺寸和黑边相同。这一步转换可以借用模拟所在核的空闲时间，由 `Game` 决定，理由写在 `components/app/src/game.h`。

颜色校正也在这一步转换里查表，把 BGR555 直接换成面板的 RGB565，两个显示后端和 PC 版共用这张表。表由 `components/app/src/color_fix.cpp` 生成，参数由 `tools/lcd_colors.py` 从一台 GBC 屏幕的实测光谱算出。校正只在呈现时进行，所以测试比对的帧和即时存档的缩略图都是未经校正的颜色。

LVGL 对象只能在 LVGL 任务里，或者持有 `display::lock()` 时操作。LVGL 的 PPA 绘制单元和游戏画面的呈现分开配置。菜单由 `MenuPainter` 在 CPU 上画成画布。

PC 版的游戏层由 `components/platform/src/host/display.cpp` 实现，见 [PC 图形界面方案](host-gui.md)。

触摸不经过 LVGL 的输入设备，因为它只支持一个触点，而游戏要多指同时按。命中规则见 [UI 布局的“触摸”一节](ui-layouts/index.md#触摸)。

实体按键由 `display.h` 的 `keys::read()` 提供，和触摸控件的按键合在一起。固件上的实体按键目前只有 USB 手柄：打开 `GBX_USB_GAMEPAD` 的板子在 USB host 口上读 HID 游戏手柄，乐鑫的 USB Host 库只在这个选项打开时进入构建。USB 的中断和两个任务都在 core 0，模拟所在的 core 1 不参与；任务栈在 PSRAM，片内 RAM 留给 JIT。设备的打开、报告描述符的读取、报告的接收和拔出后的清理都在同一个客户端任务里按库的事件顺序处理，所以设备拔出时没有别的任务还在用它。按键来自报告描述符里的 hat switch、X、Y 和按钮编号，A、B 按位置对应，一次只用一个手柄。映射表和两个任务的分工写在 `components/platform/src/esp/usb_gamepad.cpp`。

## 运行游戏

`components/app/` 的 `Game`（`src/game.h`）一次运行一个游戏，固件和 PC 版用同一套代码。各任务所在的核和优先级写在 `src/game.h`。

- 模拟任务独占当前游戏的 `gb::System` 或 `gba::System`，固件上固定在 core 1。退出游戏和载入另一个游戏之前，`Game` 释放上一个游戏的系统、ROM、ROM 缓存和 BIOS，所以两种系统不会同时存在。如果留下这些块，新游戏的分配会被它们切开，因为 ROM 缓存要一整块连续的 PSRAM。JIT 代码池用的片内 RAM 一直留着，见“代码规则”。模拟任务负责读卡、检查卡带头、加载 ROM 和选后端，按键、暂停和切换后端都在帧边界处理。启动 GBA 游戏之前，`Game` 核对卡上 BIOS 的 SHA-256，没有 BIOS 或者校验不符时不启动。
- 画面：`Game` 实现两种系统的 `FrameSink`，PPU 的工作线程直接把帧写进显示的游戏层，模拟任务不转换也不复制画面。单核上没有工作线程，由模拟任务写。
- 声音：混音器在工作线程上生成 PCM，单核上在模拟任务上生成，core 0 上的音频任务把它交给平台的 `gbx/audio.h`。ESP 实现写 I2S，PC 实现用 SDL3 的音频流，欠载由平台实现计数。
- 节奏：已提交但还没有播放的音频总量低于目标水位时，模拟任务运行一帧。水位持续偏低时，略微调高每秒生成的 PCM 帧数，所以小的速度不足不会马上变成断音。没有声音输出时按墙钟的帧周期运行。节奏只决定模拟什么时候运行，不改变模拟的时间线和结果。
- 2 倍速：在帧边界切换。2X 关闭声音，按墙钟控制节奏，屏幕显示不了的帧不渲染。不画的帧不改变模拟结果。GB 是因为 mode 3 的时序由 CPU 所在的核自己计算；GBA 不画的帧不交出扫描线任务，这期间写过的 VRAM、调色板和 OAM 的页以及寄存器，随下一个要画的行一起交出。回到 1X 时，GBA 把 2X 期间 FIFO 的当前样本补交给混音器。
- 指标：模拟任务和工作线程在帧边界记下计数，界面所在的任务每秒算一次游戏页的指标，口径见[调试设计](debug-design.md)。
- 暂停：声音停下，继续时按暂停的时长补算卡带的时钟。

串口控制台在 `main/console.cpp`，命令见[调试设计的“串口控制台”](debug-design.md#串口控制台)。

## 菜单、设置和存档

菜单的状态和导航在 app 的 `Menus`（`src/menus.h`）里。`components/ui/` 的 `MenuView` 只按 app 给的 `MenuScreen` 描述画画布，再把触摸换算成行和按钮交回 app。这样划分是因为菜单操作的游戏、存档和设置都在 app 里，ui 不需要知道它们。界面和操作见 [UI 布局的“菜单”一节](ui-layouts/index.md#菜单)。

app 里另外几部分：

- `Library`（`src/library.h`）：扫描卡上的 `roms/`，结果分批发布。ROM 列表和载入用同一个判断 `Cartridge::supports()`。
- `Settings`（`src/settings.h`）：读写 `config/<板子名>.ini`，规则见[产品设计](product-design.md#sd-卡目录)。
- `Faults`（`src/faults.h`）：ERR 灯后面的故障列表。
- `Saves`（`src/saves.h`）：卡带存档，文件格式写在 `saves.cpp` 开头。
- `Slots`（`src/slots.h`）：即时存档的槽位，文件格式写在 `slots.cpp` 开头。
- `Sounds`（`src/sounds.h`）：菜单的提示音，由 `Game` 的音频任务播放，游戏运行时叠加进游戏的 PCM。

扫描和写卡的任务都在 core 0，优先级低于音频、工作线程、呈现任务和 LVGL，不在模拟所在的核上，所以模拟任务不等读写卡。

卡带存档：模拟任务在帧边界查询这一帧可能改过的卡带 RAM，有变化就复制进副本，状态变成 DIRTY。写卡任务按[产品设计](product-design.md#卡带存档)的规则写回。存档里记着写入时板上 RTC 的时间，载入时用来补算关机期间经过的时间；两个时间有一个不可信，或者时间倒退了，就不补算，提示 CLOCK NOT SET。GBA 的存档文件还记着存档类型和容量，载入时交给系统；类型要等程序第一次访问才确定的游戏，确定以后才有东西写回。

即时存档分在三层。core 的 `Snapshot` 只管状态的复制、编码和校验，不碰文件；app 的 `Slots` 管槽位文件和写卡，`Game` 在模拟任务里于帧边界取出和恢复状态，`Menus` 管 SAVE / LOAD 页面和提示框的流程；ui 只按 `MenuScreen` 的描述画槽位列表。写卡期间游戏可以继续运行。读档以后，卡带 RAM 和时钟都当作改过，由卡带存档写回。

`Slots` 经一块 16KiB 的缓冲分段写卡和读卡，内存里只有状态的副本，不放整个槽位文件。缩略图也只留一张：菜单选中哪个槽位，就从卡上读出它的缩略图，正在写或刚写完的槽位用写入时的那一张。

## 构建

仓库里的 PNG 和 PDF 用 Git LFS 保存，规则写在 `.gitattributes` 里。clone 之前要先安装 Git LFS，并运行一次 `git lfs install`，否则检出的图片只是几行文本的指针文件。

仓库根目录是 ESP-IDF 工程，用 `CMakePresets.json` 里的 preset 选择板子、构建类型和要编译的程序：

```
eim run "idf.py --preset jc1060p470-debug build" v6.1
eim run "idf.py --preset jc1060p470-debug -p <端口> -b 460800 flash monitor" v6.1
```

正式固件的 preset 叫“板子-构建类型”，比如 `jc1060p470-release`；测试固件叫“板子-test-构建类型”，比如 `jc1060p470-test-debug`。`板子-test-profile` 和 `板子-profile` 是带 `GBX_PROFILER` 的 release 测试固件和正式固件，只在做性能分析时构建，见[真机测试计划](hardware-testing.md#性能分析版固件)和[调试设计](debug-design.md#串口控制台)。每个 preset 有自己的 build 目录 `build/<preset>/`，生成的 sdkconfig 也放在这里，不入库。sdkconfig 按下表顺序合并默认配置，后面的覆盖前面的：

| 文件 | 内容 |
|---|---|
| `sdkconfig.defaults` | 所有板子和构建类型共用的配置 |
| `sdkconfig.defaults.<target>` | SoC 共用配置，例如 ESP32-P4 的显示加速策略 |
| `sdkconfig.defaults.debug`、`sdkconfig.defaults.release` | 构建类型：优化级别、断言、日志级别、启动时的 PSRAM 自检 |
| `sdkconfig.defaults.profile` | 只有 `板子-profile` 和 `板子-test-profile` 加载，恢复 release 关掉的 PSRAM 自检 |
| `boards/<板子>/sdkconfig.defaults` | 芯片、芯片修订、主频、flash 和 PSRAM |

要保留的配置改动写回对应的 defaults 文件。已生成的 sdkconfig 会保留已有取值，修改 defaults 后要确认各 preset 的实际配置。

公共配置里有两项不能随意改。UART 控制台和测试传输统一用 460800 baud，修改时要同时更新 bootloader。FreeRTOS 的 tick 是 1000Hz，不要改回 100Hz，那样驱动里 5ms 这类短延时会被截成 0，屏幕和触摸的复位时序就不够了。

PC 版是 `host/` 下单独的 CMake 工程，preset 在 `host/CMakePresets.json` 里。默认构建 GUI，需要 uv，首次配置会下载固定版本的 SDL3 和 LVGL；`headless-*` preset 是不依赖图形库的无窗口版：

```
cd host
cmake --preset debug
cmake --build --preset debug
```

`components/` 下除了 `board/`，各组件的 CMakeLists 都写成两用：在 ESP-IDF 里注册成组件，在 PC 上是普通的静态库。

各板共用 `partitions.csv`，布局只用前 16MB flash。代码和 UI 素材在不同的分区，可以分开烧录：只改素材时用 `idf.py assets-flash`，只改代码时用 `app-flash`。资源包由 `tools/ui/pack_assets.py` 在正式固件构建时生成，见 [UI 布局](ui-layouts/index.md#资源包)。

## 测试

测试在连着电脑的板子上运行，数据常驻 SD 卡。选择范围、判据和运行方法见[真机测试计划](hardware-testing.md)。

- `tests/runner/` 读取套件并选择用例，确认板上数据的版本，保存结果和报告。单步套件的选择、传输、报告和离线复判由 `single_step.py` 处理，SM83 和 ARM7TDMI 各自提供读取套件、编解码和判据的部分。
- `tests/device/` 是板上的测试固件，从 SD 卡读取用例，在两个后端上分别执行并比较，回传结果。
- `tests/protocol/` 定义电脑和板子共用的传输和执行协议，只维护最新版本。
- `tests/gba/` 放自写的 GBA 测试 ROM 的源码和参考帧，ROM 构建以后放进卡上的测试数据。
- `tests/sdcard/` 是卡上 `tests/` 目录在电脑上的副本，由 `tools/prepare_gb_tests.py` 和 `tools/prepare_gba_tests.py` 生成，不提交。

解释器的结果必须通过外部套件验证，JIT 和通过验证的解释器比较实际结果；两个后端结果相同不能当作独立的正确性依据。

PC 版只有解释器，用于开发和调试，不代替板上验收。图形界面见 [PC 图形界面方案](host-gui.md)，`host/tests/` 只检查桌面 UI、输入和退出流程。

## 代码规则

代码使用 gnu++23，具体约束如下：

- 固件和 PC 版都用 `-fno-exceptions -fno-rtti` 编译。固件上 libstdc++ 的 nothrow `operator new` 分配失败时会在抛异常的地方 abort，不返回 nullptr，所以 `components/platform/src/esp/platform.cpp` 用 `malloc()` 和 `memalign()` 替换了它的 nothrow 版本。
- 模拟循环里不分配堆内存，需要的内存在加载 ROM 时分配好。
- 每条指令、每个周期都会走到的路径上不用虚函数。系统接口这种每帧才调用一次的地方可以用。
- 不用 iostream，日志经过平台接口输出。
- 用模板展开专用版本之前，先测代码体积，因为 P4 的 L1 指令缓存只有 16KB。
- ESP-IDF 的 API 只在 `main/`、`components/platform/src/esp/`、`components/board/`、`boards/`、`drivers/` 和 `tests/device/` 里调用。

热代码的位置由链接配置固定，固件和测试固件共用同一套规则。`components/core/linker.lf` 和 `components/jit/linker.lf` 把 GBA 的解释器、设备执行代码和 JIT 运行时 helper，以及两个系统共用的块查找和工作线程队列映射到片内 RAM，其中只在载入、复位或单步测试时运行的函数逐个列出，留在 flash。`tools/link_core_layout.py` 再按固定顺序排列这些对象：共用的代码在前，GBA 的执行代码在后。这样测试、应用和 JIT 编译器的改动不会移动热代码。GBA 比 GB 难跑到原速，所以两个系统争片内 RAM 或位置时 GBA 优先：GB 的整个执行核心都留在 flash，从 PSRAM 运行。

固件编入哪些系统由 `components/core/Kconfig` 的 `GBX_SYSTEMS` 选择，代码按 `gbx/systems.h` 的 `GBX_HAS_GB` 和 `GBX_HAS_GBA` 区分。S31 的片内 RAM 放不下两个系统的执行核心，所以 `sdkconfig.defaults.esp32s31` 默认只编入 GB（`GBX_SYSTEMS_GB_ONLY`），不编入 GBA 的核心和 JIT，GB 的解释器、设备执行代码、运行循环、JIT 的派发和运行时 helper 放进片内 RAM。`wks_s31_cb` 和 `esp32_s31_korvo_1` 在各自的 `sdkconfig.defaults` 里改成只编入 GBA，并设定下面说的 GBA 的片内和 PSRAM 预算。只有应用或测试调用、每帧最多调用一次的入口也留在 flash，因为链接器会删掉一种构建没有引用的函数，热代码的位置就会随之改变。JIT 的编译、安装和块的撤销放在 `Sm83Jit::install()` 和 `retire_containing()` 里，和编译器一起留在 flash，编译器的改动就不会改变 `Sm83Jit::execute()` 的大小。热代码查的表放在全局指针的寻址范围下面。链接器会把范围里的表的寻址缩成一条指令，表在不在范围里取决于应用和测试的数据，所以留在范围里，代码的大小就会跟着它们变。只编入 GB 时，即时存档的缩略图、菜单和显示里放游戏画面的缓冲都按 GB 的 160×144 分配。只编入 GBA（`GBX_SYSTEMS_GBA_ONLY`）时，菜单只列 GBA 游戏，GB 的核心仍然编入，留在 flash。GB JIT 第二级和 GBA ROM cache 在 PSRAM 里的预算由 `GBX_GB_JIT_STORE_KB` 和 `GBX_GBA_ROM_CACHE_KB` 设定，各板在 `boards/<板子>/sdkconfig.defaults` 里按自己的 PSRAM 设值。`GBX_GB_JIT_STORE_LEAVE_KB` 不为 0 时，第二级等游戏的存档和即时存档分到内存以后才分配，取剩下的 PSRAM，最多到 `GBX_GB_JIT_STORE_KB`，并且留出这么多，给游戏运行时菜单和存档的分配。ROM 列表在 PSRAM 里的大小由 `components/app/Kconfig` 设定。C5 用了这两项。GB JIT 代码池在片内 RAM 里的大小由 `GBX_GB_JIT_POOL_KB` 设定，片内 RAM 不够的板子调小它，比如 C5。GBA JIT 的代码池、第二级和第二级留出的 PSRAM 分别由 `GBX_GBA_JIT_POOL_KB`、`GBX_GBA_JIT_STORE_KB` 和 `GBX_GBA_JIT_STORE_LEAVE_KB` 设定，含义和 GB 的三项相同，代码池的块数按每 192 字节一块算。片内 RAM 放不下 GBA 解释器的板子可以打开 `GBX_GBA_INTERPRETER_IN_FLASH`，让解释器（`arm7_mapped`）留在 flash，空出来的片内 RAM 给 JIT 的代码池。JIT 只在单步和不翻译的代码上调用解释器，所以这样做主要是解释器后端变慢。PSRAM 紧张的板子可以打开 `components/ui/Kconfig` 的 `GBX_UI_PACK_IN_FLASH`，游戏页的 VFD 直接从 flash 里的资源包读字模和格子底下的外壳，不在 PSRAM 里复制一份。P4 v3 以前的芯片把片内 RAM 分成两段，IDF 链接的代码和数据只能放在前一段，装不下 GBA 全部的执行代码。所以跑在 CPU 所在核上的执行代码放在后一段的数据区，和 JIT 代码池所在的堆共用这一段。另一个核上的渲染器和混音器放得进前一段，就留在前一段，这样后一段的堆能多出这部分空间给代码池。P4 上不在片内的代码都从 PSRAM 经两个核共用的 L2 缓存读取，热代码放在片内，另一个核上的任务对模拟速度的影响就小一些。函数改名或删除以后，要同步改 linker.lf 和脚本里的名单；执行代码改动以后，检查 map 里的地址，并上板测量布局对性能的影响。

片内 RAM 还留给每次访存都要用的 `gb::System` 和 `gba::System`、JIT 的代码池和块索引，模拟、渲染、音频和呈现任务的栈，中断处理函数和驱动要求的 DMA 缓冲。JIT 代码池要一整块连续的片内空间，所以访问不频繁的代码和数据放进 PSRAM：app 的全局对象由 `components/app/linker.lf` 放置，冷的静态缓冲用 `EXT_RAM_BSS_ATTR`，只处理文件、菜单或串口命令的任务和 LVGL 任务把栈放在 PSRAM，LVGL 的堆优先分配在 PSRAM。GBA 的 BIOS 也由 `Game` 分配在 PSRAM：JIT 只在编译 BIOS 的代码时整段读它，运行时只读 BIOS 里的数据和设 latch 用的字，但是解释器执行 BIOS 时每次取指令都要读。P4 v3 以前的芯片上，片内的堆分成高低两块。IDF 先从高的一块分配片内内存，JIT 的代码池也要从这块里出；低的一块是 IDF 的 retention 内存，只有指明要它的分配，或者高的一块已经放不下的分配，才会用到它。所以留在片内的任务栈申请 retention 内存，放在低的一块，见 `components/platform/src/esp/task_stack.h`。IDF 默认在开机时从最大的空闲块里切 32KB，留给 DMA 和必须在片内的分配，这块正好落在高的一块里，所以公共 `sdkconfig.defaults` 关掉了这项预留。`Game` 初始化时就占住 JIT 代码池要的一块片内 RAM，大小取 GB 和 GBA 两种 JIT 里大的一种，以后每个游戏的 JIT 都把 gateway 和代码池放在这块里面，退出游戏也不释放。游戏运行时高的一块只剩下零碎的空间，任务这时第一次创建、以后一直保留的小对象会落进这些空隙，比如 IDF 第一次初始化函数内的静态变量时建的两个信号量；如果退出游戏时释放了这块，这些对象就会把下一个游戏要的连续空间切开。公共 `sdkconfig.defaults` 还把 IDF 的一部分中断函数和锁放回 flash，前提和依据写在那个文件里。

## 待定

- 自制掌机的板子名。
- PC 版桌面调试面板的图形库。

# 任务 1-3：ARM7TDMI 解释器

在 `components/core/` 里新建 `gba/`，写 ARM7TDMI 的解释器，让它执行 SingleStepTests 的单步用例时，寄存器、流水线和每次总线访问都和预期一致。1-4 用它打通板上的单步测试，1-5 拿 JIT 的结果和它比对，以后 JIT 遇到不能翻译的情况也回退到它。这个任务依赖 1-1 和 1-2：测试数据先准备好，再开始写模拟器。

开工时在[任务状态](status.md)里把这个任务标为“进行中”。

## 先读

- 路线图 GBA 段开头的几段和里程碑 1，[架构](../../architecture.md)的“分层”“目录”“模拟器”和“代码规则”。
- `refs/docs/gbatek.htm` 的 “ARM CPU Reference” 全部小节，尤其是 “ARM CPU Register Set”“ARM CPU Exceptions”“ARM CPU Memory Alignments” 和 “ARM CPU Instruction Cycle Times”。
- 1-1 生成脚本开头对用例字段的说明，以及 `refs/tests/arm7tdmi/README.md`。
- GB 段的 SM83 解释器和平坦总线（`gb/sm83.h`、`gb/bus.h`），以及 [GB 任务 1-1](../gb/task-1-1.md) 的做法。

## 范围

- 新建 `components/core/` 下的 `include/gbx/gba/` 和 `src/gba/`，CMakeLists 和其他源文件一样写成两用，PC 版和测试固件都要能编译。
- 实现 ARMv4T 的全部 ARM 和 Thumb 指令：数据处理和移位器的进位（包括寄存器移位量为 0、32 和大于 32，以及 RRX）、PSR 传送、乘法和长乘法、单个和半字、有符号的读写、块传送（包括用户模式寄存器、空的寄存器列表和基址在列表里的情况）、SWP、分支和 BX、SWI，以及协处理器指令和未定义指令。GBA 没有协处理器，协处理器指令按用例和 GBATEK 处理。
- 条件执行和 NZCV。CPU 模式和分组寄存器：USR、FIQ、IRQ、SVC、ABT、UND 和 SYS，各模式的 SPSR，以及 MSR 切换模式。异常的进入和返回：SWI、未定义指令，以及写 PC 时带 S 位的数据处理和带 `^` 的 LDM。IRQ 的进入在里程碑 2 和中断一起接上，但是进入异常的代码在这里写好。
- 两级流水线：用例的 `pipeline` 是已经取到的两条指令，执行时 PC 比当前指令超前 8 字节（Thumb 是 4 字节），分支以后重新填满流水线。
- 每次总线访问带上类型（取指、读或写）、宽度、地址和 N/S 标记。内部周期也要能被总线看到，里程碑 2 按它们和等待周期计时。读写未对齐的地址按 GBATEK “ARM CPU Memory Alignments”：LDR 的循环移位、LDRH 和 LDRSH 的未对齐行为、写入时强制对齐。
- 单步用例用平坦总线，按用例记录的访问返回数据，取指的返回规则见 1-1 的说明。解释器能从任意给定的 CPU 状态开始，执行一条指令以后返回。单步用例这样执行，JIT 回退时也这样调用。

## 不做

- 内存映射、等待周期、预取缓冲、IO 寄存器、中断线和其他设备，这些在里程碑 2。
- 板上的单步测试通路，这是 1-4 的内容。
- 整机的测速和按游戏的瓶颈做的优化，这些在里程碑 2 以后。指令的实现本身仍然要按性能写，见“约束”。

## 约束

- 写代码时就要注意性能，解释器和 JIT 都一样。在满足功能的前提下选性能最好的实现，不写明显低效的代码，也不留给以后的优化任务再改。
- 里程碑 2 会把平坦总线换成真实的内存映射，每次访问之前把设备推进到当前时刻，并按区域加上等待周期。CPU 和总线之间的边界要让这次替换不必重写指令的实现，但是现在不要实现这些设备。
- 每次访存都会走到总线，所以不能用虚函数分派。用模板参数选择总线之前，先量一下代码体积，P4 的 L1 指令缓存只有 16KB。
- 逐次记录总线访问只有测试需要，游戏运行时不记录，记录功能也不能给每次访存增加开销。
- ARM7TDMI 没有定义的编码按 GBATEK 执行未定义指令异常，不能当成走不到的分支。
- 用例 README 的 “Known Issues” 说 `r15` 同时作为写回寄存器和目标寄存器的几种组合可能不准。遇到这类用例，按 GBATEK 和 ARM 的文档判断，列出用例名和依据，不要为了通过去迁就它。
- GBA 的解释器以后是热代码，按 `cmake/core_layout.cmake` 和 `components/core/linker.lf` 的规则放进片内 RAM。GBA 的对象排在 GB 的热代码之后，不能移动 GB 的热代码。记录片内 RAM 的用量增加了多少，JIT 代码池和 L2 cache 的大小保持不变。

## 由你决定

CPU 状态的组织、解码用 switch 还是编译期生成的表、ARM 和 Thumb 是否共用指令语义、总线接口的形式，以及 `gba/` 下的文件划分。选择的理由写在代码注释里。如果改变了模块边界，同步更新架构文档。

## 开发期间的验证

板上的单步测试通路要到 1-4 才有。在这之前，可以在 PC 上写一个临时程序，读取 `refs/tests/arm7tdmi` 的用例，和解释器的结果对照，用来排错。这个程序不提交，不加进 CTest，也不写进文档，因为验收测试在板子上运行。

## 完成标准

- 在临时对照中跑完全部用例，除了已经列出依据、交给用户确认的用例以外全部通过。比较的项目和 1-4 的判定相同，`cycle` 不比较。
- PC 的 debug 构建和 `sila_p4-test-release` 编译通过，没有新的警告。
- 这里的通过只是开发期间的结果。要等 1-4 在板子上跑通全部单步用例，解释器才算通过测试，汇报时不要说成已经通过测试。

## 收尾

- 满足全部完成标准以后，在[任务状态](status.md)里把这个任务标为“已完成”。
- 更新架构文档中“`core/` 下的 `gba/` 还没有建立”的说法，写明 ARM7TDMI 解释器和总线接口的模块边界。
- 向用户汇报设计选择、临时对照的结果、片内 RAM 的用量变化，以及发现的用例问题。

## 开发核对结果

解释器和测试用的平坦总线已经实现，在 PC 上用临时程序跑完了 `refs/tests/arm7tdmi/v1` 的 45 个文件、2,250,000 条用例。比较的项目和 1-4 的判定相同：R0 至 R15、各模式的分组寄存器、CPSR、5 个 SPSR、流水线，以及每次总线访问的类型、宽度、地址、数据、N/S 标记和先后顺序，`cycle` 不比较。用例状态里的 `access`，也就是下一次取指是 N 还是 S，也比较了。板上的验证由任务 1-4 完成。

比较 R15 时不算 ARM 状态下的 bit 1–0 和 Thumb 状态下的 bit 0，因为 ARM7TDMI TRM（DDI 0029G）2.6.1 说这几位没有定义、要忽略。这样比较，2,242,294 条和套件一致。6,206 条没有定义结果的 R15 写入，和用户商定不和套件比较，直接计为通过，只用来检查解释器和 JIT 的结果一致。另外 1,500 条 CP14 的 MRC 和 MCR，用例的预期和 GBATEK 以及原机测试矛盾，和用户商定也按 R15 写入的做法处理。依据如下：

1. R15 的低位。指令把没有对齐的地址写进 R15 时，用例从对齐的地址取指，但是 R15 里留着低位，比如 ARM 状态下 BX 到 bit 1 为 1 的地址以后，R15 是目标地址加 8。GBATEK “ARM CPU Memory Alignments”写明 R15 总是强制对齐，没对齐的跳转不影响后面读 R15 的指令；THUMB.5 也写明 Thumb 状态下 `BX R15` 的 PC 按 `($+4) AND NOT 2` 对齐。所以解释器按 GBATEK 清掉低位。refs 里没有测试 ROM 检查这一点。这样的用例有 33,362 条，除了 R15 的低位，取指和其他字段都和解释器相同。
2. 没有定义结果的 R15 写入，6,206 条。ARM 的单个数据传送（LDR、STR、LDRB、STRB、LDRH、STRH、LDRSB、LDRSH）以 R15 为基址并且写回、目标不是 R15 时，用例从写回值加 4 的地址重新取指；MRS 以 R15 为目标时，用例不重新取指，R15 得到 PSR 的值以后照常加 4。ARM 的文档把这些编码定为 unpredictable，GBATEK 不允许这样用，用例 README 的 “Known Issues” 也说 R15 作为写回寄存器和目标寄存器的情况可能不准。在 LDM 和 STM 写回 R15，以及乘法和 SWP 以 R15 为目标时，用例都从写进 R15 的值重新取指，解释器对这几类写入统一按这个做法处理。
3. CP14 的 MRC 和 MCR，1,500 条。用例让条件成立的 MRC、MCR p14 都进入未定义指令异常。GBATEK “ARM CP14 ICEbreaker Debug Communications Channel”写明 GBA 访问 CP14 不产生异常，MRC 读到 `$+8` 处预取的指令；1-1 选为必过的 alyosha `arm/Coprocessor_14.gba` 在原机上检查这一点，以及 MRC 以 R15 为目标时设置 NZCV。解释器按 GBATEK 实现。CP14 的 CDP、LDC 和 STC 仍然进入未定义指令异常，用例和这个 ROM 的说明一致。

| 文件 | 只有 R15 的低位不同 | R15 写入 | CP14 |
|---|---|---|---|
| `arm_bx` | 5,936 | | |
| `arm_data_proc_immediate_shift` | 780 | | |
| `arm_data_proc_immediate` | 756 | | |
| `arm_data_proc_register_shift` | 719 | | |
| `arm_ldm_stm` | 4,716 | | |
| `arm_ldr_str_immediate_offset` | 596 | 1,190 | |
| `arm_ldr_str_register_offset` | 615 | 1,166 | |
| `arm_ldrh_strh` | 623 | 1,156 | |
| `arm_ldrsb_ldrsh` | 1,220 | 1,114 | |
| `arm_mcr_mrc` | | | 1,500 |
| `arm_mrs` | | 1,580 | |
| `arm_mul_mla` | 958 | | |
| `arm_mull_mlal` | 1,804 | | |
| `arm_swp` | 1,182 | | |
| `thumb_bx` | 13,381 | | |
| `thumb_ldm_stm` | 50 | | |
| `thumb_push_pop` | 26 | | |

不和套件比较的用例按编码认定：上面列出的单个数据传送以 R15 为基址写回、目标不是 R15，MRS 以 R15 为目标，以及 CP14 的 MRC 和 MCR。它们照常在两个后端上运行，两边的结果要一致。

开工前和用户商定的几点都按用例实现，相关用例全部通过：Thumb 的 0xDExx 按无条件跳转执行；乘法带 S 时的 C 按 [bmchtech 的原机研究](https://bmchtech.github.io/post/multiply/)计算；GBATEK 只写了编程限制、没有写硬件结果的地方按用例的预期，包括 MSR 改 T 位和保留位、写入无效的模式值、USR 和 SYS 模式下读写 SPSR、没有 SPSR 的模式下带 S 写 R15、TST、TEQ、CMP、CMN 以 R15 为 Rd、R15 作为 Rs 和乘法、SWP 的操作数时读到的值，以及带 S 的 LDM、STM 写回 User 模式的基址寄存器。

内部周期以后的取指按 N 处理。TRM 第 6 章的周期表里，CPU 在这个周期驱动 SEQ=1；GBATEK “GBA GamePak Prefetch” 的 “Prefetch Disable Bug” 说 GBA 上这次取指是 1N，用例也记为 N。预取打开时，里程碑 2 的总线按预取缓冲处理这次取指。

另外两处不同不影响判定，记下来给 1-4 和里程碑 2 参考：

- SWP 的读和写，TRM 6.11 说两个周期都驱动 LOCK，解释器两次访问都带 Lock；24,910 条用例只在写入时标 Lock。1-4 只比较 N/S 标记。
- 377,792 条用例的 `cycle` 和解释器数出的总线周期不同。用例里写入的周期号和它前面那次访问相同，内部周期也不计。

用例没有覆盖的部分，包括 IRQ 的进入、Thumb 的保留编码、BKPT 和 `11101` 开头的编码、ARM 的 `011…1` 未定义编码，以及无效模式下的分组寄存器，用临时程序检查过。CP14 的 MRC 和 MCR 也按 GBATEK 的描述检查过，它们的时序要到里程碑 2 用 alyosha 的 ROM 检查。

PC 的 debug 构建和 `sila_p4-test-release` 都编译通过，没有新的警告。测试固件还没有调用 GBA 的代码，这些代码在链接时都被丢掉，所以这个任务没有改变片内 RAM 的用量和固件的大小，GB 热代码在 map 里的地址也没有变。按 P4 release 构建的目标文件，1-4 接上以后，平坦总线上的解释器（`arm7_step.cpp`）留在 flash，代码和只读数据共 13,398 字节；`arm7.cpp` 里以后整机解释器和 JIT 也要用的 `multiply_carry`、`arm7_switch_mode` 和模式表共 510 字节，进片内 RAM，排在 GB 的 PPU 之后。JIT 代码池和 L2 cache 的大小不变。

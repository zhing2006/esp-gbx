# 任务 2-3：MBC 和 OAM DMA

实现 MBC1、MBC2、MBC3 和 MBC5 的 bank 切换和卡带 RAM，以及 OAM DMA。解释器和 JIT 同时实现：JIT 的块标识要加入 bank 映射，OAM DMA 期间的总线限制不能被快速访存绕过。这个任务依赖 2-2。

开工时在[任务状态](status.md)里把这个任务标为“进行中”。

## 先读

- `refs/docs/pandocs/src/` 中的 `MBCs.md`、`nombc.md`、`MBC1.md`、`MBC2.md`、`MBC3.md`、`MBC5.md`、`The_Cartridge_Header.md` 和 `OAM_DMA_Transfer.md`。
- [产品设计](../../product-design.md)“GB”一节关于 HRAM 中 DMA 例程的说明，以及“ROM 加载”一节。
- 2-1 的内存映射、JIT 块标识、测试索引和报告。

## 范围

- 按卡带头选择控制器。MBC1 包括 multicart 的接线方式，识别方法由你决定，依据写进注释。
- MBC2 的内置 RAM，MBC3 的 ROM 和 RAM bank，MBC5 的 9 位 ROM bank，ROM 最大 8MiB。
- 卡带 RAM 的使能、bank 切换和容量，容量按卡带头分配。
- OAM DMA 的启动延迟、传输时序和重新启动，以及传输期间 CPU 访问总线得到的结果，依据 Pan Docs 和 mooneye 的 `oam_dma` 相关用例。
- JIT 的块标识加入代码所在区域的 bank，切换 bank 不清空整个代码缓存。卡带 RAM 中的代码也由 JIT 执行，RAM 被改写、关闭或者换 bank 以后，对应的块失效。OAM DMA 期间，快速访存不能绕过总线限制。游戏复制到 HRAM 里的 DMA 例程由 JIT 执行。
- 测试索引中的这些 ROM 改为已支持。

## 不做

- MBC3 的 RTC 和卡带存档的写回，这些在里程碑 3。
- OAM 在 PPU 各模式下的访问限制，这在 2-4。
- `oam_bug` 描述的 OAM 损坏，它只记入报告。
- SD 分页缓存。GB 的 ROM 整个载入 PSRAM。

## 约束

- 游戏切换 bank 很频繁，每次切换都不能清空代码缓存，也不能重新编译所有块。
- ROM 访问层放在 `common/`，接口要给 GBA 的分页缓存留出位置，但是 GB 读取 ROM 的路径上不能有分页查找的开销。
- OAM DMA 写 OAM 时，和 CPU 写 OAM 走同一条路径，2-4 要把这些写入交给渲染所在的核。
- 回退、测试时间和回传大小沿用 2-1 的规则。

## 由你决定

块标识中 bank 的编码、bank 切换以后查找块的方式、OAM DMA 期间的访问路径，以及卡带控制器的代码组织。

## 完成标准

- 按[双后端优化要求](../../hardware-testing.md#每个任务的双后端优化)，分别优化解释器和 JIT 的 bank 切换、内存访问与 DMA 相关路径，提交两边各自的优化前后数据和剩余瓶颈。完成优化并清理临时计时后，再做最终验证。
- 在 Sila P4 上，两个后端都通过 mooneye `emulator-only` 下的 `mbc1`、`mbc2`、`mbc5`，以及 `acceptance` 下 OAM DMA 相关的用例，结果一致，回退清单为空或者已经和用户确认。之前通过且受本任务影响的 ROM 仍然通过。
- SM83 单步只在实际改动影响 CPU 执行时运行，按[真机测试计划](../../hardware-testing.md#选择本次运行的测试)选择受影响的操作码组；通用路径的影响无法缩小时才跑全量，不因任务收尾自动跑全量。
- 报告给出这些 ROM 的速度。按实测速度估算的全量耗时仍在路线图的预算以内；超出时先和用户确认优化方案。
- 另外两块板子的正式固件和测试固件都还能构建。

## 收尾

- 满足全部完成标准以后，在[任务状态](status.md)里把这个任务标为“已完成”。
- 在架构文档中说明 ROM 访问层和卡带控制器的模块边界，验证结论保存在运行报告中，测试方法有变化时更新真机测试计划。
- 向用户汇报测试结果、速度和回退清单。

## 实现与验证

MBC1、MBC1M、MBC2、MBC3、MBC5、卡带 RAM 和 OAM DMA 已经实现。ROM 窗口与控制器的边界、JIT bank 标识及代码失效规则见[架构](../../architecture.md#模拟器)。允许直接访问的 RAM 保留写入权限，JIT 在实际取指不匹配时回收对应的块。测试方法见[真机测试计划](../../hardware-testing.md#运行整机-rom)，双后端验证结果、优化数据和测试耗时保存在运行报告中。

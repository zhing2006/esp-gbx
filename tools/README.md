# 工具

本目录放开发用的脚本，都在仓库根目录运行。`ui/` 下的脚本和 `lcd_colors.py` 用 uv 运行，所需的 Python 版本和库写在各脚本开头；其他脚本用 Python 3.11 或更新版本，参数和数据格式写在脚本开头的说明里。

## 测试数据和测试

| 脚本 | 用途 |
|---|---|
| `fix_sm83_stop.py` | 从下载的 SM83 单步套件生成 `sm83-stop-pc-v1` 副本，只修正普通 STOP 入口的 PC 预期，原始套件不变。依据见[任务 1-1](../docs/plans/gb/task-1-1.md#开发核对结果)。 |
| `prepare_gb_tests.py` | 把 SM83 套件的 JSON 转成按操作码索引的 `cases.bin`，再整理选中的 GB 测试 ROM、参考图和文件清单，生成复制到 SD 卡的 `tests/gb/`，默认输出到仓库的 `tests/sdcard/gb`。`tests/runner/` 也用它读取用例。 |
| `prepare_gba_tests.py` | 把 ARM7TDMI 单步套件转成按文件和用例索引的 `cases.bin`，再整理选中的 GBA 测试 ROM，构建自写的测试 ROM，加上参考帧和文件清单，生成复制到 SD 卡的 `tests/gba/`，默认输出到仓库的 `tests/sdcard/gba`。`--toolchain` 传给 `gba_test_roms.py`。`--check-bios` 按 No-Intro 的校验值核对 GBA BIOS 的转储。 |
| `gba_test_roms.py` | 从 `tests/gba/roms/` 构建自写的 GBA 测试 ROM。工具链固定为 Arm GNU Toolchain 15.3.Rel2，构建前核对版本；`--toolchain` 指定解压后的目录，不指定时依次找 `ARM_GNU_TOOLCHAIN` 和 PATH。macOS 和 Windows 的下载文件名写在脚本开头。macOS（Apple silicon）上构建过，Windows 上没有验证。 |
| `gba_reference_frames.py` | 维护 `tests/gba/frames/` 的参考帧。`check` 核对提交的参考帧：清单完整，自写 PPU 场景的帧和 `tests/gba/roms/ppu/model.py` 算出的画面相同。`generate` 用参考模拟器重新生成。卡上的 GBA 测试数据在 GBA 任务 1-2 以后不再改动，所以只有确认参考帧有错、用户同意替换时才再用它。 |
| `reference_identity.py` | 测试固件构建时自动运行，给 SM83 和 ARM7TDMI 的解释器各算出一个标识，覆盖各自的核心代码和测试共用的执行代码，写进生成的头文件。runner 用这个标识判断一份报告能不能作为验证基准。 |
| `check_rv32_encoding.py` | 用 ESP-IDF 工具链的汇编器和链接器，核对 PC 版 `gbx_rv32_encoding` 生成的 RV32 机器码。 |

这几个脚本在测试流程里的用法见[真机测试计划](../docs/hardware-testing.md)。

### GBA 参考模拟器

参考帧由 NanoBoyAdvance v1.8.3 生成，源码取自 https://github.com/nba-emu/NanoBoyAdvance 的 tag `v1.8.3`（提交 `8c3b159c649b027ea8fc87df3290bad4cdcdcf4b`）。取帧用的是一个链接它的核心库的小程序，只用核心库的公开头文件。按任务 1-2 和用户商定的做法，这个程序和模拟器的源码都不进入仓库，本地副本放在 `refs/emulators/` 下，被 git 忽略。运行参数是：`Config::skip_bios` 打开，BIOS 用和卡上相同的 No-Intro 转储；不用色彩校正和帧间混合，这两项在 NanoBoyAdvance 的前端着色器里，核心库交出的是原始颜色；这些 ROM 不接存档芯片，也没有按键输入。程序的命令行和输出格式写在 `gba_reference_frames.py` 开头，`generate --nba` 传入这个程序。`--mgba` 可以再传入一个同样接口的 mGBA 0.10.5 程序，生成时记下 mGBA 的画面是否一致，只用来对照。

## 构建

| 脚本 | 用途 |
|---|---|
| `link_core_layout.py` | 固件和测试固件构建时自动运行，改写 IDF 生成的链接脚本，把 `components/core/linker.lf` 和 `components/jit/linker.lf` 映射到片内的代码按固定顺序排到片内 RAM 代码段的开头，P4 v3 以前的芯片上，GBA 跑在 CPU 所在核上的执行代码放进片内 RAM 后一段的数据区，并检查其中列出的冷函数都在对象里，见[架构](../docs/architecture.md#代码规则)。 |
| `ui/pack_assets.py` | 正式固件构建时自动运行，把所选板子的 UI 素材打成资源包，写进 assets 分区，见 [UI 布局](../docs/ui-layouts/index.md#资源包)。 |

## 板上调试和性能分析

| 脚本 | 用途 |
|---|---|
| `console.py` | 和固件的串口控制台交互，命令见[调试设计](../docs/debug-design.md#串口控制台)。`serve` 占住串口，把板子的输出写进日志，把截图存成 PNG，把 `file` 取回的文件按卡上的路径存在运行目录的 `files/` 下；`send` 发命令并打印回复；`replay` 按文件里的命令和截图检查点重放一段游戏，停下以后可以从停下的那一行续跑。`serve` 需要 pyserial，IDF 的 Python 环境已经带着。 |
| `profile_report.py` | 汇总分析版固件的采样：`<板子>-test-profile` 测试固件经 `run_roms.py` 或 `run_gba_roms.py` 生成的报告，或者 `<板子>-profile` 正式固件控制台日志里 `prof` 的输出。用对应固件的 ELF 解析符号，按类别和函数列出样本；芯片掩膜 ROM 里的样本要用 `--rom-image` 传入从板上读出的 ROM 才有函数名，读法见脚本说明；测试报告还列出两个后端的耗时和计数：GB 的是 JIT 计数，GBA 的是 CPU 所在的核等渲染线程的次数和时长。`riscv32-esp-elf-nm` 不在 PATH 里时通过 `eim run` 调用。 |

## 颜色校正

| 脚本 | 用途 |
|---|---|
| `lcd_colors.py` | 由 `refs/docs/gbc-lcd/` 里一台 GBC 屏幕的测量数据算出 COLOR FIX 的参数，写成 `components/app/src/lcd_colors.h`：各分量按级数的响应，以及从三个分量的响应得到线性 sRGB 的矩阵和偏移。白点、黑电平等选择和理由写在脚本开头。`--compare <目录>` 另外输出各种选择的对比图。改了测量数据或选择以后重新运行，再重新构建固件。 |

## UI 素材

`ui/` 下的脚本生成游戏页和菜单的素材和效果图，运行顺序见 [UI 布局](../docs/ui-layouts/index.md#生成和检查)。

| 脚本 | 用途 |
|---|---|
| `ui/build_sprites.py` | 从概念图生成各板共用的控件 sprite 和它们的各种状态 |
| `ui/vfd_font.py` | 生成 VFD 点阵字体的字形表，以及每种点距的发光字图 |
| `ui/menu_font.py` | 把菜单用的像素字体从 BDF 转成资源包里的字形表 |
| `ui/menu_sounds.py` | 合成菜单的提示音，写成 `assets/ui/common/sounds/` 里的 WAV 文件 |
| `ui/export_boards.py` | 按各板的 `spec.json` 生成板子底图和 `board.json` 布局 |
| `ui/render_previews.py` | 从 `assets/ui/` 生成 `docs/ui-layouts/` 里的效果图。加 `--check` 时重新生成并和现有 PNG 逐像素比较，有缺失或不一致就返回非零退出码，不写文件；它检查的是素材和效果图是否一致，不是模拟器的正确性。 |
| `ui/pasteup.py` | 为新板子拼一张底稿：纯色的外壳加上从 Tab5 底图切下的部件，作为生成成品底图的输入 |
| `ui/preview_scene.py` | 效果图里游戏画面用的原创示意图，不是游戏 ROM，也不是测试参考 |
| `ui/menu_kit.py` | 按菜单的网格、配色和字体画出各个界面，供效果图使用 |
| `ui/menu_scenes.py` | 菜单效果图里的示例内容：文件名、数量和 ROM 头信息都是编的 |
| `ui/uikit.py` | 以上脚本共用的路径、形状、底图和合成函数 |

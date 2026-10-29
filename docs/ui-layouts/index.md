# UI 布局

本文说明游戏页的外壳设计、菜单、素材文件、触摸规则和生成方法。所有板子用同一套外壳风格和控件，每块板子有自己的底图和布局。构建正式固件时，素材打包进 assets 分区，固件用 LVGL 显示。效果图里的游戏场景由 `tools/ui/preview_scene.py` 绘制，VFD 数值都按未采集显示为 `--`，所以效果图不是模拟器截图，也不能作为模拟核心测试的参考帧。

调试指标的含义、统计口径和接入顺序见[调试设计](../debug-design.md)。

## 效果图

| 板子 | 逻辑尺寸 | 未载入游戏 | GB | GBC | GBA | 主界面 | 暂停菜单 |
|---|---|---|---|---|---|---|---|
| Sila P4X | 1280×452 | [idle](sila_p4x/idle.png) | [gb](sila_p4x/gb.png) | [gbc](sila_p4x/gbc.png) | [gba](sila_p4x/gba.png) | [home](sila_p4x/home.png) | [pause](sila_p4x/pause.png) |
| Sila P4 | 720×720 | [idle](sila_p4/idle.png) | [gb](sila_p4/gb.png) | [gbc](sila_p4/gbc.png) | [gba](sila_p4/gba.png) | [home](sila_p4/home.png) | [pause](sila_p4/pause.png) |
| JC1060P470 | 600×1024 | [idle](jc1060p470/idle.png) | [gb](jc1060p470/gb.png) | [gbc](jc1060p470/gbc.png) | [gba](jc1060p470/gba.png) | [home](jc1060p470/home.png) | [pause](jc1060p470/pause.png) |
| M5Stack Tab5 | 720×1280 | [idle](m5stack_tab5/idle.png) | [gb](m5stack_tab5/gb.png) | [gbc](m5stack_tab5/gbc.png) | [gba](m5stack_tab5/gba.png) | [home](m5stack_tab5/home.png) | [pause](m5stack_tab5/pause.png) |
| OSPTEK ESP32-P4C5 | 480×800 | [idle](osptek_p4c5/idle.png) | [gb](osptek_p4c5/gb.png) | [gbc](osptek_p4c5/gbc.png) | [gba](osptek_p4c5/gba.png) | [home](osptek_p4c5/home.png) | [pause](osptek_p4c5/pause.png) |
| 慧勤智远 ESP32-S31 CB | 480×800 | [idle](wks_s31_cb/idle.png) | [gb](wks_s31_cb/gb.png) | [gbc](wks_s31_cb/gbc.png) | [gba](wks_s31_cb/gba.png) | [home](wks_s31_cb/home.png) | [pause](wks_s31_cb/pause.png) |
| 乐鑫 ESP32-S31-Korvo-1 | 480×800 | [idle](esp32_s31_korvo_1/idle.png) | [gb](esp32_s31_korvo_1/gb.png) | [gbc](esp32_s31_korvo_1/gbc.png) | [gba](esp32_s31_korvo_1/gba.png) | [home](esp32_s31_korvo_1/home.png) | [pause](esp32_s31_korvo_1/pause.png) |
| Waveshare ESP32-C5-Touch-LCD-2.8 | 240×320 | [idle](waveshare_c5_lcd28/idle.png) | [gb](waveshare_c5_lcd28/gb.png) | [gbc](waveshare_c5_lcd28/gbc.png) | [gba](waveshare_c5_lcd28/gba.png) | [home](waveshare_c5_lcd28/home.png) | [pause](waveshare_c5_lcd28/pause.png) |

GB 效果图点亮了 GB 灯和 INT 灯。GBC 效果图点亮了 GBC 灯和 JIT 灯。GBA 效果图点亮了 GBA、JIT 和 SAVE 灯，速度拨在 2X。未载入游戏时和主界面上，所有灯都不亮，`SPEED` 格显示 `NO ROM`。暂停菜单的效果图保留了游戏中的 GB 灯和 INT 灯，`SPEED` 格显示 `PAUSE`。[states.png](states.png) 在 Tab5 上列出了每个控件、拨杆和指示灯的全部状态。菜单的每个界面和提示框另外按窗口大小输出在 [menus/](menus/) 里，见“菜单”一节。JC1060P470 的固件编入了蓝牙手柄，所以它的主界面效果图比其他板子多一张 WIRELESS GAMEPAD 卡片。

Sila P4X 的 6.2 英寸屏幕用横屏布局。它的底图是 `tools/ui/pasteup.py` 按 `raws/ui/sila_p4x/spec.json` 拼出的底稿，直接使用，没有再生成成品底图，也没有序列号贴纸。屏幕和触摸的接线条件见 [Sila P4X 板级文档](../boards/sila_p4x.md#屏幕与触摸)，显示方向和触摸尚未上板确认。

OSPTEK ESP32-P4C5 的 4.3 英寸屏幕是 480×800 的竖屏，布局按 JC1060P470 的竖屏布局缩小。游戏窗口和屏幕一样宽，镜片横贯整个屏幕，只在窗口上下露出来。窗口外没有地方露出屏幕不发光的边框，所以 `plate.display_border` 为 0。七个灯在镜片下沿排成一行，VFD 是 2×2 四格。它的底图和 Sila P4X 一样直接使用 `pasteup.py` 拼出的底稿，没有序列号贴纸。

慧勤智远 ESP32-S31 CB 配的 4.3 英寸 RGB 屏同样按 480×800 竖屏使用，`raws/ui/wks_s31_cb/` 复制了 OSPTEK ESP32-P4C5 的底稿和 `spec.json`，只改了板名，所以两块板子的底图和布局相同。乐鑫 ESP32-S31-Korvo-1 的屏幕也是 4.3 英寸 800×480，`raws/ui/esp32_s31_korvo_1/` 又复制了慧勤智远 ESP32-S31 CB 的这两个文件，只改了板名。

Waveshare ESP32-C5-Touch-LCD-2.8 的 2.8 英寸屏幕是 240×320 的竖屏，布局按慧勤智远 ESP32-S31 CB 的布局缩小一半，`control_scale` 是 0.3。屏幕只有 240 像素宽，放不下 480×320 的窗口，所以游戏窗口是 240×160，游戏画面和菜单都按原大小显示。游戏画面每隔一帧显示一次，见[架构](../architecture.md#显示和触摸)。GB 没有 L/R，屏幕上也没有地方放，所以去掉了 L/R。ESP32-C5 只有一个核，UI 和模拟在同一个核上运行，所以这块板子没有 VFD，固件还打开了 `GBX_UI_STATIC_SHELL`：控件开机时画好以后不再随触摸改变外观，只有 SPEED 拨杆跟着倍速移动，指示灯照常亮灭。七个灯在镜片下沿排成一行。底图直接使用 `pasteup.py` 拼出的底稿。SETTINGS 没有 SCALING 这一行，见 [settings](waveshare_c5_lcd28/settings.png)。

## 外壳

外壳参考了 Game Boy、Game Boy Color 和 Game Boy Advance 的外形和配色，但没有使用任天堂的标识或文字。上半部分是暖灰色的不透明塑料，下半部分是半透明的靛紫色塑料，可以看到里面的电路板。屏幕外有深色镜片，A/B 是斜放的酒红色圆键，L/R 是包住屏幕左右边缘的肩键。底图画成带纸纹和颗粒的喷枪插画风格。素材转换为 RGB565 供固件使用。

镜片上的屏幕开口比游戏窗口大一圈，露出屏幕不发光的深灰色边框。边框宽度由各板 `spec.json` 里的 `plate.display_border` 设置。Sila P4 和 Sila P4X 的镜片边框比较窄，所以这一圈比另外两块板子窄。

外壳不随游戏的机型变化，模拟的机器由镜片上的 GB、GBC、GBA 三个灯表示。

## 指示灯和拨杆

| 灯 | 亮起时的颜色 | 含义 |
|---|---|---|
| GB、GBC、GBA | 黄绿、玫红、紫 | 模拟的机器：GB 模式亮 GB 灯，CGB 模式和 DMG 兼容模式亮 GBC 灯，GBA 游戏亮 GBA 灯；未载入游戏时都不亮 |
| INT、JIT | 绿 | 当前的执行后端，同一时间只亮一个 |
| SAVE | 琥珀、红 | 琥珀色常亮表示 `DIRTY`，琥珀色闪烁表示 `WRITING`，红色表示 `ERROR`，`CLEAN` 时熄灭 |
| ERR | 红 | 有未处理的错误 |

存档状态的定义见调试设计。Tab5 和 JC1060P470 的灯竖排在屏幕两侧。Sila P4 的屏幕两侧放了 VFD，OSPTEK ESP32-P4C5 的游戏窗口占满了屏幕宽度，所以这两块板子的七个灯在镜片下沿排成一行。Sila P4X 的灯竖排在镜片里、屏幕两侧，左边是 GB、GBC、GBA，右边是 INT、JIT、SAVE、ERR。

左上角的 SPEED 拨杆有 1X 和 2X 两个位置，分别表示单倍和双倍模拟速度。拨到 2X 时，`SPEED` 格的单位显示 `FAST`，`AUDIO` 格的主值显示 `OFF`，欠载次数和最低水位显示 `--`，指标的口径见[调试设计](../debug-design.md#2-倍速)。右上角是 MENU 键。玩 GB 和 GBC 游戏时，L/R 的外观不变，不显示禁用状态。

## VFD

`SPEED`、`FRAME`、`AUDIO`、`MEM` 四组指标使用本项目的点阵字体。格子标题是底图的一部分，标题以外的主值、单位和附加信息由程序绘制。Tab5、JC1060P470 和 OSPTEK ESP32-P4C5 在格子底行显示附加信息，Sila P4 和 Sila P4X 的窄格在单位下面逐行显示。各板显示哪些附加信息见[调试设计](../debug-design.md#在不同尺寸的面板上取舍)。未采集时显示 `--`，无效、过期和预热状态按调试设计区分。

字体保存在 `assets/ui/common/vfd_font.json`，字段定义见 [telemetry-fields.json](telemetry-fields.json)。每块板子在 `raws/ui/<板子>/spec.json` 的 `vfd.formats` 里写每格的主值、单位和附加信息的各行。每行指定所在的行号或者标签行，以及左对齐还是右对齐。`tools/ui/export_boards.py` 据此算出各行位置，写进 `board.json`。

## 菜单

主界面、ROMS 列表、SETTINGS、暂停菜单、SAVE / LOAD 和各种提示框都画在游戏窗口里，各板用同一套布局。菜单显示时，呈现任务不向游戏窗口写游戏帧，回到游戏以后再恢复。

### 网格、字体和配色

菜单画在 240×160 的网格上，在 480×320 的窗口里整体放大 2 倍显示，所以字形、线条和圆角的像素一样大。不直接按 480×320 画，是因为窗口在 Tab5 上只有约 41×28mm，在 Sila P4 上约 48×32mm，12px 的字原样显示只有 1mm 左右高。Waveshare ESP32-C5-Touch-LCD-2.8 的窗口只有 240×160，菜单按原大小显示。

字体是缝合像素字体 12px 比例宽度的简体中文字形版，覆盖拉丁字母、中文、假名和韩文，日文汉字也按简体中文的字形显示。`tools/ui/menu_font.py` 把 `raws/ui/common/fonts/` 里的 BDF 转成 `assets/ui/common/menu_font.bin`，格式写在脚本开头。字体里没有的字符和不合法的 UTF-8 字节，都画成和汉字等宽的空心方框，用户能看出缺了几个字。一行放不下的文字截断，以 `...` 结尾；ROMS 列表里选中的那一行横向滚动，显示全名。

配色取自外壳：暖灰底、靛紫标题栏、酒红选中条。光标停在不能操作的项上时，选中条是灰色的。机型徽标的颜色和镜片上的指示灯一致。颜色、PALETTE 三个选项的颜色和图标都在 `assets/ui/common/menu.json`：GRAY 和 GREEN 是 GB 模式的 4 级颜色，COLOR 的 4 个代表色只用来画选项里的色块。

### 操作

菜单以按键为主，触摸是辅助。这样以后自制掌机的实体按键和 PC 的键盘都能直接操作菜单。

| 按键 | 作用 |
|---|---|
| 方向键上、下 | 移动光标，按住连续移动 |
| 方向键左、右 | SETTINGS 里调整数值或者切换选项；SAVE / LOAD 和提示框里切换按钮 |
| L、R | 列表翻页 |
| A、START | 确认 |
| B | 回到上一级。在提示框里，B 等于取消：不启动游戏，或者不退出游戏；只有 OK 一个按钮的提示框，B 等于 OK |
| MENU | 游戏中打开暂停菜单；在暂停菜单里回到游戏 |

触摸时，点一行把光标移到这一行，再点一次已经选中的行等于按 A，所以误触不会直接启动游戏或者确认提示框。在列表上拖动可以滚动。SETTINGS 里点数值条上的某一格，直接设到那一档；点选项直接选中它。窗口外的 A、B 等控件照常可用。

### 界面

| 效果图 | 说明 |
|---|---|
| [home](menus/home.png)、[home-settings](menus/home-settings.png)、[home-errors](menus/home-errors.png) | 主界面有 ROMS、SETTINGS 和 ERRORS 三个入口，编入了蓝牙手柄的固件还有第四个入口 WIRELESS GAMEPAD。入口是列表，以后可以增加。卡片高 26 像素，按放得下四张卡片的位置排列，没有 WIRELESS GAMEPAD 时第四格空着。ERRORS 右边是故障的条数，没有故障时显示为灰色，标 `NONE`。标题栏右边是卡的状态，底栏说明光标所在的入口，ROMS 的说明是上次玩的游戏，ERRORS 的说明是最新一条故障 |
| [roms-top](menus/roms-top.png)、[roms](menus/roms.png)、[roms-long](menus/roms-long.png) | ROMS 按 `roms/` 的目录浏览，A 进入子目录，B 回到上一级，在顶层按 B 回到主界面。先列子目录，右边是其中 ROM 的个数，包括更深的子目录；再列 ROM。名称排序不区分大小写，数字按数值比较。标题栏显示当前路径和光标位置，底栏显示所选 ROM 头里的标题、MBC 和卡带 RAM 大小。GBA 游戏的底栏显示标题和游戏代码，登记为带 RTC 的游戏后面再标 `RTC` |
| [roms-gbc](menus/roms-gbc.png)、[roms-unsupported](menus/roms-unsupported.png) | 徽标按 ROM 头的 CGB 标志显示（Pan Docs “The Cartridge Header”的 0143），0x80 和 0xC0 的游戏都显示 GBC。徽标只表示卡带，不表示游戏会按哪种模式启动，模式由 PALETTE 决定，见 SETTINGS。GBA 游戏显示 GBA 徽标。卡带类型还不能运行的游戏整行变灰，比如 MBC7，选中时底栏用红字写出原因，按 A 不启动 |
| [roms-missing-glyph](menus/roms-missing-glyph.png) | 字体缺字时显示方框 |
| [roms-scanning](menus/roms-scanning.png)、[roms-no-card](menus/roms-no-card.png)、[roms-empty](menus/roms-empty.png) | 扫描在后台进行，标题栏右边显示 `SCANNING`，扫到的目录和 ROM 陆续出现。没有卡和卡上没有 ROM 时显示说明 |
| [settings](menus/settings.png)、[settings-pc](menus/settings-pc.png) | 一共 6 行，每行 21 像素，一页放得下，不滚动。游戏窗口放不下 2 倍画面的板子不缩放画面，所以没有 SCALING 这一行，只有 5 行。VOLUME 和 BRIGHTNESS 是 10 格的数值条，CPU、PALETTE、COLOR FIX 和 SCALING 是选项。从暂停菜单进入时，标题栏右边标 `IN GAME`。游戏中不能修改的项和当前平台不支持的项显示为灰色，光标停在上面时，底栏用红字写出原因 |
| [settings-palette](menus/settings-palette.png)、[settings-color-fix](menus/settings-color-fix.png) | PALETTE 的选项是 GRAY、GREEN 和 COLOR，表示卡带插在 GB 上还是 GBC 上，每个选项前画一个 2×2 的色块。选 GRAY 或 GREEN 时，黑白游戏和 CGB 标志为 0x80 的游戏按 GB 模式运行，用选中的颜色显示；选 COLOR 时，0x80 的游戏按 CGB 模式运行，黑白游戏按 DMG 兼容模式运行，颜色由兼容调色板决定。0xC0 的游戏不看这一项，总是按 CGB 模式运行。默认是 GRAY。COLOR FIX 是 LCD 颜色校正，选项是 OFF 和 ON，默认 ON，作用于 CGB 模式和兼容模式的画面和缩略图，在主界面里总是可以修改 |
| [settings-game](menus/settings-game.png)、[settings-game-palette](menus/settings-game-palette.png)、[settings-game-color](menus/settings-game-color.png) | 游戏运行中不能在 GB 和 GBC 之间切换。游戏按 GB 模式运行时，PALETTE 只能在 GRAY 和 GREEN 之间切换，COLOR 这个选项是灰的，左右键跳过它，光标停在这一行时底栏说明要在启动游戏之前选；COLOR FIX 整行是灰的。按 CGB 模式或兼容模式运行时，PALETTE 整行是灰的，COLOR FIX 可以修改，改了立即生效 |
| [loading](menus/loading.png) | 载入时显示游戏名和读取进度 |
| [pause](menus/pause.png)、[pause-writing](menus/pause-writing.png)、[pause-error](menus/pause-error.png) | 暂停菜单浮在压暗的最后一帧游戏画面上，有 RESUME、SAVE / LOAD、SETTINGS、ERRORS 和 QUIT，ERRORS 在没有故障时是灰色。下面是存档状态，颜色和 SAVE 灯一致。有故障时，ERRORS 右边用红字写出故障的条数，原因要进 ERRORS 页面看。存档出错时，第一项是 RETRY SAVE |
| [errors](menus/errors.png) | 从主界面或者暂停菜单的 ERRORS 进入。按时间倒序列出最近的故障，左边是发生了多久（`<1m`、`5m`、`2h`、`3d`，按开机以后的单调时钟算，不依赖板上时钟），右边是原因，最多折成两行。底部是 CLEAR 和 BACK，左右切换，默认选中 CLEAR。CLEAR 清空列表、熄灭 ERR 灯，然后回到进来的地方；BACK 和 B 直接回去 |
| [home-gamepad](menus/home-gamepad.png)、[gamepad](menus/gamepad.png)、[gamepad-none](menus/gamepad-none.png) | 主界面的 WIRELESS GAMEPAD 卡片右边是保存的手柄的状态：已经连上时标 `ON`，配对过但是没有连上时标 `OFF`，没有配对过时标 `NONE`。光标停在这张卡片上时，底栏写出手柄的名字和它是否连上。从这张卡片进入 WIRELESS GAMEPAD 页面。页面中间写出保存的手柄是否连上和它的名字，没有配对过时显示 `NO GAMEPAD`。底部是 PAIR 和 BACK，默认选中 PAIR。暂停菜单里没有这个入口 |
| [gamepad-searching](menus/gamepad-searching.png)、[gamepad-paired](menus/gamepad-paired.png)、[gamepad-failed](menus/gamepad-failed.png) | 按 PAIR 开始配对，页面依次显示 `STARTING`、`SEARCHING` 和 `CONNECTING`。`SEARCHING` 时提示按住手柄的配对键，让它进入配对模式，最多找一分钟。配对进行时 PAIR 是灰色的，蓝牙手柄的按键也不起作用，但是触摸和其他按键照常可用。配对成功后显示 `PAIRED` 和新手柄的名字，新手柄替换原来保存的那个。如果失败，页面用红字显示 `NOT PAIRED` 和原因，原来保存的手柄继续使用。有了结果以后可以再按 PAIR。BACK 和 B 放弃还没完成的配对，回到主界面 |

SCALING 提供 NEAREST 和 SMOOTH，见 [settings-scaling](menus/settings-scaling.png)，两种模式使用同一套整数倍尺寸和黑边。有平滑实现的平台默认选中 SMOOTH，否则选中 NEAREST。暂停时切换会在恢复游戏时生效。CPU 显示后端没有平滑实现，因此 SCALING 整行置灰、不可修改，固定选中 NEAREST，见 [settings-scaling-unavailable](menus/settings-scaling-unavailable.png)。如果文件里保存了当前构建不支持的 SMOOTH，读取时自动回退到 NEAREST，界面选中 NEAREST，不阻止游戏运行。

光标会记住上次玩的 ROM，QUIT 回到 ROMS 和重启以后，都回到它所在的目录并停在它上面。ROMS 右边的 `SAV` 表示卡上已经有这个 ROM 的卡带存档。

ERR 灯点亮以后不会自己熄灭，要在 ERRORS 页面按 CLEAR。故障列表只保留最近 3 条，因为每条最多两行，ERRORS 页面正好放得下 3 条，清除之前每一条都看得到。同样的原因合并成一条，时间按最近一次算。存档错误本身仍然由 SAVE 灯、暂停菜单的存档状态和 RETRY SAVE 表示，直到重试成功，CLEAR 不改变它们。

### SAVE / LOAD

暂停菜单里的 SAVE / LOAD 是即时存档，有 4 个槽位。槽位在界面上叫 `SLOT 1` 到 `SLOT 4`，和卡带存档的 SAVE 区分开，界面上也不出现 STATE 这个词。SAVE / LOAD 排在 RESUME 下面，打开暂停菜单以后按一次下就能选中，QUIT 仍然是最后一项。

| 效果图 | 说明 |
|---|---|
| [slots](menus/slots.png)、[slots-empty](menus/slots-empty.png)、[slots-damaged](menus/slots-damaged.png)、[slots-time-unknown](menus/slots-time-unknown.png) | 标题栏写出游戏的文件名。左边是 4 个槽位，每个两行，第一行是槽位名，第二行是存档时板上时钟的时间。存档时时钟不可信的槽位写 `TIME UNKNOWN`，没有文件的写 `EMPTY`，没有通过校验的用红字写 `DAMAGED`。右边是光标所在槽位的缩略图，下面是 SAVE 和 LOAD 两个按钮。底栏说明光标所在的槽位：普通的槽位写出存档里 ROM 头的标题，空槽位和时间不明的槽位写一句说明，不能读的槽位用红字写出原因 |
| [slots-rom](menus/slots-rom.png)、[slots-version](menus/slots-version.png) | 存档里 ROM 头的标识和当前 ROM 不同时，第二行用琥珀色写 `OTHER ROM`，底栏也用琥珀色写出存档里的标题。这种槽位仍然可以读，但是读之前要确认。格式版本不同的槽位用红字写 `OTHER VERSION`，不能读，也没有缩略图 |
| [slots-color](menus/slots-color.png)、[slots-mode](menus/slots-mode.png) | 按 CGB 模式和兼容模式存的槽位，缩略图是彩色的。运行模式和当前游戏不同的槽位，第二行用红字写 `OTHER MODE`，不能读，缩略图照常显示。底栏用红字写出存档时 PALETTE 的设置：存档时按 GB 模式运行的写 `Can't load: PALETTE was not COLOR.`，按彩色运行的写 `Can't load: PALETTE was COLOR.`。0xC0 的游戏总是按 CGB 模式运行，不会出现这种槽位 |
| [slots-gba](menus/slots-gba.png) | GBA 游戏的缩略图是 240×160，在网格上占 120×80，比 GB 的宽，所以槽位的行变窄，SAVE 和 LOAD 跟着缩略图居中 |
| [slot-actions](menus/slot-actions.png)、[slot-actions-load](menus/slot-actions-load.png) | 在槽位上按 A，光标移到按钮上，默认选中 SAVE，槽位的选中条变成红框。左右切换按钮，底栏说明所选按钮的作用，B 回到槽位列表。空的、损坏的、其他版本的和运行模式不同的槽位，LOAD 是灰色的，光标停在上面时底栏用红字写出原因 |
| [slot-writing](menus/slot-writing.png)、[pause-slot-writing](menus/pause-slot-writing.png) | 选 SAVE 以后，光标回到这个槽位，第二行换成进度条，缩略图换成新的画面。写完之前不能开始新的存档或读档，所以选中条和两个按钮都是灰色，底栏提示可以先回到游戏。回到游戏以后，文件在后台接着写，这时再打开暂停菜单，SAVE / LOAD 右边用琥珀色显示写入的百分比，写完就不再显示 |
| [slots-no-card](menus/slots-no-card.png) | 没有卡时只显示说明 |

打开 SAVE / LOAD 时，后台读出 4 个槽位的文件并校验，还没读完的槽位第二行显示 `READING`。光标停在这一局上次存档或读档用过的槽位，没有用过时停在 SLOT 1。B 回到暂停菜单，MENU 和在 SETTINGS 里一样直接回到游戏。触摸时，SAVE 和 LOAD 按钮和列表的行一样，点一次选中，再点一次等于按 A。

缩略图是存档时的那一帧。GB 模式的槽位保存 160×144 的 4 个灰阶，显示时用当前 PALETTE 的颜色，PALETTE 选 COLOR 时用 GRAY。CGB 模式和兼容模式的槽位保存 160×144 的 BGR555，也就是游戏写进调色板 RAM 的颜色，不经过颜色校正，显示时按当前的 COLOR FIX 处理。GBA 的槽位保存 240×160 的 BGR555，COLOR FIX 对 GBA 游戏不起作用，所以按原样显示。缩略图按游戏的一个像素对应屏幕的一个像素来画，不跟着菜单放大 2 倍，所以 GB 的在网格上占 80×72，GBA 的占 120×80。菜单按原大小显示时，网格上只画得下一半大小的缩略图，取每 2×2 像素的左上角那个。空的、损坏的和其他版本的槽位只画一个空框。效果图里暂停的游戏画面和缩略图都是 `preview_scene.py` 的示意图经过翻转、平移得到的，不是模拟器的画面。

空槽位选 SAVE 以后直接开始写。槽位里已经有文件时要先确认，损坏的和其他版本的槽位也一样。选 SAVE 以后，模拟所在的核复制一份状态，编码和写卡在另一个核上进行，所以这时回到游戏不用等文件写完。QUIT 要等写完，见 dialog-slot-saving。

读档时，槽位这一行也显示进度条，读完以后直接回到游戏。ROM 标识不同时，读档之前要确认。读档以后卡带 RAM 换成了槽位里的内容，接下来的写回会覆盖卡上的 `.sav`，这一点不另外提示。带 RTC 的游戏读档以后，如果存档时和现在的板上时钟都可信，就把中间这段时间补算到游戏的时钟上，否则用提示框说明。

### 提示框

提示框只在游戏开始运行之前或者暂停时出现，不会和游戏画面同时占用窗口。

| 效果图 | 什么时候出现 | 按钮 |
|---|---|---|
| [dialog-load](menus/dialog-load.png) | 载入失败，比如读卡出错、文件太大或者内存不足，写出文件名和原因 | OK，回到列表 |
| [dialog-mismatch](menus/dialog-mismatch.png) | 有效存档里记录的 ROM 头标识和当前 ROM 不同，并排列出两边的标题 | LOAD SAVE、START NEW。选 START NEW 时，旧存档先复制成 `.sav.old` |
| [dialog-damaged](menus/dialog-damaged.png) | 卡上的存档文件（一份或两份）都没有通过校验，游戏从空白的卡带 RAM 开始，旧文件复制成 `.sav.old` | OK |
| [dialog-clock](menus/dialog-clock.png) | 带 RTC 的游戏启动时，板上的时钟不可信或者时间倒退，没有补算关机期间的时间。只在存档里记录了上次的时间时出现 | OK |
| [dialog-saving](menus/dialog-saving.png) | QUIT 时正在提交卡带存档 | 无 |
| [dialog-save-failed](menus/dialog-save-failed.png) | QUIT 时提交失败，写出原因 | RETRY、QUIT WITHOUT SAVING，默认选中 RETRY |
| [dialog-slot-overwrite](menus/dialog-slot-overwrite.png) | SAVE 的槽位里已经有文件 | REPLACE、CANCEL，默认选中 CANCEL |
| [dialog-slot-rom](menus/dialog-slot-rom.png) | 读取 `OTHER ROM` 的槽位之前，并排列出两边的标题 | LOAD、CANCEL，默认选中 CANCEL |
| [dialog-slot-clock](menus/dialog-slot-clock.png) | 带 RTC 的游戏读档以后，存档时或者现在的板上时钟不可信，或者时间倒退，没有把这段时间补算到游戏的时钟上。三种原因分别写 | OK，回到游戏 |
| [dialog-slot-failed](menus/dialog-slot-failed.png) | 暂停时写槽位失败，比如卡满了或者卡被拔掉，写出原因。槽位按卡上留下的文件显示，可能是空的，也可能是损坏的 | OK |
| [dialog-slot-saving](menus/dialog-slot-saving.png) | QUIT 时槽位还在写。写完以后再提交卡带存档，见 dialog-saving。这时写失败，弹出 dialog-slot-failed，按 OK 回到暂停菜单，不退出 | 无 |

运行中写回失败时不弹提示框，SAVE 灯变红、ERR 灯点亮，原因记进故障列表。回到游戏以后，后台写槽位失败也不弹提示框，ERR 灯点亮，原因记进故障列表。SAVE 灯和暂停菜单下方的存档状态只表示卡带存档，不受槽位影响。

### 声音

菜单操作有提示音。它们不是模拟出来的声音，所以不受 GB APU 的限制，是 48kHz、16 位的单声道采样，由 `tools/ui/menu_sounds.py` 用正弦泛音和衰减包络合成，不用第三方素材。五个提示音的响度相同，按 ITU-R BS.1770 的 K 加权计算最响的 200ms 里的能量，所以很短的 move 峰值比其他几个高。

| 声音 | 什么时候响 |
|---|---|
| move | 光标移动、翻页、调整数值或者切换选项；点一行只移动光标时也响 |
| confirm | A 或 START 有了结果，比如进入页面、启动游戏、关闭提示框；游戏中按 MENU 打开暂停菜单 |
| back | B 或 MENU 回到上一级，或者回到游戏 |
| denied | A 或 START 没有结果，比如选中灰色的行 |
| dialog | 弹出带按钮的提示框 |

光标已经在列表一端时，再按方向键不响。新的提示音会打断还在响的那个。没有游戏声音时，提示音单独输出；回到游戏时还没响完的部分叠加在游戏声音上。音量跟随 SETTINGS 的 VOLUME。

## 坐标和绘制

布局使用板子的逻辑像素坐标，具体位置和缩放以 `board.json` 为准。UI 先绘制，游戏画面独立合成，按 SCALING 选择整数倍最近邻或平滑缩放，UI 不参与游戏像素的混合。游戏画面按游戏窗口放得下的最大整数倍放大。游戏窗口一般是 480×320，GBA 放大 2 倍铺满，GB/GBC 放大 2 倍居中留黑边。Waveshare ESP32-C5-Touch-LCD-2.8 的游戏窗口是 240×160，画面不放大，GB/GBC 居中留黑边。

JC1060P470 的面板是 1024×600，UI 使用竖屏的 600×1024。板子按 USB 口朝下竖着拿，所以 UI 顺时针转 270° 显示在面板上，触摸坐标按同样的方向换算回 UI 坐标。这个方向还没有上板确认。Sila P4X 的面板是 452×1280，UI 使用横屏的 1280×452，同样顺时针转 270° 显示，方向见 [Sila P4X](../boards/sila_p4x.md#显示配置)。Sila P4、Tab5 和 OSPTEK ESP32-P4C5 的 UI 尺寸和面板一致，见 [Sila P4](../boards/sila_p4.md)、[Tab5](../boards/m5stack_tab5.md) 和 [OSPTEK ESP32-P4C5](../boards/osptek_p4c5.md)。慧勤智远 ESP32-S31 CB 的面板是 800×480，按面板扫描方向的右边朝上竖着拿，UI 顺时针转 90° 显示，见 [慧勤智远 ESP32-S31 CB](../boards/wks_s31_cb.md#显示配置)。乐鑫 ESP32-S31-Korvo-1 的面板也是 800×480，但按面板扫描方向的左边朝上竖着拿，UI 顺时针转 270° 显示，见 [乐鑫 ESP32-S31-Korvo-1](../boards/esp32_s31_korvo_1.md#显示配置)。Waveshare ESP32-C5-Touch-LCD-2.8 的 UI 尺寸和面板一致，都是 240×320，见 [Waveshare ESP32-C5-Touch-LCD-2.8](../boards/waveshare_c5_lcd28.md)。

sprite 的尺寸是画出来的轮廓，包括阴影，不是触摸范围。

## 触摸

控件的命中区域比图形大。命中形状取自控件拟合的几何形状，外面再加一圈边距，细的按钮会补到大约 10mm 宽。一个触点落在几个控件的范围里时，算给离它最近的控件，所以加宽的部分只占控件之间的空隙，不会抢走旁边控件图形上的触点。几个手指可以同时按几个控件。方向键按触点相对中心的角度分成 8 个方向。速度拨杆在手指落下时切换。

按下立即生效，松开有很短的防抖，这样触摸控制器偶尔漏报一次触点时，按键不会断开。参数在 `tools/ui/pack_assets.py` 和 `ui/controls.cpp` 里。

## 文件

`assets/ui/` 保存可用素材与布局，`raws/ui/` 保存原始图和布局来源，`tools/ui/` 保存生成工具。共用控件放在 `common/`，板子专用内容按型号分目录。

| 入口 | 用途 |
|---|---|
| `assets/ui/common/sprites.json` | 共用控件的图像、锚点和状态 |
| `assets/ui/common/vfd_font.json` | 点阵字体 |
| `assets/ui/common/menu.json` | 菜单的颜色、调色板和图标 |
| `assets/ui/common/menu_font.bin` | 菜单字体，由 `raws/ui/common/fonts/` 里的 BDF 生成 |
| `assets/ui/common/sounds/` | 菜单的提示音，由 `tools/ui/menu_sounds.py` 生成的 WAV 文件 |
| `assets/ui/<板子>/board.json` | 板子布局和显示字段绑定 |
| `raws/ui/<板子>/spec.json` | 布局与底图的生成配置 |

字段和图像处理规则以配置及生成脚本为准。

## 资源包

构建正式固件时，`tools/ui/pack_assets.py` 把所选板子的素材打成资源包，写进 assets 分区。sprite 在打包时就缩放到板子上的最终位置，并转换成 RGB565，固件把分区映射进地址空间以后直接交给 LVGL，不解码也不复制。包的格式由 `ui/include/gbx/ui/pack_format.h` 和 `pack_assets.py` 共同定义，改格式时两边一起改，并增加版本号。

## 生成和检查

原始图和局部修改素材保存在 `raws/ui/`。局部改图只合入需要的区域，避免无关部分随重新生成而变化。

在仓库根目录按顺序运行：

```sh
uv run tools/ui/build_sprites.py
uv run tools/ui/vfd_font.py
uv run tools/ui/menu_font.py
uv run tools/ui/menu_sounds.py
uv run tools/ui/export_boards.py
uv run tools/ui/render_previews.py
```

各脚本的用途见[工具说明](../../tools/README.md)，生成参数见脚本和 `spec.json`。哪些板子的主界面效果图有 WIRELESS GAMEPAD，由 `render_previews.py` 里的 `BLUETOOTH_GAMEPAD` 列出，板子的固件编入或者去掉蓝牙手柄时要一起改。改过素材以后，用下面的命令确认效果图和素材一致：

```sh
uv run tools/ui/render_previews.py --check
```

## 添加板子

1. 在 `raws/ui/<板子>/spec.json` 中定义布局，沿用共用控件和各项状态含义。游戏窗口是 480×320，屏幕宽度不到 480 时用 240×160。没有 VFD 的板子在 `spec.json` 里不写 `vfd`，底稿的 `pasteup` 里也不写 VFD 的 `windows`。
2. 准备板子底图。需要拼贴时可用 `tools/ui/pasteup.py`，原始素材保存在对应的 `raws/ui/` 目录。
3. 运行导出与预览工具，检查屏幕开口、控件、指示灯和 VFD 的位置，再上板确认显示方向与触摸。

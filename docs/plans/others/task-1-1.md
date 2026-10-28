# 任务 1-1：USB 游戏手柄

让 P4 板子经 USB host 口接标准 HID 游戏手柄，和触摸控件一起操作菜单和游戏。验收在 OSPTEK P4C5 上做，它的 USB-A 口一直有 5V。Sila P4 的 USB 口不给设备供电，见它的板级文档。功能由板级配置逐块打开，手柄在哪块板子的口上用过，才在那块板子上打开，其他板子只要求能构建。这个任务没有依赖。

开工时在[任务状态](status.md)里把这个任务标为“进行中”。

## 先读

- [OSPTEK P4C5 板级文档](../../boards/osptek_p4c5.md)的“烧录和恢复”一节：USB-A 和 J8 共用 P4 的高速 USB。
- `components/platform/include/gbx/display.h` 里 `platform::keys` 的约定，以及它的 ESP 实现。
- [UI 布局](../../ui-layouts/index.md#操作)的“操作”一节和 [PC 图形界面方案](../../host-gui.md)的键盘映射。
- [架构](../../architecture.md)的“双核分工”“运行游戏”和“代码规则”。

## 范围

- 用乐鑫的 USB Host 库读手柄，组件版本固定在 `idf_component.yml` 里。不用 `usb_host_hid` 类驱动，因为它在拔出时由自己的任务释放设备对象，而打开设备要等的控制传输在另一个任务里，两者会竞争。打开设备、读报告描述符、收报告和拔出后的清理都放在同一个客户端任务里。
- 通用 HID 游戏手柄按报告描述符解析十字键（hat switch）、左摇杆和按钮。按钮按 HID 编号映射，DualSense 和 DS4 的编号顺序也是这样：L1、R1 是 L、R，Options 是 Start，Create 或 Share 是 Select，PS 键是 MENU。
- 映射按物理位置，和 PC 版的键盘一致：右边的面键是 A，下边的面键是 B。左摇杆推过一半行程时当作方向键。
- 支持热插拔。手柄拔掉时，松开它按住的全部键。
- 手柄的按键经 `keys::read()` 提交，和触摸控件的按键合在一起，菜单和游戏里都能用。

## 不做

- XInput（Xbox 360 有线协议）和 GIP（Xbox One/Series 有线协议）。支持 D-input 模式的手柄切到这个模式就能用。
- 蓝牙手柄。
- 改键界面、摇杆的模拟量、震动、指示灯，以及同时使用多个手柄。
- 在 OSPTEK P4C5 以外的板子上打开。

## 约束

- USB 的中断和任务都在 core 0，模拟任务所在的 core 1 上不增加任务和中断。`keys::read()` 只读共享的按键状态，不等待 USB。
- 任务栈放在 PSRAM，USB 的 DMA 缓冲也放在 PSRAM，片内 RAM 留给 JIT 的代码池。
- 保持 `keys::read()` 的约定：两次读取之间按下又松开的键算按住一次。
- 功能由板级配置 `GBX_USB_GAMEPAD` 打开，没有打开的板子不编入 USB 的代码和组件。没有 USB OTG 的芯片上不能打开这个选项。
- 启动失败时撤销已经做的步骤，不留下装了一半的 USB 库。

## 完成标准

- 在 OSPTEK P4C5 上，手柄插上就能用：菜单里能移动、确认、返回和翻页，GB 和 GBA 游戏里全部按键都有效，MENU 能打开暂停菜单。游戏中拔掉再插上，按键不会卡住，手柄可以继续用；开机前就插着的手柄也能用。验证过的手柄型号写进结论。
- 关闭 `GBX_USB_GAMEPAD` 的板子构建通过，构建里没有 USB 组件。打开它的 OSPTEK 构建通过。都没有新的警告。
- 性能和片内 RAM 的影响按代码判断，不做实测，见结论。

## 收尾

- 满足全部完成标准以后，在[任务状态](status.md)里把这个任务标为“已完成”。
- 在架构文档里写明 USB 手柄的入口、开关和它所在的核，在 OSPTEK 和 Sila P4 的板级文档里写明 USB 口的供电。
- 向用户汇报支持的手柄和还没有解决的问题。

## 结论

OSPTEK P4C5（ESP32-P4 v3.2、400MHz）打开了 `GBX_USB_GAMEPAD`，用 debug 固件验证。DualSense（VID:PID `054c:0ce6`）经 USB-A 接入，报告描述符解析出 hat switch、左摇杆和 13 个按钮。用户在 2026-10-29 试过全部按键、菜单和游戏里的操作、游戏中拔插，以及插着手柄开机，都正常。其他型号的手柄没有验证。

关闭这个选项的构建用 `sila_p4-release` 检查过：组件管理器跳过了 `espressif/usb`，固件只比改动前大 48 字节，来自 `keys::start()` 的空实现和启动时的一次调用。改动的 PC 版文件用 clang++ 按 gnu++23 检查过语法。这台 Windows 机器上的 `build/host-debug` 配置成了 MSVC，编不过核心代码里的 GNU 扩展，和这次改动无关，所以 PC 版没有完整构建。

性能和片内 RAM 按代码判断，用户决定不做实测。打开选项时，USB 的中断和两个任务都在 core 0，DualSense 每 4ms 一份报告，每份报告是一次 64 字节的拷贝和查表；任务栈和传输缓冲在 PSRAM，片内 RAM 只多了两个任务控制块、USB 库自己的对象和 `hcd_dwc.c` 里约 1KB 的 `IRAM_ATTR` 函数。这些数字都没有实测。

Sila P4 的第二个 Type-C 口接的是 P4 的高速 USB，但是板子由串口供电时，插在这个口上的 DualSense 不亮，P4 也检测不到设备，所以 Sila P4 没有打开这个选项。

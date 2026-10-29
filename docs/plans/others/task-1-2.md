# 任务 1-2：P4 LP Core 蓝牙手柄

P4 板子经板上的 C5/C6 接蓝牙手柄。C5/C6 负责蓝牙 Controller，HCI 经 ESP-Hosted 传到 P4。P4 端的工作按运行时机分成两部分：配对这类只在菜单里或开机时做一次的事，由 HP 核用 ESP-IDF 自带的 NimBLE 和 esp_hosted 组件完成；游戏运行时一直要做的收发和按键解析由 LP Core 执行，代码自研，放在 LP SRAM 里。LP SRAM 放不下带配对的 NimBLE，而运行时只用到协议里很小的一部分。

依赖 [1-1 USB 游戏手柄](task-1-1.md)，复用按键映射、报告描述符的解析和 `keys::read()` 接口。开工时在[任务状态](status.md)中标为“进行中”。

## 分工

一次性的部分在 HP 核上运行，代码留在 flash 里：

- 配对：用户在菜单里选择配对以后，HP 核用 NimBLE 扫描、连接，完成 SMP 配对和绑定，发现 HID 服务，读 Report Map，算出按键布局。绑定信息、输入报告和 CCCD 的 handle、按键布局一起写进 NVS。配对完成后断开连接，停止 NimBLE。
- 开机：初始化 ESP-Hosted 和 Controller，从 NVS 读出配对结果放进 LP SRAM，再把传输交给 LP Core。

运行时的部分在 LP Core 上运行：

- 收发 ESP-Hosted 的帧，处理 HCI 事件和 ACL 数据。
- 连接绑定过的手柄，用保存的 LTK 开加密，打开输入报告的通知。手柄在游戏中休眠后又被唤醒时，也走这一步。
- 用 HP 核算好的布局解析输入报告，写进共享的按键状态。
- 连接断开时松开全部按键，然后重新连接。
- 回应手柄发来的连接参数更新、ATT 和 SMP 请求，不回应的话连接会超时断开。

连接事件的时序和加解密由 Controller 处理，LP Core 只要做到毫秒级的轮询。配对时 LP Core 先停下来交出传输，配对结束后 HP 核再交还给它。

## 范围和约束

- 首先支持一个 Xbox Series X|S 手柄（1914），记录验证时的固件版本。保留配对、链路加密和断连重连，不用无加密连接代替。
- NimBLE 和 esp_hosted 的版本固定在 `idf_component.yml` 里，所有板子用同一个版本。
- NimBLE 只在配对时启动，它的内存和任务栈放在 PSRAM，配对结束后释放。
- LP Core 的运行时代码、数据、栈和收发缓冲都放在 LP SRAM，允许调用 LP ROM 已有的函数，不借用 HP SRAM 或 PSRAM。
- 游戏运行时，HP 核只读取共享的按键状态，不承担蓝牙收发中断、协议处理或手柄解析。`keys::read()` 不等待蓝牙，两次读取之间的短按不能丢失。
- 暂不做多手柄、震动和其他蓝牙设备。功能按板级配置打开，没有打开的板子不编入蓝牙的代码和组件。

## 完成标准

1. 先在 `build/` 下验证 LP Core 能操作 ESP-Hosted 所用的传输控制器收发数据，再编译完整的 LP 程序，按实际可用的 LP SRAM 检查链接结果，计入栈、缓冲和共享区。任何一项不成立时，记录原因并重新评估方案，不自动改用 HP 内存。
2. 上面两项通过以后再上板验证：从菜单配对，重启后 LP Core 用保存的 LTK 重连；菜单和游戏里的操作正常；游戏中手柄断开以后没有卡键，唤醒后能重连。检查 LP Core 的栈和缓冲余量，测量对模拟速度的影响和 HP 核片内 RAM 的静态占用。验证通过后才在对应板子上默认打开，并更新任务状态。

## 参考

- [P4 LP Core 编程](https://docs.espressif.com/projects/esp-idf/en/latest/esp32p4/api-reference/system/ulp-lp-core.html)
- [ESP-Hosted 蓝牙传输](https://github.com/espressif/esp-hosted-mcu/blob/main/docs/features/bluetooth.md)
- [ESP-Hosted 迁移文档](https://github.com/espressif/esp-hosted-mcu/blob/main/docs/migration.md)

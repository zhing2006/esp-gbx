# 任务 1-2：P4 LP Core 蓝牙手柄

暂定用自研代码，在 P4 的 LP Core 和 LP SRAM 内完成蓝牙手柄接入。C5/C6 负责蓝牙 Controller，经 ESP-Hosted 传输 HCI。P4 端的传输驱动、BLE Host 和手柄处理都由 LP Core 执行。自研实现的容量和性能未实测。

依赖 [1-1 USB 游戏手柄](task-1-1.md)，复用按键映射和 `keys::read()` 接口。开工时在[任务状态](status.md)中标为“进行中”。

## 范围和约束

- 首先支持一个 Xbox Series X|S 手柄（1914），记录验证时的固件版本。保留配对、链路加密和断连重连，不用无加密连接代替。
- P4 端自行实现，只保留接入该手柄所需的协议功能，不引入完整 NimBLE 或 Bluedroid。
- 运行时代码、数据、栈和收发缓冲都放在 LP SRAM，允许调用 LP ROM 已有函数，不借用 HP SRAM 或 PSRAM。
- HP 核负责启动时加载、配置 LP 程序，运行时只读取共享按键状态，不承担蓝牙收发中断、协议栈或手柄解析任务。`keys::read()` 不等待蓝牙，两次读取之间的短按不能丢失。
- 暂不做多手柄、震动和其他蓝牙设备；功能按板级配置打开。

## 完成标准

1. 先在 `build/` 下编译完整 LP 程序，按实际可用 LP SRAM 检查链接结果，计入栈、缓冲和共享区。放不下时记录缺口，重新评估方案，不自动改用 HP 内存。
2. 容量通过后再上板验证：配对、输入、断连重连，以及菜单和游戏操作正常，断连后没有卡键。检查栈和缓冲余量，测量对模拟速度的影响；验证通过后才在对应板子上默认打开，并更新任务状态。

## 参考

- [P4 LP Core 编程](https://docs.espressif.com/projects/esp-idf/en/latest/esp32p4/api-reference/system/ulp-lp-core.html)
- [ESP-Hosted 蓝牙传输](https://github.com/espressif/esp-hosted-mcu/blob/main/docs/features/bluetooth.md)

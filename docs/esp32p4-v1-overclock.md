# ESP32-P4 v1.x 的 CPU 与 PSRAM 超频实验

本文适用于 ESP32-P4 v1.0、v1.3，修改位置以 ESP-IDF `v6.1` 为准。目标是让 CPU 尝试运行在 400MHz、PSRAM 尝试运行在 250MHz。

乐鑫的 [v1.3 规格书](https://documentation.espressif.com/esp32-p4-chip-revision-v1.3_datasheet_en.html)将 CPU 主频列为 360MHz，并说明需要 400MHz 芯片时应联系乐鑫；PSRAM 接口最高频率列为 200MHz。能写入更高的时钟配置，不代表具体芯片能稳定工作在该频率。

## 两组时钟分别配置

| 部分 | 默认配置 | 实验配置 | 时钟来源 |
| --- | --- | --- | --- |
| CPU | 360MHz | 400MHz | CPLL |
| PSRAM | 200MHz | 250MHz | MPLL 二分频，MPLL 从 400MHz 改为 500MHz |

CPU 调频不会自动改变 PSRAM 的配置。PSRAM 的 250MHz 模式还需要相应的读写等待周期和 DQS 时序校准。

v1.x 必须保留以下修订选择：

```ini
CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y
CONFIG_ESP32P4_REV_MIN_100=y
```

`LESS_V3` 同时控制寄存器定义、MPLL 初始化和电源管理的修订分支。为了显示 400MHz 或 250MHz 选项而把它关闭，会让软件使用 v3.x 的硬件配置。

## CPU 400MHz

### 不同 IDF 版本的配置项

ESP-IDF [v5.5.3 的 Kconfig.cpu](https://github.com/espressif/esp-idf/blob/v5.5.3/components/esp_system/port/soc/esp32p4/Kconfig.cpu)有以下两项：

```ini
CONFIG_ESP_FORCE_400MHZ_ON_REV_LESS_V3=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_400=y
```

第一项的菜单名称是“Force 400MHz on revision < 3.0 (EXPERIMENTAL)”，用来解锁旧修订的 400MHz 选项。但该版本的 [rtc_clk.c](https://github.com/espressif/esp-idf/blob/v5.5.3/components/esp_hw_support/port/esp32p4/rtc_clk.c)仍在 v1.x 分支中限制可用频率，不能仅凭菜单里有这个开关，就认为两项配置足以让 CPU 运行在 400MHz。

本项目使用的 [v6.1 Kconfig.cpu](https://github.com/espressif/esp-idf/blob/v6.1/components/esp_system/port/soc/esp32p4/Kconfig.cpu)没有 `ESP_FORCE_400MHZ_ON_REV_LESS_V3`，但有 `ESP_DEFAULT_CPU_FREQ_MHZ_400`。这里需要修改下面两处运行时代码，再选择 400MHz。

### v6.1 的两处修改

文件是 IDF 目录下的 `components/esp_hw_support/port/esp32p4/rtc_clk.c`。只修改 `CONFIG_ESP32P4_SELECTS_REV_LESS_V3` 为真的分支，保留原有的 90、180、360MHz 路径。

第一处是 `rtc_clk_cpu_freq_mhz_to_config()`。在 v1.x 分支的 `if (freq_mhz <= xtal_freq && freq_mhz != 0)` 之前加入：

```c
if (freq_mhz == 400) {
    *out_config = (rtc_cpu_freq_config_t) {
        .source = SOC_CPU_CLK_SRC_CPLL,
        .div = { .integer = 1 },
        .source_freq_mhz = 400,
        .freq_mhz = 400,
    };
    return true;
}
```

这让启动代码能够取得 400MHz 对应的 CPLL 配置。否则，原函数会返回 `false`，`esp_clk_init()` 随后的 `assert(res)` 会失败。

第二处是 `rtc_clk_cpu_freq_to_cpll_mhz()`。在 v1.x 分支的 `switch (cpu_freq_mhz)` 中，让 400MHz 和 360MHz 使用同一组总线分频：

```c
case 400:
case 360:
    mem_divider = 2;
    apb_divider = 2;
    break;
```

`sys_divider` 保持原来的 1。CPU 为 400MHz 时，MEM/SYS 为 200MHz，APB 为 100MHz。只修改第一处，仍会在第二处遇到不支持的频率并调用 `abort()`。

IDF 已经有 CPLL 的 400MHz 寄存器配置，继续使用 `rtc_clk_cpu_freq_set_config()` 的切换和校准流程即可，不需要在 `app_main()` 中另写调频代码。对应源码见 [v6.1 rtc_clk.c](https://github.com/espressif/esp-idf/blob/v6.1/components/esp_hw_support/port/esp32p4/rtc_clk.c)和 [clk_tree_ll.h](https://github.com/espressif/esp-idf/blob/v6.1/components/esp_hal_clock/esp32p4/include/hal/clk_tree_ll.h)。

## PSRAM 250MHz

v5.5.3 和 v6.1 的 P4 PSRAM 配置中都没有类似 CPU 的强制超频开关。`components/esp_psram/esp32p4/Kconfig.spiram` 对 250MHz 的限制是：

```kconfig
config SPIRAM_SPEED_250M
    depends on !ESP32P4_SELECTS_REV_LESS_V3
    bool "250MHz clock speed"
```

实验补丁可以只移除这一项的 `depends on` 限制，保留全局的 `ESP32P4_SELECTS_REV_LESS_V3=y`。然后选择 `CONFIG_SPIRAM_SPEED_250M=y`。不要同时保留 `CONFIG_SPIRAM_SPEED_200M=y`，它们属于同一个互斥选项组。

v6.1 已有配套实现：

| 修改位置 | 选择 250MHz 后的行为 |
| --- | --- |
| `components/esp_psram/device/esp_psram_impl_ap_hex.c` | 请求 500MHz MPLL，MSPI 二分频；配置读取延迟 18 周期、写入延迟 9 周期 |
| `components/esp_hw_support/mspi/mspi_timing_tuning/port/esp32p4/mspi_timing_tuning_configs.h` | 时序配置使用 500MHz MPLL |
| `components/esp_hw_support/mspi/mspi_timing_tuning/tuning_scheme_impl/mspi_timing_by_dqs.c` | 校准访问使用对应的等待周期 |
| `components/esp_hw_support/port/esp32p4/rtc_clk.c` | 因为保留 `LESS_V3=y`，仍调用 `clk_ll_mpll_set_config_v1()` |

PSRAM 的读取延迟编码是 6，对应 18 周期；写入延迟编码是 3，对应 9 周期。不要把寄存器编码直接改成 18 或 9，也不要只改 MPLL 频率而留下 200MHz 的时序参数。源码见 [Kconfig.spiram](https://github.com/espressif/esp-idf/blob/v6.1/components/esp_psram/esp32p4/Kconfig.spiram)、[PSRAM 驱动](https://github.com/espressif/esp-idf/blob/v6.1/components/esp_psram/device/esp_psram_impl_ap_hex.c)和 [DQS 校准](https://github.com/espressif/esp-idf/blob/v6.1/components/esp_hw_support/mspi/mspi_timing_tuning/tuning_scheme_impl/mspi_timing_by_dqs.c)。

## 项目配置和后续验证

应用上述 v6.1 实验补丁后，目标板的配置为：

```ini
CONFIG_IDF_TARGET="esp32p4"
CONFIG_ESP32P4_SELECTS_REV_LESS_V3=y
CONFIG_ESP32P4_REV_MIN_100=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_400=y
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_HEX=y
CONFIG_SPIRAM_SPEED_250M=y
CONFIG_SPIRAM_MEMTEST=y
# CONFIG_PM_ENABLE is not set
```

先使用固定 CPU 频率，不启用动态调频。IDF 对 AXI 和 PSRAM 频率的关系有额外处理，动态降频需要另行检查 `components/esp_hw_support/clk_utils.c`。

项目的 `sdkconfig.defaults` 不会覆盖构建目录里已经保存的选项。实验时需要同步更新目标 preset 的 `build/<preset>/sdkconfig`，或通过该 preset 的 `menuconfig` 重新选择频率。生成的 `sdkconfig` 不提交，长期保留的设置才写进板级 defaults。

验证时先测试 CPU 400MHz、PSRAM 200MHz，再测试 400MHz、250MHz，以便区分两项改动的影响。开机内存检查通过以后，还需要覆盖超过缓存容量的 PSRAM 读写、双核访问，以及实际使用的 DMA 和显示负载。模拟器的功能测试按[硬件测试指南](hardware-testing.md)执行，解释器和 JIT 都要运行。

启动日志中的 CPU 频率和 PSRAM `Speed` 可以确认所选配置；其中 PSRAM `Speed` 打印的是配置值，不能单凭这行日志判断实际频率和稳定性。PLL 校准或 PSRAM 初始化失败时，先恢复低频，不要通过删除断言继续运行。

恢复 v1.x 的常规配置时，选择 CPU 360MHz、PSRAM 200MHz，并撤销实验补丁。现有 `sila_p4`、`jc1060p470` 等 v1.x 板子的默认配置不需要为了这份说明而修改。

`sila_p4x` 使用 v3.x 芯片，项目配置为 CPU 400MHz、PSRAM 250MHz；它不需要本文的 v1.x 补丁。

## 验证情况

在 Sila P4（ESP32-P4 v1.3）上，用 ESP-IDF v6.1 加上本文的两处补丁，按上面的配置构建了 `sila_p4` 的 release 固件，CPU 400MHz 和 PSRAM 250MHz 同时启用，没有先单独测试 400MHz、200MHz。固件启动正常，开机的 PSRAM 内存检查通过，手动试用没有发现问题。模拟器的测试套件、长时间运行，以及双核和 DMA 负载下的稳定性未实测。release 固件的日志级别是 WARN，启动日志里没有 CPU 频率和 PSRAM `Speed` 这两行。

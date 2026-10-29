#include "gbx/esp/storage.h"

#include <cinttypes>

#include "driver/sdspi_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "gbx/esp/board.h"
#include "sdmmc_cmd.h"
#include "sdkconfig.h"
#include "soc/soc_caps.h"
#if SOC_SDMMC_HOST_SUPPORTED
#include "driver/sdmmc_host.h"
#endif
#if CONFIG_GBX_BT_GAMEPAD
#include "bt/sdmmc_share.h"
#endif

namespace gbx::platform::storage {
namespace {

constexpr const char* kTag = "storage";
sdmmc_card_t* card = nullptr;

#if SOC_SDMMC_HOST_SUPPORTED
esp_err_t mount_sdmmc(const board::Sdmmc& wiring, const esp_vfs_fat_mount_config_t& mount) {
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = wiring.slot;
    // 40 MHz high-speed at 3.3 V, not UHS. Keep DEINIT_ARG from the default.
    host.flags &= ~(SDMMC_HOST_FLAG_8BIT | SDMMC_HOST_FLAG_DDR);
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    host.io_voltage = 3.3f;
    host.pwr_ctrl_handle = nullptr;
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk = static_cast<gpio_num_t>(wiring.clk);
    slot.cmd = static_cast<gpio_num_t>(wiring.cmd);
    slot.d0 = static_cast<gpio_num_t>(wiring.data[0]);
    slot.d1 = static_cast<gpio_num_t>(wiring.data[1]);
    slot.d2 = static_cast<gpio_num_t>(wiring.data[2]);
    slot.d3 = static_cast<gpio_num_t>(wiring.data[3]);
    slot.cd = SDMMC_SLOT_NO_CD;
    slot.wp = SDMMC_SLOT_NO_WP;
    // Assist the lines, but do not assume these replace external pull-ups.
    // Sila's schematic does not establish the external resistor population.
    slot.flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP;  // No UHS/1.8 V signaling.
#if CONFIG_GBX_BT_GAMEPAD
    // The co-processor's slot shares the controller with the LP core.
    sdmmc_share::wrap(host);
    sdmmc_share::lock();
    const esp_err_t err = esp_vfs_fat_sdmmc_mount(kSdMountPath, &host, &slot, &mount, &card);
    sdmmc_share::unlock();
    return err;
#else
    return esp_vfs_fat_sdmmc_mount(kSdMountPath, &host, &slot, &mount, &card);
#endif
}
#endif

// The board has set up the bus; unmounting leaves it to the other devices.
esp_err_t mount_sdspi(const board::Sdmmc& wiring, const esp_vfs_fat_mount_config_t& mount) {
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = wiring.slot;
    // Default speed, 20 MHz: high speed in SPI mode needs CMD6, which the
    // driver does not send.
    host.max_freq_khz = SDMMC_FREQ_DEFAULT;
    sdspi_device_config_t slot = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot.host_id = static_cast<spi_host_device_t>(wiring.slot);
    slot.gpio_cs = static_cast<gpio_num_t>(wiring.cs);
    return esp_vfs_fat_sdspi_mount(kSdMountPath, &host, &slot, &mount, &card);
}

}  // namespace

bool mount_sd() {
    if (card != nullptr) {
        return true;
    }
    const auto wiring = board::sdmmc();
    esp_vfs_fat_mount_config_t mount = VFS_FAT_MOUNT_DEFAULT_CONFIG();
    mount.format_if_mount_failed = false;
    mount.max_files = 4;
    esp_err_t err = ESP_ERR_NOT_SUPPORTED;
    if (wiring.bus == board::Sdmmc::Bus::spi) {
        err = mount_sdspi(wiring, mount);
    } else {
#if SOC_SDMMC_HOST_SUPPORTED
        err = mount_sdmmc(wiring, mount);
#endif
    }
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "SD mount failed: %s; check card and FAT32 filesystem; no formatting attempted",
                 esp_err_to_name(err));
        card = nullptr;
        return false;
    }
    ESP_LOGI(kTag, "SD mounted at %s: %.5s, %" PRIu64 " bytes, %d kHz, %u-bit, fixed 3.3 V",
             kSdMountPath, card->cid.name,
             static_cast<uint64_t>(card->csd.capacity) * card->csd.sector_size,
             card->real_freq_khz, 1U << card->log_bus_width);
    return true;
}

void unmount_sd() {
    if (card == nullptr) {
        return;
    }
#if CONFIG_GBX_BT_GAMEPAD
    sdmmc_share::lock();
#endif
    esp_err_t err = esp_vfs_fat_sdcard_unmount(kSdMountPath, card);
#if CONFIG_GBX_BT_GAMEPAD
    sdmmc_share::unlock();
#endif
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "SD unmount: %s", esp_err_to_name(err));
    }
    card = nullptr;
}

}  // namespace gbx::platform::storage

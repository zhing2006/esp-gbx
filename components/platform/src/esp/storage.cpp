#include "gbx/esp/storage.h"

#include <cinttypes>

#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "gbx/esp/board.h"
#include "sdmmc_cmd.h"

namespace gbx::platform::storage {
namespace {

constexpr const char* kTag = "storage";
sdmmc_card_t* card = nullptr;

}  // namespace

bool mount_sd() {
    if (card != nullptr) {
        return true;
    }
    const auto wiring = board::sdmmc();
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
    esp_vfs_fat_mount_config_t mount = VFS_FAT_MOUNT_DEFAULT_CONFIG();
    mount.format_if_mount_failed = false;
    mount.max_files = 4;
    esp_err_t err = esp_vfs_fat_sdmmc_mount(kSdMountPath, &host, &slot, &mount, &card);
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
    esp_err_t err = esp_vfs_fat_sdcard_unmount(kSdMountPath, card);
    if (err != ESP_OK) {
        ESP_LOGW(kTag, "SD unmount: %s", esp_err_to_name(err));
    }
    card = nullptr;
}

}  // namespace gbx::platform::storage

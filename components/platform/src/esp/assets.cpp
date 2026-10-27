#include "gbx/display.h"

#include "esp_log.h"
#include "esp_partition.h"

namespace gbx::platform {

namespace {

constexpr const char* kTag = "assets";
constexpr auto kAssetsSubtype = static_cast<esp_partition_subtype_t>(0x40);  // see partitions.csv

}  // namespace

std::span<const std::byte> asset_pack() {
    static std::span<const std::byte> mapped;
    if (!mapped.empty()) {
        return mapped;
    }
    const esp_partition_t* partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA, kAssetsSubtype, "assets");
    if (partition == nullptr) {
        ESP_LOGE(kTag, "no assets partition in the partition table");
        return {};
    }
    const void* data = nullptr;
    esp_partition_mmap_handle_t handle;
    esp_err_t err = esp_partition_mmap(partition, 0, partition->size, ESP_PARTITION_MMAP_DATA, &data, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "cannot map the assets partition: %s", esp_err_to_name(err));
        return {};
    }
    mapped = {static_cast<const std::byte*>(data), partition->size};
    return mapped;
}

}  // namespace gbx::platform

#include "console.h"
#include "esp_chip_info.h"
#include "esp_log.h"
#include "esp_psram.h"
#include "gbx/app.h"
#include "gbx/esp/board.h"

namespace {

constexpr const char* kTag = "main";

}  // namespace

extern "C" void app_main(void) {
    if (!gbx::board::init()) {
        ESP_LOGE(kTag, "Board initialization failed");
        return;
    }

    esp_chip_info_t chip;
    esp_chip_info(&chip);
    ESP_LOGI(kTag, "%s, chip revision v%d.%d, %u MiB PSRAM", gbx::board::name(), chip.revision / 100,
             chip.revision % 100, static_cast<unsigned>(esp_psram_get_size() >> 20));

    gbx::console::start();
    gbx::app::run();
}

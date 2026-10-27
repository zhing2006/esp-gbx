#include "lvgl.h"

#include <cstring>

#include "esp_log.h"
#include "src/core/lv_global.h"
#include "src/misc/lv_area_private.h"

namespace gbx::platform::display::detail {
namespace {

// LVGL 9.6.0~1 exposes draw-unit callbacks but no unit lookup API. Keep this
// version-specific adapter here rather than modifying a managed component.
int32_t (*ppa_evaluate)(lv_draw_unit_t*, lv_draw_task_t*) = nullptr;

int32_t evaluate(lv_draw_unit_t* unit, lv_draw_task_t* task) {
    lv_area_t clipped;
    if (!lv_area_intersect(&clipped, &task->area, &task->clip_area)) return 0;
    const int64_t pixels = int64_t{lv_area_get_width(&clipped)} * lv_area_get_height(&clipped);
    // The first Sila P4 measurement showed a regression for the roughly 2K
    // pixel volume updates, unlike large canvas copies. The threshold is
    // measured independently from the presentation-copy threshold.
    if (pixels < CONFIG_GBX_LVGL_PPA_MIN_PIXELS) return 0;
    // The pinned fill implementation uses header.w as pitch. Unlike its
    // image implementation it cannot address padded rows correctly.
    if (task->type == LV_DRAW_TASK_TYPE_FILL && task->target_layer->draw_buf) {
        const auto& h = task->target_layer->draw_buf->header;
        if (h.stride != h.w * lv_color_format_get_size(static_cast<lv_color_format_t>(h.cf))) return 0;
    }
    return ppa_evaluate(unit, task);
}

}  // namespace

bool configure_lvgl_acceleration() {
    for (auto* unit = LV_GLOBAL_DEFAULT()->draw_info.unit_head; unit; unit = unit->next) {
        if (unit->name && std::strcmp(unit->name, "ESP_PPA") == 0) {
            if (unit->evaluate_cb != evaluate) {
                ppa_evaluate = unit->evaluate_cb;
                unit->evaluate_cb = evaluate;
            }
            return true;
        }
    }
    ESP_LOGE("display", "the configured LVGL PPA draw unit is missing");
    return false;
}

}  // namespace gbx::platform::display::detail

// LVGL's heap, selected by CONFIG_LV_USE_CUSTOM_MALLOC. LVGL keeps its
// objects, styles, timers and draw tasks here, and the UI touches them a few
// times a frame at most. The C library's malloc() would put every block up
// to CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL bytes in internal RAM; these take
// PSRAM first and internal RAM only when PSRAM is full.
#include "esp_heap_caps.h"
#include "lvgl.h"

namespace {
constexpr uint32_t kFirst = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
constexpr uint32_t kSecond = MALLOC_CAP_DEFAULT;
}  // namespace

extern "C" {

void lv_mem_init(void) {}
void lv_mem_deinit(void) {}
lv_mem_pool_t lv_mem_add_pool(void*, size_t) { return nullptr; }
void lv_mem_remove_pool(lv_mem_pool_t) {}
void* lv_malloc_core(size_t size) { return heap_caps_malloc_prefer(size, 2, kFirst, kSecond); }
void* lv_realloc_core(void* p, size_t size) { return heap_caps_realloc_prefer(p, size, 2, kFirst, kSecond); }
void lv_free_core(void* p) { heap_caps_free(p); }
void lv_mem_monitor_core(lv_mem_monitor_t*) {}
lv_result_t lv_mem_test_core(void) { return LV_RESULT_OK; }

}  // extern "C"

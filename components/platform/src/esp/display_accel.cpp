#include "display_backend.h"
#include "display_pixels.h"

#include <atomic>
#include <cstring>
#include <cstdlib>

#include "driver/ppa.h"
#include "esp_async_color_convert.h"
#include "esp_cache.h"
#include "esp_private/esp_cache_private.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "soc/soc_caps.h"

#if !SOC_PPA_SUPPORTED || !SOC_DMA2D_SUPPORTED
#error "The accelerated display backend requires PPA and DMA2D"
#endif

namespace gbx::platform::display::detail {
namespace {

size_t cache_alignment = 0;
ppa_client_handle_t srm = nullptr, fill = nullptr;
async_color_convert_handle_t copier = nullptr;
SemaphoreHandle_t done = nullptr;
// Native GB/GBC and GBA frames fit.
constexpr size_t kSourcePixels = 240 * 160;
// DMA2D descriptors hold widths, heights and offsets in 14 bits.
constexpr int kDma2dFieldMax = 0x3fff;
// The game frame the CPU prepares for the hardware: RGB565 at the source
// size for SMOOTH, the rows of widen_rows() for NEAREST. The present task
// owns it, lends it to run_frame_step(), and grows it when a NEAREST frame
// needs more.
uint16_t* staging = nullptr;
size_t staging_pixels = 0;
std::atomic<size_t> staging_bytes{0};

bool reserve_staging(size_t pixels) {
    if (pixels <= staging_pixels) return true;
    auto* grown = static_cast<uint16_t*>(heap_caps_aligned_alloc(cache_alignment, pixels * sizeof(uint16_t),
                                                                 MALLOC_CAP_SPIRAM));
    if (grown == nullptr) return false;
    heap_caps_free(staging);
    staging = grown;
    staging_pixels = pixels;
    staging_bytes.store(pixels * sizeof(uint16_t), std::memory_order_relaxed);
    return true;
}

#ifdef GBX_PROFILER
struct Counters {
    std::atomic<uint32_t> submit{0}, complete{0}, count{0}, maximum{0};
    void finish(int64_t start) {
        const auto us = static_cast<uint32_t>(esp_timer_get_time() - start);
        complete.fetch_add(us);
        count.fetch_add(1);
        uint32_t old = maximum.load();
        while (old < us && !maximum.compare_exchange_weak(old, us)) {}
    }
};
Counters copy_times, rotate_times, fill_times, scale_times, rows_times;
#define PROFILE(...) __VA_ARGS__
#else
#define PROFILE(...)
#endif

bool completed() {
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(done, &woken);
    return woken == pdTRUE;
}

bool ppa_done(ppa_client_handle_t, ppa_event_data_t*, void*) { return completed(); }
bool copy_done(async_color_convert_handle_t, async_color_convert_event_data_t*, void*) { return completed(); }

void wait() {
    // A timeout is not cancellation. Never reuse either buffer after one:
    // v6.1 has no cancellation API that establishes the required ownership.
    if (xSemaphoreTake(done, pdMS_TO_TICKS(1000)) != pdTRUE) {
        ESP_LOGE("display", "hardware operation timed out; buffers remain owned by DMA");
        std::abort();
    }
}

Rect panel_rect(const Config& config, const Rect& r) {
    switch (config.panel.rotation) {
        case board::Rotation::none: return r;
        case board::Rotation::cw90:
            return {config.ui.height - r.y1, r.x0, config.ui.height - r.y0, r.x1};
        case board::Rotation::cw180:
            return {config.ui.width - r.x1, config.ui.height - r.y1,
                    config.ui.width - r.x0, config.ui.height - r.y0};
        case board::Rotation::cw270:
            return {r.y0, config.ui.width - r.x1, r.y1, config.ui.width - r.x0};
    }
    std::abort();
}

ppa_srm_rotation_angle_t rotation(const Config& config) {
    // PPA angles are counter-clockwise; the board interface is clockwise.
    switch (config.panel.rotation) {
        case board::Rotation::none: return PPA_SRM_ROTATION_ANGLE_0;
        case board::Rotation::cw90: return PPA_SRM_ROTATION_ANGLE_270;
        case board::Rotation::cw180: return PPA_SRM_ROTATION_ANGLE_180;
        case board::Rotation::cw270: return PPA_SRM_ROTATION_ANGLE_90;
    }
    std::abort();
}

ppa_out_pic_blk_config_t output(uint16_t* fb, const Config& config, const Rect& r) {
    ppa_out_pic_blk_config_t out{};
    out.buffer = fb;
    out.buffer_size = config.panel.width * config.panel.height * sizeof(uint16_t);
    out.pic_w = config.panel.width;
    out.pic_h = config.panel.height;
    out.block_offset_x = r.x0;
    out.block_offset_y = r.y0;
    return out;
}

void preserve_rows(uint16_t* fb, const Config& config, const Rect& r) {
    // IDF v6.1 SRM invalidates complete destination rows BEFORE DMA, without
    // writing them back. Preserve CPU pixels outside this rectangle on those
    // rows, including the partial cache lines at both ends. The present task
    // owns the entire back buffer and makes no accesses until wait() returns.
    const uintptr_t first = reinterpret_cast<uintptr_t>(fb + r.y0 * config.panel.width);
    const uintptr_t last = reinterpret_cast<uintptr_t>(fb + r.y1 * config.panel.width);
    const uintptr_t begin = first & ~(cache_alignment - 1);
    const uintptr_t end = (last + cache_alignment - 1) & ~(cache_alignment - 1);
    ESP_ERROR_CHECK(esp_cache_msync(reinterpret_cast<void*>(begin), end - begin,
                                  ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_INVALIDATE));
}

}  // namespace

bool backend_start() {
    ESP_ERROR_CHECK(esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &cache_alignment));
    done = xSemaphoreCreateBinary();
    if (!done || !reserve_staging(kSourcePixels)) {
        backend_stop();
        return false;
    }
    ppa_client_config_t client{};
    client.oper_type = PPA_OPERATION_SRM;
    client.max_pending_trans_num = 1;
    client.data_burst_length = PPA_DATA_BURST_LENGTH_128;
    async_color_convert_config_t dma{};
    // DMA2D returns the transaction to its pool after calling copy_done.
    // Keep a second slot so a waiter on another core can submit immediately.
    dma.backlog = 2;
    dma.dma_burst_size = 128;
    const ppa_event_callbacks_t callbacks{.on_trans_done = ppa_done};
    esp_err_t error = ppa_register_client(&client, &srm);
    if (error == ESP_OK) error = ppa_client_register_event_callbacks(srm, &callbacks);
    client.oper_type = PPA_OPERATION_FILL;
    if (error == ESP_OK) error = ppa_register_client(&client, &fill);
    if (error == ESP_OK) error = ppa_client_register_event_callbacks(fill, &callbacks);
    if (error == ESP_OK) error = esp_async_color_convert_install_dma2d(&dma, &copier);
    if (error != ESP_OK) {
        ESP_LOGE("display", "cannot initialize accelerated backend: %s", esp_err_to_name(error));
        backend_stop();
        return false;
    }
    return true;
}

void backend_stop() {
    if (copier) {
        // The last completion can wake a different core before DMA2D retires
        // its transaction. No new work is submitted while closing the backend.
        esp_err_t error;
        unsigned attempts = 0;
        while ((error = esp_async_color_convert_uninstall(copier)) == ESP_ERR_INVALID_STATE && attempts++ < 10)
            vTaskDelay(1);
        ESP_ERROR_CHECK(error);
        copier = nullptr;
    }
    if (fill) { ESP_ERROR_CHECK(ppa_unregister_client(fill)); fill = nullptr; }
    if (srm) { ESP_ERROR_CHECK(ppa_unregister_client(srm)); srm = nullptr; }
    if (done) { vSemaphoreDelete(done); done = nullptr; }
    heap_caps_free(staging);
    staging = nullptr;
    staging_pixels = 0;
    staging_bytes.store(0, std::memory_order_relaxed);
}

bool game_size_supported(int width, int height) {
    return static_cast<size_t>(width) * height <= kSourcePixels;
}
#ifdef GBX_PROFILER
size_t backend_buffer_bytes() { return staging_bytes.load(std::memory_order_relaxed); }
#endif

void copy_ui_rect(uint16_t* fb, const Config& config, const Rect& r) {
    const int width = r.x1 - r.x0;
    const int height = r.y1 - r.y0;
    // Sila P4: warm narrow/short rectangles lose to DMA setup and full-row
    // cache maintenance; a 480x320 copy and a 32x480 strip benefit. Retain
    // CPU row processing below that conservative measured boundary.
    // Tall VFD updates also benefited in the running-game measurement.
    const bool small = width * height < 32768 && height < 128;
    if (small) {
        put_rect(fb, config, r);
        return;
    }
    copy_ui_rect_accelerated(fb, config, r);
}

void copy_ui_rect_accelerated(uint16_t* fb, const Config& config, const Rect& r) {
    PROFILE(const int64_t start = esp_timer_get_time();)
    if (config.panel.rotation == board::Rotation::none) {
        async_color_convert_request_t request{};
        // DMA2D synchronizes the entire declared picture. Declare only the
        // affected full-width row strip, not a whole screen per dirty rect.
        // Pixel offsets remain within that strip; no source pixels are copied
        // to an intermediate buffer and no unrelated screen rows are flushed.
        request.src_buffer = config.shadow + r.y0 * config.ui.width;
        request.src_stride = config.ui.width;
        request.src_height = r.y1 - r.y0;
        request.src_x = r.x0;
        request.dst_buffer = fb + r.y0 * config.panel.width;
        request.dst_stride = config.panel.width;
        request.dst_height = r.y1 - r.y0;
        request.dst_x = r.x0;
        request.copy_width = r.x1 - r.x0;
        request.copy_height = r.y1 - r.y0;
        request.src_color_format = ESP_COLOR_FOURCC_RGB16;
        request.dst_color_format = ESP_COLOR_FOURCC_RGB16;
        ESP_ERROR_CHECK(esp_async_color_convert(copier, &request, copy_done, nullptr));
        PROFILE(copy_times.submit.fetch_add(static_cast<uint32_t>(esp_timer_get_time() - start));)
        wait();
        PROFILE(copy_times.finish(start);)
    } else {
        const Rect dst = panel_rect(config, r);
        preserve_rows(fb, config, dst);
        ppa_srm_oper_config_t request{};
        request.in.buffer = config.shadow;
        request.in.pic_w = config.ui.width;
        request.in.pic_h = config.ui.height;
        request.in.block_w = r.x1 - r.x0;
        request.in.block_h = r.y1 - r.y0;
        request.in.block_offset_x = r.x0;
        request.in.block_offset_y = r.y0;
        request.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
        request.out = output(fb, config, dst);
        request.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
        request.rotation_angle = rotation(config);
        request.scale_x = request.scale_y = 1;
        request.mode = PPA_TRANS_MODE_NON_BLOCKING;
        ESP_ERROR_CHECK(ppa_do_scale_rotate_mirror(srm, &request));
        PROFILE(rotate_times.submit.fetch_add(static_cast<uint32_t>(esp_timer_get_time() - start));)
        wait();
        PROFILE(rotate_times.finish(start);)
    }
}

void fill_black(uint16_t* fb, const Config& config, const Rect& r) {
    PROFILE(const int64_t start = esp_timer_get_time();)
    const Rect dst = panel_rect(config, r);
    ppa_fill_oper_config_t request{};
    request.out = output(fb, config, dst);
    request.out.fill_cm = PPA_FILL_COLOR_MODE_RGB565;
    request.fill_block_w = dst.x1 - dst.x0;
    request.fill_block_h = dst.y1 - dst.y0;
    request.fill_color_val = 0;
    request.mode = PPA_TRANS_MODE_NON_BLOCKING;
    // Unlike SRM, v6.1 fill already writes back and invalidates its row
    // envelope. No CPU may refill those cache lines before completion.
    ESP_ERROR_CHECK(ppa_do_fill(fill, &request));
    PROFILE(fill_times.submit.fetch_add(static_cast<uint32_t>(esp_timer_get_time() - start));)
    wait();
    PROFILE(fill_times.finish(start);)
}

namespace {

struct FrameStep {
    const Config* config;
    const GameLayer* layer;
    const uint16_t* frame;
};

void widen_step(void* arg) {
    const auto& step = *static_cast<const FrameStep*>(arg);
    widen_rows(staging, *step.config, *step.layer, step.frame);
}

void convert_step(void* arg) {
    const auto& step = *static_cast<const FrameStep*>(arg);
    const size_t count = static_cast<size_t>(step.layer->width) * step.layer->height;
    const uint16_t* frame = step.frame;
    if (!step.layer->colors && !(reinterpret_cast<uintptr_t>(frame) & 3) && !(count & 1)) {
        // Without a color table, two pixels a word, as to_rgb565() does
        // each: GBA frames change color at nearly every pixel, where the
        // runs below do not pay off.
        typedef uint32_t __attribute__((may_alias)) Word;
        const auto* in = reinterpret_cast<const Word*>(frame);
        auto* out = reinterpret_cast<Word*>(staging);
        for (size_t i = 0; i < count / 2; ++i) {
            const uint32_t c = in[i];
            const uint32_t g = (c >> 5) & 0x001f001fu;
            out[i] = (c & 0x001f001fu) << 11 | ((g << 1) | ((g >> 4) & 0x00010001u)) << 5 | ((c >> 10) & 0x001f001fu);
        }
        return;
    }
    uint16_t last = static_cast<uint16_t>(frame[0] ^ 1u);
    uint16_t color = 0;
    for (size_t i = 0; i < count; ++i) {
        uint16_t pixel = frame[i];
        if (pixel != last) {
            last = pixel;
            // Reuse long runs of one color, as in draw_nearest. Keep GCC 15
            // from hoisting the conversion out of the changed-color branch.
            asm volatile("" : "+r"(pixel));
            color = game_rgb565(pixel, *step.layer);
        }
        staging[i] = color;
    }
}

// NEAREST: the CPU writes each run of equal panel rows once, and DMA2D
// copies it into the run's rows, one copy for each. The CPU writes 1 / scale
// of the image and leaves no dirty cache lines in the frame buffer for the
// panel flush to write back. Drawing every pixel on the CPU is bound by
// PSRAM: on Sila P4 with a GB game this took the present task from about
// 23 % of core 0 to 14 %.
void draw_nearest_rows(uint16_t* fb, const Config& config, const GameLayer& layer) {
    const Rect dst = panel_rect(config, layer.image);
    const int s = layer.scale;
    const int width = dst.x1 - dst.x0;
    const int rows = (dst.y1 - dst.y0) / s;
    for (int k = 0; k < s; ++k) {
        PROFILE(const int64_t start = esp_timer_get_time();)
        // One DMA2D row is a run of `s` panel rows, and copy k fills the
        // k-th of each run. The declared picture is exactly the image's
        // panel rows, which the driver writes back and invalidates first.
        async_color_convert_request_t request{};
        request.src_buffer = staging;
        request.src_stride = width;
        request.src_height = rows;
        request.dst_buffer = fb + dst.y0 * config.panel.width;
        request.dst_stride = config.panel.width * s;
        request.dst_height = rows;
        request.dst_x = k * config.panel.width + dst.x0;
        request.copy_width = width;
        request.copy_height = rows;
        request.src_color_format = ESP_COLOR_FOURCC_RGB16;
        request.dst_color_format = ESP_COLOR_FOURCC_RGB16;
        ESP_ERROR_CHECK(esp_async_color_convert(copier, &request, copy_done, nullptr));
        PROFILE(rows_times.submit.fetch_add(static_cast<uint32_t>(esp_timer_get_time() - start));)
        wait();
        PROFILE(rows_times.finish(start);)
    }
}

}  // namespace

void draw_game(uint16_t* fb, const Config& config, const GameLayer& layer, const uint16_t* frame) {
    FrameStep step{&config, &layer, frame};
    if (layer.scaling == Scaling::nearest) {
        // Without the buffer, or past the descriptors' limits, the CPU draws.
        if (config.panel.width * layer.scale > kDma2dFieldMax ||
            !reserve_staging(static_cast<size_t>(layer.width) * layer.height * layer.scale)) {
            draw_nearest(fb, config, layer, frame);
            return;
        }
        PROFILE(set_present_stage(PresentStage::convert);)
        run_frame_step(widen_step, &step);
        PROFILE(set_present_stage(PresentStage::rows_wait);)
        draw_nearest_rows(fb, config, layer);
        return;
    }
    // An RGB565 frame goes to the PPA as it is.
    if (!layer.rgb565) {
        PROFILE(set_present_stage(PresentStage::convert);)
        run_frame_step(convert_step, &step);
    }
    PROFILE(set_present_stage(PresentStage::ppa_submit);
            const int64_t start = esp_timer_get_time();)
    const Rect dst = panel_rect(config, layer.image);
    preserve_rows(fb, config, dst);
    ppa_srm_oper_config_t request{};
    request.in.buffer = layer.rgb565 ? frame : staging;
    request.in.pic_w = request.in.block_w = layer.width;
    request.in.pic_h = request.in.block_h = layer.height;
    request.in.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    request.out = output(fb, config, dst);
    request.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;
    request.rotation_angle = rotation(config);
    request.scale_x = request.scale_y = layer.scale;
    request.mode = PPA_TRANS_MODE_NON_BLOCKING;
    ESP_ERROR_CHECK(ppa_do_scale_rotate_mirror(srm, &request));
    PROFILE(scale_times.submit.fetch_add(static_cast<uint32_t>(esp_timer_get_time() - start));
            set_present_stage(PresentStage::ppa_wait);)
    wait();
    PROFILE(scale_times.finish(start);)
}

}  // namespace gbx::platform::display::detail

#ifdef GBX_PROFILER
namespace gbx::platform::display {
HardwareTimes take_hardware_times() {
    const auto take = [](detail::Counters& t) {
        return HardwareOperationTimes{t.submit.exchange(0), t.complete.exchange(0), t.count.exchange(0),
                                      t.maximum.exchange(0)};
    };
    return {take(detail::copy_times), take(detail::rotate_times), take(detail::fill_times), take(detail::scale_times),
            take(detail::rows_times)};
}
}  // namespace gbx::platform::display
#endif

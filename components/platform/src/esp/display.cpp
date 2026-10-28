#include "gbx/display.h"

#include <algorithm>
#include <atomic>

#include "display_backend.h"
#include "display_pixels.h"
#include "esp_attr.h"
#include "esp_private/esp_cache_private.h"
#include "esp_heap_caps.h"
#include "esp_lcd_panel_commands.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gbx/esp/board.h"
#include "gbx/platform.h"
#include "lvgl.h"
#include "soc/soc_caps.h"
#include "task_stack.h"
#if SOC_MIPI_DSI_SUPPORTED
#include "esp_lcd_mipi_dsi.h"
#endif
#if SOC_LCD_RGB_SUPPORTED
#include "esp_lcd_panel_rgb.h"
#endif

// LVGL draws into a full-screen copy of the UI in PSRAM, the shadow, in direct
// mode and only records which areas changed. A present task owns the panel's
// two frame buffers: it copies the changed areas from the shadow into the
// buffer that is not being scanned out, turning them to the panel's
// orientation on the way, then asks the panel driver to show that buffer from
// the next frame and waits until the switch has happened. A buffer that missed a change gets
// it at the next present, so both stay complete and nothing tears.
//
// A panel on SPI keeps the image in its own RAM. The present task keeps one
// frame buffer in PSRAM, updates it the same way and sends the panel the
// rows it changed, then waits until the transfer has read them; without a
// tearing signal the panel can show a frame half updated. On an unrotated
// SPI panel the game's rows skip the frame buffer: the present task builds
// them in bands in internal RAM and sends those, see game_band().
//
// While a game shows, the present task also writes the newest game frame,
// scaled, into the game window of that buffer, and leaves the window out of
// the UI copies. Frames reach it through three slots: the producer fills
// one, the present task reads another, and the third holds the newest frame
// published, traded with one atomic exchange on either side.
namespace gbx::platform::display {

namespace {

using board::Rotation;
using detail::GameLayer;
using detail::kMaxLine;
using detail::Rect;

constexpr const char* kTag = "display";
constexpr int kFrameBuffers = 2;  // one on an SPI panel, see buffer_count
constexpr int kUiCore = 0;  // the emulator gets the other core
// Both below the emulator's PPU and APU worker (5): while presenting takes
// a few milliseconds a frame, the worker still takes the scanlines the CPU
// core waits to hand over.
constexpr int kLvglPriority = 3;
constexpr int kPresentPriority = 4;  // above LVGL, below the PPU/APU worker
// The frame step on the emulator's core, see run_frame_step(): below the
// emulation task (5), so it only takes the time the emulation leaves.
constexpr int kStepPriority = 4;
// The least time between two game frames taken, see GBX_DISPLAY_GAME_HALF_RATE:
// one and a half frame periods of a game at normal speed.
#if CONFIG_GBX_DISPLAY_GAME_HALF_RATE
constexpr int64_t kGameFrameGapUs = 25'000;
#else
constexpr int64_t kGameFrameGapUs = 0;
#endif
// game_frame_due() says yes this much before kGameFrameGapUs, as the present
// task takes a frame some time after it is published.
constexpr int64_t kDueEarlyUs = 4'000;
// The LVGL lock also covers the game frame, see GBX_DISPLAY_LOCK_GAME_DRAW.
#if CONFIG_GBX_DISPLAY_LOCK_GAME_DRAW
constexpr bool kGameHoldsLvgl = true;
#else
constexpr bool kGameHoldsLvgl = false;
#endif
constexpr int kMaxRects = 16;        // more changed areas than this are copied as a full screen
constexpr int kSwapTimeoutMs = 100;
constexpr int kSlots = 3;
// The largest game frame: the GBA's, or the GB's in a build without the GBA
// (components/core/Kconfig).
#if CONFIG_GBX_SYSTEMS_GB_ONLY
constexpr size_t kSlotPixels = 160 * 144;
#else
constexpr size_t kSlotPixels = 240 * 160;
#endif
constexpr uint32_t kFresh = 4;  // in ready: a frame the present task has not taken

struct Damage {
    Rect rects[kMaxRects];
    int count = 0;
    bool all = false;

    void add(const Rect& r) {
        if (count < kMaxRects) {
            rects[count++] = r;
        } else {
            all = true;
        }
    }
    bool empty() const { return count == 0 && !all; }
};

board::Display panel;
Size ui{0, 0};
uint16_t* shadow = nullptr;
detail::Config backend_config;
uint16_t* frame_buffers[kFrameBuffers] = {};
int buffer_count = kFrameBuffers;
int back = 1;  // the DPI and RGB drivers start by scanning out buffer 0
// An unrotated SPI panel's two bands of whole rows, in internal RAM: one is
// built while the other is sent. Writing the game into the frame buffer in
// PSRAM, writing it back and having the DMA read it again took most of the
// present task's time on ESP32-C5, whose PSRAM is quad SPI.
constexpr int kBandRows = 8;
uint16_t* bands[2] = {};

TaskHandle_t present_task = nullptr;
TaskHandle_t step_task = nullptr;  // on the emulator's core
SemaphoreHandle_t step_done = nullptr;
void (*step_function)(void*) = nullptr;  // set by the present task while it waits
void* step_argument = nullptr;
std::atomic<bool> core_lent{false};
SemaphoreHandle_t frame_done = nullptr;
std::atomic<uint32_t> frames_done{0};

portMUX_TYPE damage_lock = portMUX_INITIALIZER_UNLOCKED;
Damage pending;  // filled by the LVGL task, taken by the present task

// Set by show_game() and hide_game() under damage_lock. Each show_game()
// counts a generation, after which the present task blacks out the window
// in both buffers; hide_game() has it copy the UI back into the window.
GameLayer layer_config;
std::atomic<const uint16_t*> color_table{nullptr};
std::atomic<uint64_t> shade_colors{0};  // see set_game_shades(), shade 0 in the low bits
uint32_t layer_generation = 0;
bool restore_window = false;

uint16_t* slots[kSlots] = {};
uint64_t slot_ids[kSlots] = {};
std::atomic<uint32_t> ready{1};  // a slot index, | kFresh
int writer = 2;                  // the producer's slot
std::atomic<bool> producing{false};
std::atomic<Scaling> requested_scaling{Scaling::nearest};
std::atomic<int64_t> shown_frames{0};
std::atomic<int64_t> taken_us{0};  // when the present task took the latest game frame
// See set_brightness(). backlight_on is set by the present task with the
// first frame shown.
std::atomic<int> brightness{100};
std::atomic<bool> backlight_on{false};

#ifdef GBX_PROFILER
// See take_present_times().
std::atomic<uint32_t> times_ui{0}, times_game{0}, times_flush{0}, times_loops{0}, times_games{0};
std::atomic<uint32_t> times_lock{0}, times_fill{0}, times_present{0}, times_scanout{0};
std::atomic<uint32_t> times_render{0}, times_renders{0}, times_rects{0}, times_pixels{0};
std::atomic<size_t> profile_buffer_bytes{0};
int64_t render_start = 0;  // LVGL task only
std::atomic<PresentStage> present_stage{PresentStage::notify};
std::atomic<uint32_t> flow_published{0}, flow_overwritten{0}, flow_taken{0};
std::atomic<uint32_t> publish_interval[4]{}, take_age[4]{}, overwrite_age[4]{}, scan_cycles[4]{};
std::atomic<uint32_t> overwrites[static_cast<size_t>(PresentStage::count)][4]{};
std::atomic<uint32_t> lock_max_us{0}, busy_max_us{0};
uint32_t published_at[kSlots]{};  // transferred with slot ownership, like slot_ids
uint32_t last_publish_us = 0;  // producer only; reset starts a new statistics window, not a new producer
uint32_t last_scan_count = 0;  // console only

unsigned age_bucket(uint32_t us) { return us < 5000 ? 0 : us < 12000 ? 1 : us < 20000 ? 2 : 3; }
void record_max(std::atomic<uint32_t>& counter, uint32_t value) {
    uint32_t old = counter.load(std::memory_order_relaxed);
    while (old < value && !counter.compare_exchange_weak(old, value, std::memory_order_relaxed)) {}
}
unsigned task_state_bucket(eTaskState state) {
    switch (state) {
        case eRunning: return 0;
        case eReady: return 1;
        case eBlocked: return 2;
        default: return 3;
    }
}
#define GBX_IF_PROFILER(...) __VA_ARGS__
#else
#define GBX_IF_PROFILER(...)
#endif

// Copies one rectangle of the shadow into a frame buffer in panel orientation.
void copy_rect(uint16_t* fb, const Rect& r) {
    GBX_IF_PROFILER(times_rects.fetch_add(1, std::memory_order_relaxed);
                    times_pixels.fetch_add((r.x1 - r.x0) * (r.y1 - r.y0), std::memory_order_relaxed);)
    detail::copy_ui_rect(fb, backend_config, r);
}

// Panel rows [first, last) covered by a UI rectangle.
void panel_rows(const Rect& r, int& first, int& last) {
    switch (panel.rotation) {
        case Rotation::none:
            first = r.y0, last = r.y1;
            break;
        case Rotation::cw90:
            first = r.x0, last = r.x1;
            break;
        case Rotation::cw180:
            first = ui.height - r.y1, last = ui.height - r.y0;
            break;
        case Rotation::cw270:
            first = ui.width - r.x1, last = ui.width - r.x0;
            break;
    }
}

// Copies a UI rectangle, leaving out the part inside `hole`.
void copy_around(uint16_t* fb, const Rect& r, const Rect& hole) {
    if (r.x1 <= hole.x0 || r.x0 >= hole.x1 || r.y1 <= hole.y0 || r.y0 >= hole.y1) {
        copy_rect(fb, r);
        return;
    }
    const int y0 = std::max(r.y0, hole.y0);
    const int y1 = std::min(r.y1, hole.y1);
    const Rect parts[] = {{r.x0, r.y0, r.x1, y0},
                          {r.x0, y1, r.x1, r.y1},
                          {r.x0, y0, std::min(r.x1, hole.x0), y1},
                          {std::max(r.x0, hole.x1), y0, r.x1, y1}};
    for (const Rect& part : parts) {
        if (part.x0 < part.x1 && part.y0 < part.y1) {
            copy_rect(fb, part);
        }
    }
}

// The DPI driver reports every frame it has sent, the RGB driver only the
// switch to a new buffer (the GDMA link switch event), an SPI panel's IO the
// end of the rows' transfer; present_loop() waits for one after
// esp_lcd_panel_draw_bitmap().
IRAM_ATTR bool frame_buffer_complete() {
    frames_done.fetch_add(1, std::memory_order_relaxed);
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(frame_done, &woken);
    return woken == pdTRUE;
}

#if SOC_MIPI_DSI_SUPPORTED
IRAM_ATTR bool on_dpi_frame_buffer_complete(esp_lcd_panel_handle_t, esp_lcd_dpi_panel_event_data_t*, void*) {
    return frame_buffer_complete();
}
#endif

#if SOC_LCD_RGB_SUPPORTED
IRAM_ATTR bool on_rgb_frame_buffer_complete(esp_lcd_panel_handle_t, const esp_lcd_rgb_panel_event_data_t*, void*) {
    return frame_buffer_complete();
}
#endif

IRAM_ATTR bool on_spi_transfer_done(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t*, void*) {
    return frame_buffer_complete();
}

// The panel's frame buffers, and its buffer-complete event on
// frame_buffer_complete(). An SPI panel's buffer is ours, in PSRAM, which its
// DMA reads row by row.
bool attach_frame_buffers() {
    void* fb0 = nullptr;
    void* fb1 = nullptr;
    esp_err_t err = ESP_ERR_NOT_SUPPORTED;
    switch (panel.driver) {
        case board::PanelDriver::dpi:
#if SOC_MIPI_DSI_SUPPORTED
            err = esp_lcd_dpi_panel_get_frame_buffer(panel.panel, kFrameBuffers, &fb0, &fb1);
            if (err == ESP_OK) {
                esp_lcd_dpi_panel_event_callbacks_t callbacks{};
                callbacks.on_frame_buf_complete = on_dpi_frame_buffer_complete;
                err = esp_lcd_dpi_panel_register_event_callbacks(panel.panel, &callbacks, nullptr);
            }
#endif
            break;
        case board::PanelDriver::rgb:
#if SOC_LCD_RGB_SUPPORTED
            err = esp_lcd_rgb_panel_get_frame_buffer(panel.panel, kFrameBuffers, &fb0, &fb1);
            if (err == ESP_OK) {
                esp_lcd_rgb_panel_event_callbacks_t callbacks{};
                callbacks.on_frame_buf_complete = on_rgb_frame_buffer_complete;
                err = esp_lcd_rgb_panel_register_event_callbacks(panel.panel, &callbacks, nullptr);
            }
#endif
            break;
        case board::PanelDriver::spi: {
            size_t alignment = 0;
            esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &alignment);
            const size_t bytes = static_cast<size_t>(panel.width) * panel.height * sizeof(uint16_t);
            fb0 = heap_caps_aligned_calloc(std::max<size_t>(alignment, 4), 1, bytes, MALLOC_CAP_SPIRAM);
            buffer_count = 1;
            err = fb0 ? ESP_OK : ESP_ERR_NO_MEM;
            if (err == ESP_OK && panel.rotation == Rotation::none) {
                // Without them the game goes through the frame buffer.
                const size_t band_bytes = static_cast<size_t>(panel.width) * kBandRows * sizeof(uint16_t);
                for (auto*& band : bands) {
                    band = static_cast<uint16_t*>(heap_caps_malloc(band_bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA));
                }
                if (!bands[0] || !bands[1]) {
                    for (auto*& band : bands) {
                        heap_caps_free(band);
                        band = nullptr;
                    }
                }
            }
            if (err == ESP_OK) {
                esp_lcd_panel_io_callbacks_t callbacks{};
                callbacks.on_color_trans_done = on_spi_transfer_done;
                err = esp_lcd_panel_io_register_event_callbacks(panel.io, &callbacks, nullptr);
            }
            break;
        }
    }
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "panel frame buffers: %s", esp_err_to_name(err));
        return false;
    }
    frame_buffers[0] = static_cast<uint16_t*>(fb0);
    frame_buffers[1] = static_cast<uint16_t*>(fb1);
    return true;
}

#if !CONFIG_FREERTOS_UNICORE
void step_loop(void*) {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        step_function(step_argument);
        xSemaphoreGive(step_done);
    }
}
#endif

// Sends the rows of the image from the bands, see game_band(): whole rows
// when the rest of them changed, otherwise only the image's columns. The
// window covers all of them, and the bands after the first continue the
// write with RAMWRC (ST7789V2 datasheet 9.1.33, MIPI DCS
// write_memory_continue), a transfer each. The board sets the panel no gap.
// Returns the transfers started. A transfer waits for those queued before
// it, so the band built next has been sent.
int send_game_rows(const GameLayer& layer, const uint16_t* frame, const uint16_t* fb, bool whole) {
    const int x0 = whole ? 0 : layer.image.x0;
    const int x1 = whole ? panel.width : layer.image.x1;
    const int y0 = layer.image.y0, y1 = layer.image.y1;
    const uint8_t columns[] = {static_cast<uint8_t>(x0 >> 8), static_cast<uint8_t>(x0),
                               static_cast<uint8_t>((x1 - 1) >> 8), static_cast<uint8_t>(x1 - 1)};
    const uint8_t rows[] = {static_cast<uint8_t>(y0 >> 8), static_cast<uint8_t>(y0),
                            static_cast<uint8_t>((y1 - 1) >> 8), static_cast<uint8_t>(y1 - 1)};
    esp_lcd_panel_io_tx_param(panel.io, LCD_CMD_CASET, columns, sizeof columns);
    esp_lcd_panel_io_tx_param(panel.io, LCD_CMD_RASET, rows, sizeof rows);
    const int band_rows = kBandRows * panel.width / (x1 - x0);
    int sent = 0;
    for (int y = y0; y < y1; y += band_rows, ++sent) {
        const int count = std::min(band_rows, y1 - y);
        uint16_t* band = bands[sent & 1];
        detail::game_band(band, x0, x1, fb, backend_config, layer, frame, y, count);
        esp_lcd_panel_io_tx_color(panel.io, sent ? LCD_CMD_RAMWRC : LCD_CMD_RAMWR, band,
                                  static_cast<size_t>(x1 - x0) * count * sizeof(uint16_t));
    }
    return sent;
}

void present_loop(void*) {
    Damage carry;  // changes the back buffer has not received yet
    const Rect full{0, 0, ui.width, ui.height};
    GameLayer layer;
    uint32_t generation = 0;
    bool window_ready[kFrameBuffers] = {};     // blacked out since the latest show_game()
    uint32_t frame_drawn[kFrameBuffers] = {};  // frames_taken when the buffer got its frame
    uint32_t frames_taken = 0;                 // frames read since the latest show_game()
    int reader = 0;
    uint64_t counted_id = ~uint64_t{0};
    int64_t defer_until = 0;  // when a fresh frame waiting for kGameFrameGapUs is due
    for (;;) {
        GBX_IF_PROFILER(present_stage.store(PresentStage::notify, std::memory_order_relaxed);)
        TickType_t wait = portMAX_DELAY;
        if (defer_until) {
            const int64_t left = defer_until - esp_timer_get_time();
            wait = left > 0 ? pdMS_TO_TICKS(left / 1000 + 1) : 0;
        }
        ulTaskNotifyTake(pdTRUE, wait);
        GBX_IF_PROFILER(present_stage.store(PresentStage::dispatch, std::memory_order_relaxed);
                        const int64_t busy_start = esp_timer_get_time();
                        const uint32_t scans_start = frames_done.load(std::memory_order_relaxed);)

        Damage now;
        const Rect old_window = layer.window;
        portENTER_CRITICAL(&damage_lock);
        now = pending;
        pending = Damage{};
        const bool restore = restore_window;
        restore_window = false;
        layer = layer_config;
        layer.colors = layer.rgb565 ? nullptr : color_table.load(std::memory_order_acquire);
        if (layer.shades) {
            const uint64_t c = shade_colors.load(std::memory_order_relaxed);
            const uint16_t colors[4] = {static_cast<uint16_t>(c), static_cast<uint16_t>(c >> 16),
                                        static_cast<uint16_t>(c >> 32), static_cast<uint16_t>(c >> 48)};
            detail::set_shade_pairs(layer, colors);
        }
        const bool shown_again = generation != layer_generation;
        generation = layer_generation;
        portEXIT_CRITICAL(&damage_lock);
        if (shown_again) {
            window_ready[0] = window_ready[1] = false;
            frame_drawn[0] = frame_drawn[1] = 0;
            frames_taken = 0;
            taken_us.store(0, std::memory_order_relaxed);
        }
        if (restore) {
            now.add(old_window);
        }
        const bool fresh = layer.shown && (ready.load(std::memory_order_acquire) & kFresh);
        const int64_t taken = taken_us.load(std::memory_order_relaxed);
        const bool deferred = fresh && esp_timer_get_time() - taken < kGameFrameGapUs;
        defer_until = deferred ? taken + kGameFrameGapUs : 0;
        if (fresh && !deferred) {
            taken_us.store(esp_timer_get_time(), std::memory_order_relaxed);
            reader = static_cast<int>(ready.exchange(reader, std::memory_order_acq_rel) & 3);
            GBX_IF_PROFILER(flow_taken.fetch_add(1, std::memory_order_relaxed);
                            const uint32_t age = static_cast<uint32_t>(esp_timer_get_time()) - published_at[reader];
                            take_age[age_bucket(age)].fetch_add(1, std::memory_order_relaxed);)
            ++frames_taken;
        }
        uint16_t* fb = frame_buffers[back];
        const bool game_due = layer.shown && (!window_ready[back] || frame_drawn[back] != frames_taken);
        if (now.empty() && carry.empty() && !game_due) {
            continue;
        }
        int row0 = panel.height;
        int row1 = 0;
        auto touched = [&](const Rect& r) {
            int first = 0;
            int last = 0;
            panel_rows(r, first, last);
            row0 = std::min(row0, first);
            row1 = std::max(row1, last);
        };
        GBX_IF_PROFILER(times_loops.fetch_add(1); const int64_t present_start = esp_timer_get_time();)
        const bool ui_due = !now.empty() || !carry.empty();
        const bool holds_lvgl = ui_due || (kGameHoldsLvgl && game_due);
        if (holds_lvgl) {
            // Hold the LVGL lock so LVGL is not drawing into the shadow while we read it.
            GBX_IF_PROFILER(present_stage.store(PresentStage::lvgl_lock, std::memory_order_relaxed);
                            const int64_t lock_start = esp_timer_get_time();)
            lvgl_port_lock(0);
            GBX_IF_PROFILER(const int64_t lock_end = esp_timer_get_time();
                            times_lock.fetch_add(static_cast<uint32_t>(lock_end - lock_start));
                            record_max(lock_max_us, static_cast<uint32_t>(lock_end - lock_start));)
        }
        if (ui_due) {
            GBX_IF_PROFILER(const int64_t ui_start = esp_timer_get_time();
                            present_stage.store(PresentStage::ui_copy, std::memory_order_relaxed);)
            auto apply = [&](const Rect& r) {
                if (layer.shown) {
                    copy_around(fb, r, layer.window);
                } else {
                    copy_rect(fb, r);
                }
                touched(r);
            };
            if (now.all || carry.all) {
                apply(full);
            } else {
                detail::for_each_damage_rect({carry.rects, static_cast<size_t>(carry.count)},
                                             {now.rects, static_cast<size_t>(now.count)}, apply);
            }
            GBX_IF_PROFILER(times_ui.fetch_add(static_cast<uint32_t>(esp_timer_get_time() - ui_start));)
            if (!kGameHoldsLvgl) lvgl_port_unlock();
        }
        bool drew_frame = false;
        bool send_game = false;
        if (layer.shown) {
            if (!window_ready[back]) {
                GBX_IF_PROFILER(present_stage.store(PresentStage::fill, std::memory_order_relaxed);
                                const int64_t fill_start = esp_timer_get_time();)
                detail::fill_black(fb, backend_config, layer.window);
                GBX_IF_PROFILER(times_fill.fetch_add(static_cast<uint32_t>(esp_timer_get_time() - fill_start));)
                touched(layer.window);
                window_ready[back] = true;
            }
            if (frames_taken && frame_drawn[back] != frames_taken) {
                GBX_IF_PROFILER(present_stage.store(PresentStage::nearest, std::memory_order_relaxed);
                                const int64_t game_start = esp_timer_get_time();)
                if (bands[0]) {
                    send_game = true;
                } else {
                    detail::draw_game(fb, backend_config, layer, slots[reader]);
                    touched(layer.image);
                }
                GBX_IF_PROFILER(times_game.fetch_add(static_cast<uint32_t>(esp_timer_get_time() - game_start)); times_games.fetch_add(1);)
                frame_drawn[back] = frames_taken;
                drew_frame = true;
            }
        }
        if (kGameHoldsLvgl && holds_lvgl) lvgl_port_unlock();
        // With bands, the frame buffer holds no game: once a frame shows, its
        // rows under the image are not sent, and the image's rows are sent
        // again when the UI beside them changed.
        const bool banded = bands[0] && layer.shown && frames_taken;
        const bool around_image = banded && row0 < layer.image.y1 && row1 > layer.image.y0;
        send_game |= around_image;

        // The pointer lies inside the frame buffer, so the driver only writes
        // back the cache for these rows and shows this buffer from the next frame.
        // The RGB driver reports this switch once, maybe before draw_bitmap
        // returns, so its count is taken before the request, as is the end of
        // an SPI panel's transfer. The DPI driver reports every frame, and
        // only a frame ending after the request has switched, so its count is
        // taken after. An SPI panel takes the rows themselves.
        const bool counts_switches = panel.driver != board::PanelDriver::dpi;
        uint32_t seen = counts_switches ? frames_done.load(std::memory_order_relaxed) : 0;
        GBX_IF_PROFILER(present_stage.store(PresentStage::submit, std::memory_order_relaxed);
                        const int64_t flush_start = esp_timer_get_time();)
        int transfers = 1;  // that the wait below counts
        if (panel.driver == board::PanelDriver::spi) {
            transfers = 0;
            const auto send_rows = [&](int first, int last) {
                if (first >= last) return;
                esp_lcd_panel_draw_bitmap(panel.panel, 0, first, panel.width, last, fb + first * panel.width);
                ++transfers;
            };
            if (banded) {
                send_rows(row0, std::min(row1, layer.image.y0));
                send_rows(std::max(row0, layer.image.y1), row1);
                if (send_game) {
                    GBX_IF_PROFILER(const int64_t game_start = esp_timer_get_time();)
                    transfers += send_game_rows(layer, slots[reader], fb, around_image);
                    GBX_IF_PROFILER(times_game.fetch_add(static_cast<uint32_t>(esp_timer_get_time() - game_start));)
                }
            } else {
                send_rows(row0, row1);
            }
        } else {
            esp_lcd_panel_draw_bitmap(panel.panel, 0, row0, panel.width, row1, fb);
        }
        GBX_IF_PROFILER(times_flush.fetch_add(static_cast<uint32_t>(esp_timer_get_time() - flush_start));)
        GBX_IF_PROFILER(const int64_t scanout_start = esp_timer_get_time();
                        times_present.fetch_add(static_cast<uint32_t>(scanout_start - present_start));
                        present_stage.store(PresentStage::scanout, std::memory_order_relaxed);)
        if (!counts_switches) {
            seen = frames_done.load(std::memory_order_relaxed);
        }
        bool switched = true;
        while (frames_done.load(std::memory_order_relaxed) - seen < static_cast<uint32_t>(transfers)) {
            if (xSemaphoreTake(frame_done, pdMS_TO_TICKS(kSwapTimeoutMs)) != pdTRUE) {
                ESP_LOGW(kTag, "no frame completed within %d ms", kSwapTimeoutMs);
                switched = false;
                break;
            }
        }
        GBX_IF_PROFILER(times_scanout.fetch_add(static_cast<uint32_t>(esp_timer_get_time() - scanout_start));
                        present_stage.store(PresentStage::finish, std::memory_order_relaxed);
                        record_max(busy_max_us, static_cast<uint32_t>(esp_timer_get_time() - busy_start));
                        const uint32_t cycles = frames_done.load(std::memory_order_relaxed) - scans_start;
                        scan_cycles[std::min(cycles, uint32_t{3})].fetch_add(1, std::memory_order_relaxed);)
        // A frame ID counts once, when a buffer showing it first becomes the
        // scanout source; a blank screen repeats the previous ID.
        if (drew_frame && switched && slot_ids[reader] != counted_id) {
            counted_id = slot_ids[reader];
            shown_frames.fetch_add(1, std::memory_order_relaxed);
        }
        // One buffer has every change already.
        if (buffer_count == kFrameBuffers) {
            back ^= 1;
            carry = now;
        }
        if (!backlight_on.load(std::memory_order_relaxed)) {
            board::set_backlight(brightness.load());
            backlight_on.store(true);
            // A brightness set while this was turning the backlight on.
            board::set_backlight(brightness.load());
            ESP_LOGI(kTag, "first frame shown, panel rows %d-%d, backlight on", row0, row1);
        }
    }
}

void flush(lv_display_t* display, const lv_area_t* area, uint8_t*) {
    portENTER_CRITICAL(&damage_lock);
    pending.add({area->x1, area->y1, area->x2 + 1, area->y2 + 1});
    portEXIT_CRITICAL(&damage_lock);
    lv_display_flush_ready(display);
}

void on_refresh_ready(lv_event_t*) {
    xTaskNotifyGive(present_task);
}

#ifdef GBX_PROFILER
void on_render(lv_event_t* event) {
    if (lv_event_get_code(event) == LV_EVENT_RENDER_START) {
        render_start = esp_timer_get_time();
    } else {
        times_render.fetch_add(static_cast<uint32_t>(esp_timer_get_time() - render_start));
        times_renders.fetch_add(1);
    }
}
#endif

}  // namespace

bool start() {
    panel = board::display_init(kFrameBuffers);
    if (panel.panel == nullptr) {
        return false;
    }
    bool turned = panel.rotation == Rotation::cw90 || panel.rotation == Rotation::cw270;
    ui = turned ? Size{panel.height, panel.width} : Size{panel.width, panel.height};

    frame_done = xSemaphoreCreateBinary();
    if (!attach_frame_buffers()) {
        return false;
    }
    back = buffer_count - 1;

    size_t shadow_bytes = static_cast<size_t>(ui.width) * ui.height * sizeof(uint16_t);
    size_t alignment = 0;
    ESP_ERROR_CHECK(esp_cache_get_alignment(MALLOC_CAP_SPIRAM, &alignment));
    shadow = static_cast<uint16_t*>(heap_caps_aligned_calloc(alignment, 1, shadow_bytes, MALLOC_CAP_SPIRAM));
    if (shadow == nullptr) {
        ESP_LOGE(kTag, "no PSRAM for the %u-byte UI shadow", static_cast<unsigned>(shadow_bytes));
        return false;
    }
    // The game frames' slots, taken now for the largest frame: taken by the
    // first game, they would stay between the blocks that game frees and
    // split PSRAM. A producer may hold one at any time, so they are never
    // freed.
    for (auto*& slot : slots) {
        slot = static_cast<uint16_t*>(heap_caps_calloc(kSlotPixels, sizeof(uint16_t), MALLOC_CAP_SPIRAM));
        if (slot == nullptr) {
            ESP_LOGE(kTag, "no PSRAM for game frames");
            return false;
        }
    }

    backend_config = {panel, ui, shadow};
    if (!detail::backend_start()) {
        return false;
    }

    GBX_IF_PROFILER(
        profile_buffer_bytes.store(shadow_bytes * (1 + buffer_count) + kSlots * kSlotPixels * sizeof(uint16_t));)
    lvgl_port_cfg_t lvgl_config{
        .task_priority = kLvglPriority,
        .task_stack = 8192,
        .task_affinity = kUiCore,
        .task_max_sleep_ms = 500,
        // In PSRAM: internal RAM goes to the emulators' JIT code pools, and
        // the UI task runs a few times a frame at most and never touches
        // flash.
        .task_stack_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
        .timer_period_ms = 5,
    };
    ESP_ERROR_CHECK(lvgl_port_init(&lvgl_config));
#if !CONFIG_FREERTOS_UNICORE
    step_done = xSemaphoreCreateBinary();
    if (step_done == nullptr ||
        !create_internal_task(step_loop, "frame-step", 3072, nullptr, kStepPriority, &step_task, 1 - kUiCore)) {
        // Frames are then converted on the present task.
        step_task = nullptr;
    }
#endif
    create_internal_task(present_loop, "present", 4096, nullptr, kPresentPriority, &present_task, kUiCore);

    lvgl_port_lock(0);
#if CONFIG_LV_USE_PPA
    if (!detail::configure_lvgl_acceleration()) {
        lvgl_port_unlock();
        return false;
    }
#endif
    lv_display_t* display = lv_display_create(ui.width, ui.height);
    lv_display_set_color_format(display, LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display, shadow, nullptr, shadow_bytes, LV_DISPLAY_RENDER_MODE_DIRECT);
    lv_display_set_flush_cb(display, flush);
    lv_display_add_event_cb(display, on_refresh_ready, LV_EVENT_REFR_READY, nullptr);
#ifdef GBX_PROFILER
    lv_display_add_event_cb(display, on_render, LV_EVENT_RENDER_START, nullptr);
    lv_display_add_event_cb(display, on_render, LV_EVENT_RENDER_READY, nullptr);
#endif
    lvgl_port_unlock();

    ESP_LOGI(kTag, "panel %dx%d, UI %dx%d, %d frame buffers", panel.width, panel.height, ui.width, ui.height,
             buffer_count);
    return true;
}

Size size() {
    return ui;
}

bool show_game(std::span<const int16_t, 4> window, int width, int height, PixelFormat format) {
    const Scaling scaling = requested_scaling.load();
    if (present_task == nullptr || width <= 0 || height <= 0 || !detail::game_size_supported(width, height)) {
        return false;
    }
    const int scale = std::min(window[2] / width, window[3] / height);
    if (scale < 1 || width * scale > kMaxLine || window[0] < 0 || window[1] < 0 ||
        window[0] + window[2] > ui.width || window[1] + window[3] > ui.height) {
        return false;
    }
    if (static_cast<size_t>(width) * height > kSlotPixels ||
        (format == PixelFormat::shades && !game_shades_supported())) {
        return false;
    }
    GameLayer layer;
    layer.shown = true;
    layer.window = {window[0], window[1], window[0] + window[2], window[1] + window[3]};
    layer.width = width;
    layer.height = height;
    layer.scale = scale;
    layer.scaling = scaling;
    layer.rgb565 = format == PixelFormat::rgb565;
    layer.shades = format == PixelFormat::shades;
    layer.image.x0 = layer.window.x0 + (window[2] - width * scale) / 2;
    layer.image.y0 = layer.window.y0 + (window[3] - height * scale) / 2;
    layer.image.x1 = layer.image.x0 + width * scale;
    layer.image.y1 = layer.image.y0 + height * scale;
    // A frame published for an earlier layer is not shown.
    ready.fetch_and(~kFresh);
    shown_frames.store(0);
    portENTER_CRITICAL(&damage_lock);
    layer_config = layer;
    ++layer_generation;
    restore_window = false;
    portEXIT_CRITICAL(&damage_lock);
    producing.store(true);
    xTaskNotifyGive(present_task);
    return true;
}

void hide_game() {
    if (present_task == nullptr) {
        return;
    }
    producing.store(false);
    portENTER_CRITICAL(&damage_lock);
    if (layer_config.shown) {
        layer_config.shown = false;
        restore_window = true;
    }
    portEXIT_CRITICAL(&damage_lock);
    xTaskNotifyGive(present_task);
}

bool game_shades_supported() {
    // Only game_band() reads shades.
    return bands[0] != nullptr;
}

void set_game_shades(std::span<const uint16_t, 4> colors) {
    uint64_t packed = 0;
    for (int i = 0; i < 4; ++i) packed |= uint64_t{colors[i]} << (16 * i);
    shade_colors.store(packed, std::memory_order_relaxed);
}

uint16_t* game_frame_buffer() {
    return producing.load(std::memory_order_acquire) ? slots[writer] : nullptr;
}

bool game_frame_due() {
    return kGameFrameGapUs == 0 ||
           esp_timer_get_time() - taken_us.load(std::memory_order_relaxed) >= kGameFrameGapUs - kDueEarlyUs;
}

void publish_game_frame(uint64_t id) {
    slot_ids[writer] = id;
    GBX_IF_PROFILER(const uint32_t now = static_cast<uint32_t>(esp_timer_get_time());
                    published_at[writer] = now;
                    flow_published.fetch_add(1, std::memory_order_relaxed);
                    if (last_publish_us)
                        publish_interval[age_bucket(now - last_publish_us)].fetch_add(1, std::memory_order_relaxed);
                    last_publish_us = now;)
    const uint32_t previous = ready.exchange(writer | kFresh, std::memory_order_acq_rel);
    writer = static_cast<int>(previous & 3);
    GBX_IF_PROFILER(if (previous & kFresh) {
        flow_overwritten.fetch_add(1, std::memory_order_relaxed);
        overwrite_age[age_bucket(now - published_at[writer])].fetch_add(1, std::memory_order_relaxed);
        const auto stage = present_stage.load(std::memory_order_relaxed);
        const auto state = task_state_bucket(eTaskGetState(present_task));
        overwrites[static_cast<size_t>(stage)][state].fetch_add(1, std::memory_order_relaxed);
    })
    xTaskNotifyGive(present_task);
}

int64_t game_frames_shown() {
    return shown_frames.load(std::memory_order_relaxed);
}

void lend_emulator_core(bool lend) {
    core_lent.store(lend, std::memory_order_relaxed);
}

void detail::run_frame_step(void (*step)(void*), void* arg) {
    // The flag may change between frames; a step runs where it starts.
    if (step_task == nullptr || !core_lent.load(std::memory_order_relaxed)) {
        step(arg);
        return;
    }
    step_function = step;
    step_argument = arg;
    xTaskNotifyGive(step_task);
    xSemaphoreTake(step_done, portMAX_DELAY);
}

bool scaling_supported(Scaling mode) {
#if CONFIG_GBX_DISPLAY_ACCEL
    return mode == Scaling::nearest || mode == Scaling::smooth;
#else
    return mode == Scaling::nearest;
#endif
}

Scaling default_scaling() {
    // Prefer smooth where it is implemented. Explicit settings still win.
    return scaling_supported(Scaling::smooth) ? Scaling::smooth : Scaling::nearest;
}

void set_scaling(Scaling mode) {
    requested_scaling.store(scaling_supported(mode) ? mode : Scaling::nearest);
}

void set_color_table(const uint16_t* table) {
    color_table.store(table, std::memory_order_release);
}

bool brightness_supported() {
    return true;
}

void set_brightness(int percent) {
    brightness.store(std::clamp(percent, 0, 100));
    if (backlight_on.load()) {
        board::set_backlight(brightness.load());
    }
}

bool lock(uint32_t timeout_ms) {
    // lvgl_port_lock() treats 0 as "wait forever"
    return lvgl_port_lock(timeout_ms == 0 ? 1 : timeout_ms);
}

void unlock() {
    lvgl_port_unlock();
}

#ifdef GBX_PROFILER
void detail::set_present_stage(PresentStage stage) {
    present_stage.store(stage, std::memory_order_relaxed);
}

FrameFlow take_frame_flow() {
    FrameFlow result{};
    result.published = flow_published.exchange(0);
    result.overwritten = flow_overwritten.exchange(0);
    result.taken = flow_taken.exchange(0);
    const uint32_t scans = frames_done.load();
    result.scans = scans - last_scan_count;
    last_scan_count = scans;
    for (unsigned i = 0; i < 4; ++i) {
        result.publish_interval[i] = publish_interval[i].exchange(0);
        result.take_age[i] = take_age[i].exchange(0);
        result.overwrite_age[i] = overwrite_age[i].exchange(0);
        result.scan_cycles[i] = scan_cycles[i].exchange(0);
        for (size_t stage = 0; stage < static_cast<size_t>(PresentStage::count); ++stage)
            result.overwrites[stage][i] = overwrites[stage][i].exchange(0);
    }
    result.lock_max_us = lock_max_us.exchange(0);
    result.busy_max_us = busy_max_us.exchange(0);
    return result;
}

PresentTimes take_present_times() {
    return {times_ui.exchange(0), times_game.exchange(0), times_flush.exchange(0), times_loops.exchange(0),
            times_games.exchange(0), times_lock.exchange(0), times_fill.exchange(0), times_present.exchange(0),
            times_scanout.exchange(0), times_render.exchange(0), times_renders.exchange(0),
            times_rects.exchange(0), times_pixels.exchange(0),
            profile_buffer_bytes.load() + detail::backend_buffer_bytes()};
}
#endif

}  // namespace gbx::platform::display

namespace gbx::platform::touch {

namespace {

board::Touch device;

// Logical UI coordinates of a panel pixel.
Point to_ui(float px, float py) {
    using board::Rotation;
    const auto& p = display::panel;
    const auto ui = display::ui;
    switch (p.rotation) {
        case Rotation::none:
            return {px, py};
        case Rotation::cw90:
            return {py, ui.height - 1 - px};
        case Rotation::cw180:
            return {ui.width - 1 - px, ui.height - 1 - py};
        case Rotation::cw270:
            return {ui.width - 1 - py, px};
    }
    return {px, py};
}

}  // namespace

bool start() {
    if (display::panel.panel == nullptr) {
        return false;
    }
    device = board::touch_init();
    return device.handle != nullptr;
}

int read(std::span<Point> points) {
    if (device.handle == nullptr) {
        return 0;
    }
    esp_lcd_touch_point_data_t data[CONFIG_ESP_LCD_TOUCH_MAX_POINTS];
    uint8_t count = 0;
    if (esp_lcd_touch_read_data(device.handle) != ESP_OK ||
        esp_lcd_touch_get_data(device.handle, data, &count, CONFIG_ESP_LCD_TOUCH_MAX_POINTS) != ESP_OK) {
        return -1;
    }
    int n = std::min<int>(count, static_cast<int>(points.size()));
    const float sx = static_cast<float>(display::panel.width) / device.width;
    const float sy = static_cast<float>(display::panel.height) / device.height;
    for (int i = 0; i < n; ++i) {
        points[i] = to_ui(data[i].x * sx, data[i].y * sy);
    }
    return n;
}

}  // namespace gbx::platform::touch

namespace gbx::platform::keys {

// No board has game keys of its own yet; the touch controls stand in.
uint16_t read() {
    return 0;
}

}  // namespace gbx::platform::keys

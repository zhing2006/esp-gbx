#pragma once

#include <cstdint>
#include <mutex>
#include <span>

#include "gbx/ui/controls.h"
#include "gbx/ui/pack.h"
#include "gbx/ui/vfd.h"
#include "lvgl.h"

namespace gbx::ui {

// What the shell shows about the running game: the run state, the lamps and
// the VFD readings. See docs/debug-design.md for each field.
struct Status {
    enum class Run : uint8_t { no_rom, loading, run, pause, error };
    // The machine emulated: GB in GB mode, GBC in either CGB mode.
    enum class Machine : uint8_t { none, gb, gbc, gba };
    enum class Backend : uint8_t { none, interpreter, jit };
    // The SAVE lamp: amber while DIRTY, blinking amber while WRITING, red on
    // ERROR; off otherwise.
    enum class Save : uint8_t { none, clean, dirty, writing, error };
    Run run = Run::no_rom;
    Machine machine = Machine::none;
    Backend backend = Backend::none;
    Save save = Save::none;
    bool error = false;  // the ERR lamp: an error the user has not seen yet
    bool fast = false;   // running at 2X: SPEED's unit reads FAST, AUDIO reads OFF
    Reading speed_percent;
    Reading emu_fps;
    Reading drw_fps;
    Reading frame_work_p95_ms;
    Reading frame_work_avg_ms;
    Reading frame_sample_count;
    Reading frame_gap_p95_ms;
    Reading frame_gap_avg_ms;
    Reading audio_queue_ms;
    Reading audio_min_ms;
    Reading audio_underruns_total;
    Reading audio_underruns_window;
    Reading heap_internal_free_kib;
    Reading heap_psram_free_mib;
    Reading idle_core0_percent;
    Reading idle_core1_percent;
};

// The game page: the shell background, the on-screen controls, the lamps and
// the VFD, built from the asset pack. The game window is left to the
// platform's game layer.
class GamePage {
public:
    static constexpr int kMaxControls = ui::kMaxControls;
    static constexpr int kMaxLamps = 8;
    static constexpr int kMaxCells = 4;

    // Builds the page on the active screen. Hold the LVGL lock. The pack and
    // its storage must stay alive until destroy(), including on failure.
    bool create(const Pack& pack);

    // Stops callbacks and frees objects and buffers, including partial creation.
    // Stop publishing controls first, and hold the LVGL lock.
    void destroy();

    // Sets how every control looks: `hits` from hit_test(), in pack order, and
    // the speed switch position. With GBX_UI_STATIC_SHELL only the switch
    // follows. Thread safe; the page picks it up in the LVGL task.
    void show_controls(std::span<const ControlHit> hits, bool double_speed);

    // Sets what the lamps and the VFD show; there is no VFD with
    // GBX_UI_STATIC_SHELL. Thread safe; the page redraws what changed in the
    // LVGL task.
    void show_status(const Status& status);

private:
    struct ControlView {
        lv_obj_t* object = nullptr;
        lv_image_dsc_t states[9]{};
        int16_t x[9]{};
        int16_t y[9]{};
        int state_count = 0;
        int shown = 0;
    };

    struct CellView {
        lv_obj_t* canvas = nullptr;
        lv_draw_buf_t* buffer = nullptr;
        Canvas pixels{};
        // The shell under the cell, copied out of the pack: the pack is in
        // flash, which a redraw would otherwise read again.
        uint16_t* shell = nullptr;
        char shown[512] = {};  // the value, unit and lines drawn last
    };

    bool add_control(const pack_format::Control& control);
    bool add_cell(const pack_format::VfdCell& cell);
    // Whether the cell's text changed and it was drawn again.
    bool draw_cell_text(CellView& view, const pack_format::VfdCell& cell, const Status& status);
    void show_lamps(const Status& status);
    void apply();

    lv_obj_t* root_ = nullptr;
    lv_timer_t* timer_ = nullptr;
    const Pack* pack_ = nullptr;
    VfdFont font_;
    ImageView background_{};
    lv_image_dsc_t background_dsc_{};
    ControlView controls_[kMaxControls];
    int control_count_ = 0;
    // Each lamp has an image per color; at most one shows.
    lv_obj_t* lamps_[kMaxLamps][2]{};
    lv_image_dsc_t lamp_images_[kMaxLamps][2]{};
    CellView cells_[kMaxCells];
    // The cells still to compare with cell_status_, a bit each, one drawn
    // again a pass at most and kCellGapUs apart (game_page.cpp), so that a
    // change of the readings does not take the UI core for all of them at
    // once.
    unsigned pending_cells_ = 0;
    Status cell_status_;
    uint64_t cell_drawn_us_ = 0;

    std::mutex mutex_;  // guards wanted_, changed_, status_ and status_changed_
    uint8_t wanted_[kMaxControls]{};
    bool changed_ = false;
    Status status_;
    bool status_changed_ = false;
};

}  // namespace gbx::ui

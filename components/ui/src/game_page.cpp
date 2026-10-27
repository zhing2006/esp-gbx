#include "gbx/ui/game_page.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <optional>
#include <span>
#include <string_view>

#include "gbx/platform.h"
#include "src/misc/cache/instance/lv_image_cache.h"

namespace gbx::ui {

namespace {

using pack_format::ControlKind;

constexpr const char* kTag = "ui.page";
constexpr uint32_t kApplyPeriodMs = 10;
constexpr uint64_t kBlinkUs = 250'000;  // the SAVE lamp's period while WRITING
// Between two cells drawn again: drawing one and LVGL rendering it take the
// UI core for several milliseconds, above the GBA's renderer.
constexpr uint64_t kCellGapUs = 100'000;

// Image names per visual state, in the order of the state index. The D-pad
// order matches DpadDirection, so a direction is its own state index.
constexpr const char* kButtonStates[] = {"normal", "pressed"};
constexpr const char* kDpadStates[] = {"normal",  "up",       "down",      "left",      "right",
                                       "up_left", "up_right", "down_left", "down_right"};
constexpr const char* kSwitchStates[] = {"1x", "2x"};

std::span<const char* const> state_names(ControlKind kind) {
    switch (kind) {
        case ControlKind::button: return kButtonStates;
        case ControlKind::dpad: return kDpadStates;
        case ControlKind::speed_switch: return kSwitchStates;
    }
    return {};
}

const char* run_text(Status::Run run) {
    switch (run) {
        case Status::Run::no_rom: return "NO ROM";
        case Status::Run::loading: return "LOADING";
        case Status::Run::run: return nullptr;
        case Status::Run::pause: return "PAUSE";
        case Status::Run::error: return "ERROR";
    }
    return nullptr;
}

// The fields of telemetry-fields.json that the VFD bindings use.
Reading lookup(std::string_view name, const void* context) {
    const auto& s = *static_cast<const Status*>(context);
    struct Field {
        std::string_view name;
        Reading Status::*reading;
    };
    static constexpr Field kFields[] = {
        {"speed_percent", &Status::speed_percent},
        {"emu_fps", &Status::emu_fps},
        {"drw_fps", &Status::drw_fps},
        {"frame_work_p95_ms", &Status::frame_work_p95_ms},
        {"frame_work_avg_ms", &Status::frame_work_avg_ms},
        {"frame_sample_count", &Status::frame_sample_count},
        {"frame_gap_p95_ms", &Status::frame_gap_p95_ms},
        {"frame_gap_avg_ms", &Status::frame_gap_avg_ms},
        {"audio_queue_ms", &Status::audio_queue_ms},
        {"audio_min_ms", &Status::audio_min_ms},
        {"audio_underruns_total", &Status::audio_underruns_total},
        {"audio_underruns_window", &Status::audio_underruns_window},
        {"heap_internal_free_kib", &Status::heap_internal_free_kib},
        {"heap_psram_free_mib", &Status::heap_psram_free_mib},
        {"idle_core0_percent", &Status::idle_core0_percent},
        {"idle_core1_percent", &Status::idle_core1_percent},
    };
    for (const auto& field : kFields) {
        if (field.name == name) {
            return s.*field.reading;
        }
    }
    return {};
}

lv_image_dsc_t to_lvgl(const ImageView& view) {
    lv_image_dsc_t dsc{};
    dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc.header.cf = view.alpha ? LV_COLOR_FORMAT_RGB565A8 : LV_COLOR_FORMAT_RGB565;
    dsc.header.w = static_cast<uint32_t>(view.width);
    dsc.header.h = static_cast<uint32_t>(view.height);
    dsc.header.stride = static_cast<uint32_t>(view.stride);
    // RGB565A8 keeps the alpha plane right after the color plane, as the pack does.
    dsc.data_size = static_cast<uint32_t>(view.stride * view.height + (view.alpha ? view.width * view.height : 0));
    dsc.data = reinterpret_cast<const uint8_t*>(view.rgb);
    return dsc;
}

}  // namespace

bool GamePage::create(const Pack& pack) {
    pack_ = &pack;
    auto bg = pack.image("bg");
    if (!bg || !font_.load(pack)) {
        platform::log(platform::LogLevel::error, kTag, "the asset pack has no background or VFD font");
        return false;
    }
    background_ = *bg;
    background_dsc_ = to_lvgl(background_);

    lv_obj_t* screen = lv_screen_active();
    lv_obj_set_scrollable(screen, false);
    root_ = lv_obj_create(screen);
    lv_obj_remove_style_all(root_);
    lv_obj_set_size(root_, pack.width(), pack.height());
    lv_obj_set_scrollable(root_, false);
    lv_obj_t* image = lv_image_create(root_);
    lv_image_set_src(image, &background_dsc_);
    lv_obj_set_pos(image, 0, 0);

    for (const auto& control : pack.controls()) {
        if (!add_control(control)) {
            return false;
        }
    }

    int lamp = 0;
    for (const auto& l : pack.lamps()) {
        if (lamp == kMaxLamps) {
            break;
        }
        for (int color = 0; color < 2 && !field(l.colors[color]).empty(); ++color) {
            // Lamps start off; the shell already shows them unlit.
            char name[32];
            std::snprintf(name, sizeof name, "lamp.%.*s.%.*s", static_cast<int>(field(l.id).size()),
                          field(l.id).data(), static_cast<int>(field(l.colors[color]).size()),
                          field(l.colors[color]).data());
            auto view = pack.image(name);
            if (!view) {
                platform::log(platform::LogLevel::error, kTag, "missing image %s", name);
                return false;
            }
            lamp_images_[lamp][color] = to_lvgl(*view);
            lv_obj_t* object = lv_image_create(root_);
            lamps_[lamp][color] = object;
            lv_image_set_src(object, &lamp_images_[lamp][color]);
            lv_obj_set_pos(object, view->x, view->y);
            lv_obj_set_hidden(object, true);
        }
        ++lamp;
    }

    int cell = 0;
    for (const auto& c : pack.cells()) {
        if (cell == kMaxCells || !add_cell(c)) {
            return false;
        }
        draw_cell_text(cells_[cell++], c, Status{});
    }

    timer_ = lv_timer_create([](lv_timer_t* timer) { static_cast<GamePage*>(lv_timer_get_user_data(timer))->apply(); },
                             kApplyPeriodMs, this);
    return true;
}

void GamePage::destroy() {
    if (timer_) {
        lv_timer_delete(timer_);
        timer_ = nullptr;
    }
    if (root_) {
        lv_obj_delete(root_);
        root_ = nullptr;
    }
    lv_image_cache_drop(&background_dsc_);
    for (auto& control : controls_) {
        for (int i = 0; i < control.state_count; ++i) {
            lv_image_cache_drop(&control.states[i]);
        }
        control = {};
    }
    for (int i = 0; i < kMaxLamps; ++i) {
        for (int color = 0; color < 2; ++color) {
            lv_image_cache_drop(&lamp_images_[i][color]);
            lamps_[i][color] = nullptr;
            lamp_images_[i][color] = {};
        }
    }
    for (auto& cell : cells_) {
        if (cell.buffer) {
            lv_image_cache_drop(cell.buffer);
            lv_draw_buf_destroy(cell.buffer);
        }
        platform::free_external(cell.shell);
        cell = {};
    }
    pending_cells_ = 0;
    pack_ = nullptr;
    font_ = {};
    background_ = {};
    background_dsc_ = {};
    control_count_ = 0;
    std::lock_guard lock(mutex_);
    std::memset(wanted_, 0, sizeof wanted_);
    changed_ = false;
    status_ = {};
    status_changed_ = false;
}

bool GamePage::add_control(const pack_format::Control& control) {
    if (control_count_ == kMaxControls) {
        platform::log(platform::LogLevel::error, kTag, "more than %d controls", kMaxControls);
        return false;
    }
    ControlView& view = controls_[control_count_++];
    std::string_view id = field(control.id);
    auto names = state_names(control.kind);
    for (const char* state : names) {
        char name[32];
        std::snprintf(name, sizeof name, "ctl.%.*s.%s", static_cast<int>(id.size()), id.data(), state);
        auto image = pack_->image(name);
        if (!image) {
            platform::log(platform::LogLevel::error, kTag, "missing image %s", name);
            return false;
        }
        view.states[view.state_count] = to_lvgl(*image);
        view.x[view.state_count] = static_cast<int16_t>(image->x);
        view.y[view.state_count] = static_cast<int16_t>(image->y);
        ++view.state_count;
    }
    view.object = lv_image_create(root_);
    lv_image_set_src(view.object, &view.states[0]);
    lv_obj_set_pos(view.object, view.x[0], view.y[0]);
    return true;
}

bool GamePage::add_cell(const pack_format::VfdCell& cell) {
    CellView& view = cells_[&cell - pack_->cells().data()];
    int x0 = static_cast<int>(std::floor(cell.rect[0]));
    int y0 = static_cast<int>(std::floor(cell.rect[1]));
    int x1 = static_cast<int>(std::ceil(cell.rect[0] + cell.rect[2]));
    int y1 = static_cast<int>(std::ceil(cell.rect[1] + cell.rect[3]));
    view.buffer = lv_draw_buf_create(x1 - x0, y1 - y0, LV_COLOR_FORMAT_RGB565, LV_STRIDE_AUTO);
    if (view.buffer == nullptr) {
        platform::log(platform::LogLevel::error, kTag, "no memory for a VFD cell");
        return false;
    }
    view.pixels = {reinterpret_cast<uint16_t*>(view.buffer->data), x0, y0, x1 - x0, y1 - y0,
                   static_cast<int>(view.buffer->header.stride / 2)};
    const Canvas& c = view.pixels;
    view.shell = static_cast<uint16_t*>(platform::allocate_external(size_t{static_cast<unsigned>(c.width * c.height)} * 2));
    if (view.shell == nullptr) {
        platform::log(platform::LogLevel::error, kTag, "no memory for a VFD cell");
        return false;
    }
    const int bg_row = background_.stride / 2;
    for (int y = 0; y < c.height; ++y) {
        std::memcpy(view.shell + y * c.width, background_.rgb + (c.y + y) * bg_row + c.x, c.width * sizeof(uint16_t));
    }
    view.canvas = lv_canvas_create(root_);
    lv_canvas_set_draw_buf(view.canvas, view.buffer);
    lv_obj_set_pos(view.canvas, x0, y0);
    return true;
}

bool GamePage::draw_cell_text(CellView& view, const pack_format::VfdCell& cell, const Status& status) {
    // SPEED shows the run state in place of the speed unless the game runs.
    const std::string_view id = field(cell.id);
    const char* state = id == "speed" ? run_text(status.run) : nullptr;
    // 2X plays no sound, so AUDIO reads OFF instead of a level.
    if (status.fast && id == "audio") state = "OFF";
    // All texts go into one buffer, which is also what the cell compares
    // with the texts drawn last.
    char shown[sizeof view.shown];
    size_t used = 0;  // at most sizeof shown - 1, which leaves room for the terminator
    auto put = [&](std::string_view text) {
        const size_t room = sizeof shown - 1 - used;
        const size_t n = room ? std::min(text.size(), room - 1) : 0;
        std::memcpy(shown + used, text.data(), n);
        const std::string_view stored{shown + used, n};
        used += n;
        if (used < sizeof shown - 1) shown[used++] = '|';
        return stored;
    };
    auto format = [&](std::string_view binding) {
        char text[128];
        return put(format_binding(binding, lookup, &status, text));
    };
    const std::string_view value_text = state ? put(state) : format(field(cell.value));
    // A state has no unit; at 2X, FAST is the unit of the speed.
    std::optional<std::string_view> unit;
    if (state) {
        unit = std::string_view{};
    } else if (status.fast && id == "speed") {
        unit = put("FAST");
    }
    std::array<std::string_view, pack_format::kMaxCellLines> lines;
    const size_t count = std::min<size_t>(cell.line_count, lines.size());
    for (size_t i = 0; i < count; ++i) {
        lines[i] = format(field(cell.lines[i].text));
    }
    shown[used] = '\0';
    if (std::strcmp(shown, view.shown) == 0) {
        return false;
    }
    std::memcpy(view.shown, shown, sizeof shown);
    // Start from the shell under the cell, then draw the text on it.
    Canvas& c = view.pixels;
    for (int y = 0; y < c.height; ++y) {
        std::memcpy(c.pixels + y * c.stride, view.shell + y * c.width, c.width * sizeof(uint16_t));
    }
    const auto drawn = std::span(lines).first(count);
    draw_cell(c, font_, cell, value_text, unit, drawn);
    lv_obj_invalidate(view.canvas);
    return true;
}

void GamePage::show_lamps(const Status& status) {
    const bool blink_on = platform::monotonic_us() % kBlinkUs < kBlinkUs / 2;
    for (int i = 0; i < kMaxLamps && i < static_cast<int>(pack_->lamps().size()); ++i) {
        const auto& lamp = pack_->lamps()[i];
        std::string_view id = field(lamp.id);
        // The color shown, by its name in the pack, or none.
        std::string_view lit;
        if ((id == "gb" && status.machine == Status::Machine::gb) ||
            (id == "gbc" && status.machine == Status::Machine::gbc) ||
            (id == "gba" && status.machine == Status::Machine::gba) ||
            (id == "int" && status.backend == Status::Backend::interpreter) ||
            (id == "jit" && status.backend == Status::Backend::jit)) {
            lit = field(lamp.colors[0]);
        } else if (id == "save") {
            if (status.save == Status::Save::dirty || (status.save == Status::Save::writing && blink_on)) {
                lit = "amber";
            } else if (status.save == Status::Save::error) {
                lit = "red";
            }
        } else if (id == "err" && status.error) {
            lit = "red";
        }
        for (int color = 0; color < 2; ++color) {
            lv_obj_t* object = lamps_[i][color];
            const bool on = !lit.empty() && field(lamp.colors[color]) == lit;
            if (object && lv_obj_is_hidden(object) == on) {
                lv_obj_set_hidden(object, !on);
            }
        }
    }
}

void GamePage::show_status(const Status& status) {
    std::lock_guard lock(mutex_);
    status_ = status;
    status_changed_ = true;
}

void GamePage::show_controls(std::span<const ControlHit> hits, bool double_speed) {
    std::lock_guard lock(mutex_);
    for (int i = 0; i < control_count_ && i < static_cast<int>(hits.size()); ++i) {
        uint8_t state = 0;
        switch (pack_->controls()[i].kind) {
            case ControlKind::button: state = hits[i].touched ? 1 : 0; break;
            case ControlKind::dpad: state = static_cast<uint8_t>(hits[i].direction); break;
            case ControlKind::speed_switch: state = double_speed ? 1 : 0; break;
        }
        if (wanted_[i] != state) {
            wanted_[i] = state;
            changed_ = true;
        }
    }
}

void GamePage::apply() {
    uint8_t wanted[kMaxControls];
    Status status;
    bool controls = false;
    bool shell = false;
    {
        std::lock_guard lock(mutex_);
        controls = changed_;
        shell = status_changed_;
        std::memcpy(wanted, wanted_, sizeof wanted);
        status = status_;
        changed_ = status_changed_ = false;
    }
    if (status.save == Status::Save::writing) {
        // The SAVE lamp blinks, so it needs a look at every pass.
        show_lamps(status);
    }
    if (shell) {
        show_lamps(status);
        cell_status_ = status;
        pending_cells_ = (1u << std::min<size_t>(kMaxCells, pack_->cells().size())) - 1;
    }
    // At most one cell drawn again a pass, kCellGapUs after the last.
    if (pending_cells_ && platform::monotonic_us() - cell_drawn_us_ >= kCellGapUs) {
        while (pending_cells_) {
            const int i = std::countr_zero(pending_cells_);
            pending_cells_ &= pending_cells_ - 1;
            if (cells_[i].canvas && draw_cell_text(cells_[i], pack_->cells()[i], cell_status_)) {
                cell_drawn_us_ = platform::monotonic_us();
                break;
            }
        }
    }
    if (!controls) {
        return;
    }
    for (int i = 0; i < control_count_; ++i) {
        ControlView& view = controls_[i];
        int state = wanted[i] < view.state_count ? wanted[i] : 0;
        if (state != view.shown) {
            lv_image_set_src(view.object, &view.states[state]);
            lv_obj_set_pos(view.object, view.x[state], view.y[state]);
            view.shown = state;
        }
    }
}

}  // namespace gbx::ui

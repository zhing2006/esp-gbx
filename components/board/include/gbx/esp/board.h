#pragma once

#include <cstdint>

#include "driver/i2s_types.h"
#include "esp_lcd_touch.h"
#include "esp_lcd_types.h"

// Every board under boards/ implements these. Only firmware code calls them.
namespace gbx::board {

// Board name for logs, such as "GUITION JC1060P470".
const char* name();

// Brings up the board's power rails and shared buses. Called once at startup,
// before anything else touches the hardware. Returns false if required
// supplies or shared buses could not be initialized.
bool init();

// SD card wiring. The card's IO runs at a fixed 3.3 V; where an on-chip LDO
// feeds its IO domain, board::init() powers it before the platform accesses
// the card. No board here requests 1.8 V signaling.
//
// Bus::sdmmc: an SDMMC slot, its clock, command and four data lines.
// Bus::spi: the card in SPI mode on an SPI host that board::init() has set
// up, possibly shared with the panel; `slot` is the host and `cs` the card's
// chip select, the other pins are the bus's.
struct Sdmmc {
    int slot;
    int clk;
    int cmd;
    int data[4];
    enum class Bus : uint8_t { sdmmc, spi } bus = Bus::sdmmc;
    int cs = -1;
};

Sdmmc sdmmc();

// How the UI's logical screen is turned clockwise to land on the panel. The
// JC1060P470 is held upright with its USB ports at the bottom, so its
// landscape panel shows the UI turned by 270 degrees.
enum class Rotation : uint8_t { none, cw90, cw180, cw270 };

// The esp_lcd driver behind Display::panel: the DPI panel of a MIPI DSI
// screen, or an RGB panel on the LCD_CAM parallel interface, whose frame
// buffers and buffer-switch event the platform takes from the same driver;
// or a panel with RAM of its own on an SPI bus, which takes the changed rows
// with esp_lcd_panel_draw_bitmap() from one frame buffer the platform keeps.
enum class PanelDriver : uint8_t { dpi, rgb, spi };

struct Display {
    esp_lcd_panel_handle_t panel = nullptr;  // initialized and switched on
    PanelDriver driver = PanelDriver::dpi;
    int width = 0;                           // panel resolution as scanned out
    int height = 0;
    Rotation rotation = Rotation::none;
    // PanelDriver::spi: the panel's IO, whose color transfers report their
    // end on on_color_trans_done. The panel takes little-endian RGB565.
    esp_lcd_panel_io_handle_t io = nullptr;
};

// Creates the panel with `frame_buffers` RGB565 frame buffers in PSRAM, sends
// the panel's init sequence if it has one and starts the scanout; an SPI
// panel has no frame buffers of its own. The backlight stays off. Returns a
// display with a null panel on failure.
Display display_init(int frame_buffers);

// Backlight brightness, 0 (off) to 100. The backlight driver's enable pin is
// switched by PWM, see board_support::backlight_init().
void set_backlight(int percent);

struct Touch {
    esp_lcd_touch_handle_t handle = nullptr;
    int width = 0;  // range of the coordinates the controller reports, which
    int height = 0; // map linearly onto the panel resolution
};

// Starts the touch controller. Returns a touch with a null handle on failure.
Touch touch_init();

struct Audio {
    i2s_chan_handle_t tx = nullptr;  // enabled, sending zeros until written
    bool mono = false;  // one speaker on the left channel: send both channels the mix
    // The board can tell when headphones are in, see headphones(). Its
    // speaker stays off until set_speaker(); headphones get both channels.
    bool headphones = false;
};

// Creates the I2S TX channel for 16-bit stereo frames at `rate`, with
// `buffers` DMA buffers of `frames` frames, sets up the codec and switches
// the amplifier on, unless Audio::headphones. Returns a null channel when
// the board has no sound output or setup failed.
Audio audio_init(uint32_t rate, uint32_t buffers, uint32_t frames);

// The codec's output volume, 0 to 100, and its mute.
void audio_volume(int percent);
void audio_mute(bool muted);

// On a board with Audio::headphones: whether they are in, unknown when the
// read fails, and the speaker's amplifier, false when switching it failed.
// Other boards return unknown and false. One task at a time.
enum class Headphones : uint8_t { unknown, absent, present };
Headphones headphones();
bool set_speaker(bool on);

// The battery-backed clock, as seconds since 1970-01-01 00:00 of the date and
// time it holds, in its own time zone. valid is false on a board without one,
// when it cannot be read, when it reports that its time was lost (VLF), or
// when it holds an impossible date. Reads the chip; call after init().
struct Clock {
    bool valid = false;
    int64_t seconds = 0;
};
Clock read_clock();
// Sets the clock to `seconds` as read_clock() counts them, from 2000 to 2099,
// and clears its lost-time flag. False on a board without one, out of range
// or when the write fails.
bool set_clock(int64_t seconds);

}  // namespace gbx::board

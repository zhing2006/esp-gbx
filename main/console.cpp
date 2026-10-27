#include "console.h"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include "driver/uart.h"
#include "driver/usb_serial_jtag.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gbx/app.h"
#include "gbx/audio.h"
#include "gbx/display.h"
#include "gbx/files.h"
#include "gbx/platform.h"
#include "sdkconfig.h"

namespace gbx::console {
namespace {

constexpr const char* kTag = "console";

// Commands, one a line, for testing games on the board and for scripted
// runs:
//   load <card path> int|jit [gbc]
//                     gbc puts the cartridge in a GBC, see gb::run_mode()
//   keys <step> ...   each step is <keys>[:<hold ms>[,<gap ms>]][*<count>],
//                     <keys> joins right left up down a b select start l r
//                     menu with '+', or is _ to only wait <hold ms>; 100 ms
//                     by default
//   stat [<seconds>]  the status once, or every <seconds> while a game runs
//   shot              the next game frame as base64 BGR555 in SHOT lines
//   shot menu         the menu canvas as base64 RGB565 in SHOT lines
//   mute 1|0          mutes the codec; the sound still runs and plays
//   speed 1|2         sets the speed switch to 1X or 2X
//   slot <1-4>        loads the running game's instant save from the slot,
//                     as SLOTS does, and runs on from it
//   clock [<YYYY-MM-DD> <hh:mm:ss>]
//                     sets the board's clock, then prints its time
//   file <card path>  a file of the card as base64 in FILE lines, such as a
//                     slot to debug on the PC
// Replies log as warnings, which release builds still print.
int read_console(char* bytes, size_t size) {
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    return usb_serial_jtag_read_bytes(bytes, size, portMAX_DELAY);
#else
    return uart_read_bytes(static_cast<uart_port_t>(CONFIG_ESP_CONSOLE_UART_NUM), bytes, size, portMAX_DELAY);
#endif
}

// Key bits from bit 0, as in platform::keys::read(): the GB keys, then kL,
// kR and kMenu.
constexpr const char* kKeyNames[] = {"right", "left", "up", "down", "a", "b", "select", "start", "l", "r", "menu"};
constexpr int kKeys = sizeof kKeyNames / sizeof kKeyNames[0];
static_assert(platform::keys::kMenu == 1 << (kKeys - 1));

void press_keys(char* steps) {
    char* step_end = nullptr;
    for (char* step = strtok_r(steps, " ", &step_end); step; step = strtok_r(nullptr, " ", &step_end)) {
        long count = 1;
        unsigned long hold = 100, gap = 100;
        if (char* p = std::strchr(step, '*')) {
            *p = 0;
            count = std::strtol(p + 1, nullptr, 10);
        }
        if (char* p = std::strchr(step, ':')) {
            *p = 0;
            hold = std::strtoul(p + 1, &p, 10);
            if (*p == ',') gap = std::strtoul(p + 1, nullptr, 10);
        }
        uint16_t keys = 0;
        if (std::strcmp(step, "_") != 0) {
            char* name_end = nullptr;
            for (char* name = strtok_r(step, "+", &name_end); name; name = strtok_r(nullptr, "+", &name_end)) {
                int bit = 0;
                while (bit < kKeys && std::strcmp(name, kKeyNames[bit]) != 0) ++bit;
                if (bit == kKeys) {
                    ESP_LOGW(kTag, "keys: no key %s", name);
                    return;
                }
                keys |= 1 << bit;
            }
        }
        for (long i = 0; i < count; ++i) {
            app::set_console_keys(keys);
            vTaskDelay(pdMS_TO_TICKS(hold));
            if (keys == 0) continue;
            app::set_console_keys(0);
            vTaskDelay(pdMS_TO_TICKS(gap));
        }
    }
    ESP_LOGW(kTag, "keys: done");
}

// `bytes` as base64 in lines that start with `kind`, with '=' padding at the
// end. The caller prints the BEGIN line.
void send_base64(const char* kind, const uint8_t* bytes, size_t size) {
    constexpr size_t kChunk = 1536;  // 2048 base64 characters a line
    constexpr char kDigits[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    // Only shots and files use the line, so it sits in PSRAM.
    EXT_RAM_BSS_ATTR static char line[kChunk / 3 * 4 + 1];
    for (size_t offset = 0; offset < size; offset += kChunk) {
        size_t n = 0;
        for (size_t i = offset; i < offset + kChunk && i < size; i += 3) {
            const size_t left = size - i;
            const uint32_t v = bytes[i] << 16 | (left > 1 ? bytes[i + 1] << 8 : 0) | (left > 2 ? bytes[i + 2] : 0);
            for (int k = 0; k < 4; ++k) line[n++] = k <= static_cast<int>(left) ? kDigits[v >> (18 - 6 * k) & 63] : '=';
        }
        line[n] = 0;
        std::printf("%s %s\n", kind, line);
        // Writing to the console waits without blocking; a long file would
        // keep the idle task of core 0 from its watchdog.
        vTaskDelay(1);
    }
    std::printf("%s END\n", kind);
    std::fflush(stdout);
}

// Pixels as base64 in SHOT lines; the BEGIN line gives their size and,
// unless they are BGR555, their format.
void send_pixels(const uint8_t* pixels, int width, int height, const char* format) {
    // A long line reaches the UART in pieces; a status line could land
    // between them.
    app::hold_status(true);
    std::printf("SHOT BEGIN %d %d%s%s\n", width, height, format[0] ? " " : "", format);
    send_base64("SHOT", pixels, size_t{2} * width * height);
    app::hold_status(false);
}

void send_file(const char* path) {
    const int64_t size = platform::files::size(path);
    auto* bytes = size > 0 ? static_cast<uint8_t*>(heap_caps_malloc(size, MALLOC_CAP_SPIRAM)) : nullptr;
    if (bytes == nullptr || !platform::files::read(path, {bytes, static_cast<size_t>(size)})) {
        ESP_LOGW(kTag, "file: cannot read %s", path);
    } else {
        app::hold_status(true);
        std::printf("FILE BEGIN %lld %s\n", static_cast<long long>(size), path);
        send_base64("FILE", bytes, size);
        app::hold_status(false);
    }
    heap_caps_free(bytes);
}

void send_shot() {
    constexpr size_t kBytes = app::kMaxFramePixels * 2;
    auto* frame = static_cast<uint8_t*>(heap_caps_malloc(kBytes, MALLOC_CAP_SPIRAM));
    int width = 0, height = 0;
    if (frame == nullptr || !app::capture_frame(reinterpret_cast<uint16_t*>(frame), 1000, width, height)) {
        ESP_LOGW(kTag, "shot: no frame");
    } else {
        send_pixels(frame, width, height, "");
    }
    heap_caps_free(frame);
}

void send_menu_shot() {
    int width = 0, height = 0;
    app::capture_menu({}, width, height);
    const size_t pixels = size_t{1} * width * height;
    auto* menu = static_cast<uint16_t*>(heap_caps_malloc(pixels * 2, MALLOC_CAP_SPIRAM));
    if (pixels == 0 || pixels * 2 % 3 != 0 || menu == nullptr || !app::capture_menu({menu, pixels}, width, height)) {
        ESP_LOGW(kTag, "shot: no menu");
    } else {
        send_pixels(reinterpret_cast<const uint8_t*>(menu), width, height, "RGB565");
    }
    heap_caps_free(menu);
}

// Days from 1970-01-01 to a date of the Gregorian calendar.
int64_t days_from_civil(int year, int month, int day) {
    year -= month <= 2;
    const int era = year / 400;
    const int yoe = year - era * 400;
    const int doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return int64_t{era} * 146097 + doe - 719468;
}

void clock_command(const char* args) {
    while (*args == ' ') ++args;
    if (*args) {
        int year, month, day, hours, minutes, seconds;
        if (std::sscanf(args, "%d-%d-%d %d:%d:%d", &year, &month, &day, &hours, &minutes, &seconds) != 6 ||
            month < 1 || month > 12 || day < 1 || day > 31 || hours < 0 || hours > 23 || minutes < 0 ||
            minutes > 59 || seconds < 0 || seconds > 59) {
            ESP_LOGW(kTag, "clock: use clock YYYY-MM-DD hh:mm:ss");
            return;
        }
        const int64_t s = ((days_from_civil(year, month, day) * 24 + hours) * 60 + minutes) * 60 + seconds;
        if (!platform::set_wall_time(s * 1'000'000)) {
            ESP_LOGW(kTag, "clock: cannot set the board's clock");
            return;
        }
    }
    const auto now = platform::wall_time();
    const time_t t = now.us / 1'000'000;
    tm parts{};
    gmtime_r(&t, &parts);
    ESP_LOGW(kTag, "clock: %04d-%02d-%02d %02d:%02d:%02d, %s", parts.tm_year + 1900, parts.tm_mon + 1, parts.tm_mday,
             parts.tm_hour, parts.tm_min, parts.tm_sec, now.trusted ? "trusted" : "not trusted");
}

void run(char* line) {
    if (std::strncmp(line, "keys ", 5) == 0) {
        press_keys(line + 5);
    } else if (std::strncmp(line, "stat", 4) == 0 && (line[4] == 0 || line[4] == ' ')) {
        app::log_status(static_cast<uint32_t>(std::strtoul(line + 4, nullptr, 10) * 1000));
    } else if (std::strcmp(line, "shot") == 0) {
        send_shot();
    } else if (std::strcmp(line, "shot menu") == 0) {
        send_menu_shot();
    } else if (std::strncmp(line, "clock", 5) == 0 && (line[5] == 0 || line[5] == ' ')) {
        clock_command(line + 5);
    } else if (std::strcmp(line, "speed 1") == 0 || std::strcmp(line, "speed 2") == 0) {
        app::set_double_speed(line[6] == '2');
    } else if (std::strncmp(line, "slot ", 5) == 0) {
        const long slot = std::strtol(line + 5, nullptr, 10);
        if (slot >= 1 && slot <= 4) app::load_slot(static_cast<int>(slot));
        ESP_LOGW(kTag, "slot %ld%s", slot, slot >= 1 && slot <= 4 ? "" : ": no such slot");
    } else if (std::strncmp(line, "file ", 5) == 0) {
        send_file(line + 5);
    } else if (std::strncmp(line, "mute ", 5) == 0) {
        platform::audio::set_muted(line[5] == '1');
    } else if (std::strncmp(line, "load ", 5) == 0) {
        // From the end, as a card path may hold spaces.
        char* backend = std::strrchr(line, ' ');
        if (backend < line + 5) return;
        const bool cgb = std::strcmp(backend + 1, "gbc") == 0;
        if (cgb) {
            *backend = 0;
            backend = std::strrchr(line, ' ');
            if (backend < line + 5) return;
        }
        *backend++ = 0;
        const bool jit = std::strcmp(backend, "jit") == 0;
        ESP_LOGW(kTag, "load %s %s%s", line + 5, jit ? "jit" : "int", cgb ? " gbc" : "");
        app::start_game(line + 5, jit ? app::Backend::jit : app::Backend::interpreter, cgb);
#ifdef GBX_PROFILER
    } else if (profile_command(line)) {
#endif
    } else if (line[0]) {
        // The first line after a reset can start with stray bytes.
        ESP_LOGW(kTag, "unknown command: %s", line);
    }
}

void read_commands(void*) {
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
    usb_serial_jtag_driver_config_t config{.tx_buffer_size = 1024, .rx_buffer_size = 1024};
    usb_serial_jtag_driver_install(&config);
#else
    uart_driver_install(static_cast<uart_port_t>(CONFIG_ESP_CONSOLE_UART_NUM), 1024, 0, 0, nullptr, 0);
#endif
    char line[300];
    size_t length = 0;
    for (;;) {
        char c;
        if (read_console(&c, 1) != 1) continue;
        if (c != '\n' && c != '\r') {
            if (length + 1 < sizeof line) line[length++] = c;
            continue;
        }
        line[length] = 0;
        length = 0;
        run(line);
    }
}

}  // namespace

void start() {
    // On core 0: the emulation has core 1. The stack is in PSRAM, as for the
    // app's file tasks, see platform::StackMemory.
    xTaskCreatePinnedToCoreWithCaps(read_commands, "gbx-console", 4096, nullptr, 1, nullptr, 0,
                                    MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

}  // namespace gbx::console

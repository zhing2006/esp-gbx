#include "hid_gamepad.h"

namespace gbx::platform::hid {
namespace {

constexpr uint32_t kGenericDesktop = 0x01, kButtonPage = 0x09;
constexpr uint32_t usage(uint32_t page, uint32_t id) {
    return page << 16 | id;
}

}  // namespace

// Besides the Input items of the gamepad's collection, it counts the bits of
// every Input item, by report ID, to find where those start. Only values of
// up to 16 bits count.
hid_layout_t parse(const uint8_t* descriptor, size_t size, const uint16_t (&button_keys)[HID_BUTTONS]) {
    struct Globals {
        uint32_t page = 0;
        int32_t min = 0;
        int32_t max = 0;
        uint32_t max_unsigned = 0;  // for a maximum written without its sign bit
        uint32_t bits = 0;
        uint32_t count = 0;
        uint8_t report = 0;
    };
    Globals globals;
    Globals pushed[4];
    int depth_pushed = 0;
    uint32_t usages[16];
    int usage_count = 0;
    uint32_t usage_min = 0, usage_max = 0;
    bool usage_range = false;
    uint16_t offsets[256] = {};  // input bits before the next value, by report ID
    int depth = 0, gamepad_depth = 0;
    bool gamepad_done = false, report_chosen = false;
    hid_layout_t layout{};
    for (int n = 0; n < HID_BUTTONS; ++n) layout.button_keys[n] = button_keys[n];

    size_t i = 0;
    while (i < size) {
        const uint8_t prefix = descriptor[i++];
        if (prefix == 0xfe) {  // a long item: its data size, tag and data
            if (i >= size) break;
            i += 2 + descriptor[i];
            continue;
        }
        const size_t length = (prefix & 3) == 3 ? 4 : prefix & 3;
        if (i + length > size) break;
        uint32_t data = 0;
        for (size_t k = 0; k < length; ++k) data |= static_cast<uint32_t>(descriptor[i + k]) << 8 * k;
        const int shift = 32 - 8 * static_cast<int>(length);
        const int32_t sdata = length == 0 ? 0 : static_cast<int32_t>(data << shift) >> shift;
        i += length;
        const unsigned type = prefix >> 2 & 3, tag = prefix >> 4;
        if (type == 1) {  // global
            switch (tag) {
                case 0: globals.page = data; break;
                case 1: globals.min = sdata; break;
                case 2: globals.max = sdata; globals.max_unsigned = data; break;
                case 7: globals.bits = data; break;
                case 8: globals.report = static_cast<uint8_t>(data); layout.ids = true; break;
                case 9: globals.count = data; break;
                case 10: if (depth_pushed < 4) pushed[depth_pushed++] = globals; break;
                case 11: if (depth_pushed > 0) globals = pushed[--depth_pushed]; break;
                default: break;
            }
            continue;
        }
        if (type == 2) {  // local; a 4-byte usage carries its page
            const uint32_t full = length == 4 ? data : usage(globals.page, data);
            if (tag == 0 && usage_count < 16) usages[usage_count++] = full;
            if (tag == 1 || tag == 2) {
                (tag == 1 ? usage_min : usage_max) = full;
                usage_range = true;
            }
            continue;
        }
        if (type != 0) continue;
        if (tag == 10) {  // Collection
            ++depth;
            const bool application = data == 1;
            const uint32_t kind = usage_count ? usages[0] : 0;
            if (!gamepad_done && !gamepad_depth && application &&
                (kind == usage(kGenericDesktop, 4) || kind == usage(kGenericDesktop, 5))) {
                gamepad_depth = depth;
            }
        } else if (tag == 12) {  // End Collection
            if (depth == gamepad_depth) {
                gamepad_depth = 0;
                gamepad_done = true;
            }
            --depth;
        } else if (tag == 8) {  // Input
            uint16_t& offset = offsets[globals.report];
            const bool variable = (data & 3) == 2;  // data, not constant; variable, not array
            const bool usable = !report_chosen || globals.report == layout.report;
            if (gamepad_depth && variable && usable && globals.bits >= 1 && globals.bits <= 16) {
                const int32_t max = globals.min >= 0 && globals.max < globals.min
                                        ? static_cast<int32_t>(globals.max_unsigned)
                                        : globals.max;
                for (uint32_t n = 0; n < globals.count; ++n) {
                    uint32_t value_usage = 0;
                    if (usage_range) {
                        value_usage = usage_min + n <= usage_max ? usage_min + n : usage_max;
                    } else if (usage_count) {
                        value_usage = usages[n < static_cast<uint32_t>(usage_count) ? n : usage_count - 1];
                    }
                    const hid_field_t field{static_cast<uint16_t>(offset + n * globals.bits),
                                            static_cast<uint8_t>(globals.bits), globals.min, max};
                    hid_field_t* target = nullptr;
                    if (value_usage == usage(kGenericDesktop, 0x30)) target = &layout.x;
                    if (value_usage == usage(kGenericDesktop, 0x31)) target = &layout.y;
                    if (value_usage == usage(kGenericDesktop, 0x39)) target = &layout.hat;
                    const uint32_t button = value_usage - usage(kButtonPage, 0);
                    if (value_usage >> 16 == kButtonPage && button >= 1 && button <= HID_BUTTONS) {
                        target = &layout.buttons[button - 1];
                    }
                    if (target && !target->bits) {
                        *target = field;
                        layout.report = globals.report;
                        report_chosen = true;
                    }
                }
            }
            offset = static_cast<uint16_t>(offset + globals.bits * globals.count);
        }
        usage_count = 0;  // a main item ends the local items
        usage_range = false;
    }
    return layout;
}

bool has_fields(const hid_layout_t& layout) {
    bool any = layout.hat.bits || layout.x.bits || layout.y.bits;
    for (const hid_field_t& button : layout.buttons) any |= button.bits != 0;
    return any;
}

}  // namespace gbx::platform::hid

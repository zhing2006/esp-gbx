#pragma once

// Where a HID gamepad's input report keeps the values that press keys, and
// the keys a report presses. The USB gamepad (usb_gamepad.cpp) and the
// Bluetooth gamepad share it: its pairing on the HP core parses the report
// descriptor, and its runtime on the LP core, which is C, decodes the
// reports. Keys are the bits of platform::keys::read().
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define HID_KEY_RIGHT 0x001
#define HID_KEY_LEFT 0x002
#define HID_KEY_UP 0x004
#define HID_KEY_DOWN 0x008
#define HID_KEY_A 0x010
#define HID_KEY_B 0x020
#define HID_KEY_SELECT 0x040
#define HID_KEY_START 0x080
#define HID_KEY_L 0x100
#define HID_KEY_R 0x200
#define HID_KEY_MENU 0x400

// The Button page's usages 1 to HID_BUTTONS.
#define HID_BUTTONS 16

// A value in an input report: its first bit after the report ID, its width,
// 0 when the gamepad has no such value, and its logical range.
typedef struct {
    uint16_t bit;
    uint8_t bits;
    int32_t min;
    int32_t max;
} hid_field_t;

typedef struct {
    uint8_t ids;     // the reports start with a report ID
    uint8_t report;  // the ID of the report that has the fields
    hid_field_t hat, x, y;
    hid_field_t buttons[HID_BUTTONS];
    uint16_t button_keys[HID_BUTTONS];  // the keys each button presses
} hid_layout_t;

static inline bool hid_read_field(const uint8_t* data, size_t size, const hid_field_t* field, int32_t* value) {
    if (!field->bits || field->bit + field->bits > size * 8) return false;
    uint32_t bits = 0;
    const size_t first = field->bit / 8, last = (field->bit + field->bits - 1) / 8;
    for (size_t k = first; k <= last; ++k) bits |= (uint32_t)data[k] << 8 * (k - first);
    bits = bits >> field->bit % 8 & ((1u << field->bits) - 1);
    const int shift = 32 - field->bits;
    *value = field->min < 0 ? (int32_t)(bits << shift) >> shift : (int32_t)bits;
    return true;
}

// An axis pressed past half its travel presses the key on that side.
static inline uint16_t hid_axis_keys(const uint8_t* data, size_t size, const hid_field_t* field, uint16_t low,
                                     uint16_t high) {
    int32_t value;
    const int64_t span = (int64_t)field->max - field->min;
    if (span <= 0 || !hid_read_field(data, size, field, &value)) return 0;
    const int64_t twice_from_center = 2 * ((int64_t)value - field->min) - span;
    return twice_from_center > span / 2 ? high : twice_from_center < -span / 2 ? low : 0;
}

// The keys a report presses; `data` starts after the report ID.
static inline uint16_t hid_decode(const hid_layout_t* layout, const uint8_t* data, size_t size) {
    // The hat switch's positions from north, clockwise.
    static const uint16_t hat8[8] = {
        HID_KEY_UP,   HID_KEY_UP | HID_KEY_RIGHT,  HID_KEY_RIGHT, HID_KEY_DOWN | HID_KEY_RIGHT,
        HID_KEY_DOWN, HID_KEY_DOWN | HID_KEY_LEFT, HID_KEY_LEFT,  HID_KEY_UP | HID_KEY_LEFT};
    static const uint16_t hat4[4] = {HID_KEY_UP, HID_KEY_RIGHT, HID_KEY_DOWN, HID_KEY_LEFT};
    uint16_t keys = 0;
    int32_t value;
    if (hid_read_field(data, size, &layout->hat, &value)) {
        const int64_t position = (int64_t)value - layout->hat.min;
        const int64_t positions = (int64_t)layout->hat.max - layout->hat.min + 1;
        // A value outside the range is the null state: nothing pressed.
        if (positions == 8 && position >= 0 && position < 8) keys |= hat8[position];
        if (positions == 4 && position >= 0 && position < 4) keys |= hat4[position];
    }
    keys |= hid_axis_keys(data, size, &layout->x, HID_KEY_LEFT, HID_KEY_RIGHT);
    keys |= hid_axis_keys(data, size, &layout->y, HID_KEY_UP, HID_KEY_DOWN);
    for (int n = 0; n < HID_BUTTONS; ++n) {
        if (hid_read_field(data, size, &layout->buttons[n], &value) && value) keys |= layout->button_keys[n];
    }
    return keys;
}

#ifdef __cplusplus
namespace gbx::platform::hid {

// Reads a report descriptor (HID 1.11, 6.2.2) into a layout: the Input items
// inside the first application collection whose usage is Joystick or Game
// Pad. `button_keys` gives the keys of the buttons, which gamepads number in
// different orders. A layout without fields has no gamepad.
hid_layout_t parse(const uint8_t* descriptor, size_t size, const uint16_t (&button_keys)[HID_BUTTONS]);

bool has_fields(const hid_layout_t& layout);

}  // namespace gbx::platform::hid
#endif

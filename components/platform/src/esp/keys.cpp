#include "gamepads.h"
#include "gbx/display.h"
#include "gbx/gamepad.h"
#include "sdkconfig.h"

// Without a gamepad the touch controls stand in for the keys.
namespace gbx::platform::keys {

bool start() {
    bool started = true;
#if CONFIG_GBX_USB_GAMEPAD
    started = usb_gamepad::start() && started;
#endif
#if CONFIG_GBX_BT_GAMEPAD
    started = bt_gamepad::start() && started;
#endif
    return started;
}

uint16_t read() {
    uint16_t keys = 0;
#if CONFIG_GBX_USB_GAMEPAD
    keys |= usb_gamepad::read();
#endif
#if CONFIG_GBX_BT_GAMEPAD
    keys |= bt_gamepad::read();
#endif
    return keys;
}

}  // namespace gbx::platform::keys

#if !CONFIG_GBX_BT_GAMEPAD
namespace gbx::platform::gamepad {

bool bluetooth() {
    return false;
}

bool pair() {
    return false;
}

void cancel() {}

Status status() {
    return {};
}

}  // namespace gbx::platform::gamepad
#endif

#pragma once

#include <cstdint>

// The gamepads keys::start() and keys::read() combine, each in the build
// with its option: start() once from the app's task, read() from that task
// only. read() gives the keys of keys::read() and 0 while the gamepad is not
// there.
namespace gbx::platform::usb_gamepad {
bool start();
uint16_t read();
}  // namespace gbx::platform::usb_gamepad

namespace gbx::platform::bt_gamepad {
bool start();
uint16_t read();
}  // namespace gbx::platform::bt_gamepad

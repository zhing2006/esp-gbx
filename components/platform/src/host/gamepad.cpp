#include "gbx/gamepad.h"

namespace gbx::platform::gamepad {

// The PC has no Bluetooth gamepad to pair.
bool bluetooth() { return false; }
bool pair() { return false; }
void cancel() {}
Status status() { return {}; }

}  // namespace gbx::platform::gamepad

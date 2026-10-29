#pragma once

#include <cstdint>

// A Bluetooth gamepad, on boards built with one (GBX_BT_GAMEPAD): pairing it
// from the menus and how its link stands. Its keys come through keys::read()
// with the other physical controls. Without Bluetooth, bluetooth() is false,
// pair() fails and status() stays idle.
namespace gbx::platform::gamepad {

bool bluetooth();

enum class Pairing : uint8_t {
    idle,        // not pairing
    starting,    // taking the radio from the paired gamepad's link
    searching,   // looking for a gamepad in pairing mode, for up to a minute
    connecting,  // found one: connecting, pairing and reading what it reports
    paired,      // saved; the new gamepad's link runs
    failed,      // see Status::failure; the link of the gamepad paired before runs again
};

enum class Failure : uint8_t {
    none,
    radio,        // the Bluetooth co-processor did not answer
    not_found,    // no gamepad in pairing mode within the minute
    refused,      // the connection or the pairing failed
    unsupported,  // its reports have no keys to read
    storage,      // the pairing could not be saved
};

struct Status {
    Pairing pairing = Pairing::idle;
    Failure failure = Failure::none;
    bool paired = false;     // a gamepad is saved
    bool connected = false;  // the saved gamepad is connected and its keys are read
    char name[32] = {};      // the gamepad being paired, else the saved one; may be empty
};

// Starts pairing in the background. A gamepad that pairs replaces the one
// saved before. False without Bluetooth or while a pairing runs.
bool pair();

// Gives up a pairing that has not finished, and returns paired and failed
// to idle. The saved gamepad's link runs again once status() is idle.
void cancel();

// From any task.
Status status();

}  // namespace gbx::platform::gamepad

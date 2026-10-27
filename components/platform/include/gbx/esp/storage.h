#pragma once

namespace gbx::platform::storage {

inline constexpr const char* kSdMountPath = "/sdcard";

// Mounts the board's SDMMC card without formatting it, after board::init()
// succeeds, before opening files. Returns true at once while mounted; after a
// failed mount, the next call tries again. No card detect: a card taken out
// stays mounted until unmount_sd(). The board owns and enables the IO supply
// independently of this mount.
bool mount_sd();
void unmount_sd();

}  // namespace gbx::platform::storage

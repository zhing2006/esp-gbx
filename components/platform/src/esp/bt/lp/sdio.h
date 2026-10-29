#pragma once

// The LP core's side of the HP core's SDMMC controller: the lock it shares
// with the SD card's driver on the HP core, and SDIO commands to the
// co-processor by programmed I/O. The HP core has set up the slot: its pins,
// clock and bus width, and the card itself.
#include <stdbool.h>
#include <stdint.h>

#include "../bt_shared.h"

extern bt_shared_t bt_shared;

// Takes the controller from the HP core, waiting while it has it, and puts
// it in programmed I/O mode. The HP core's interrupts from the controller
// stay off while the LP core has it, see sdmmc_share.cpp.
void sdio_acquire(void);
void sdio_release(void);

// The level of the co-processor's D1, low while it asks for the host's
// attention. Read it without the controller.
bool sdio_interrupted(void);

// CMD52: one register byte of function `fn`.
bool sdio_read_byte(uint8_t fn, uint32_t addr, uint8_t* value);
bool sdio_write_byte(uint8_t fn, uint32_t addr, uint8_t value);

// CMD53 in byte mode to or from incrementing addresses: a read takes one
// CMD53 for each 4 bytes, a write of up to 512 bytes one CMD53.
bool sdio_read_bytes(uint8_t fn, uint32_t addr, void* data, uint32_t size);
bool sdio_write_bytes(uint8_t fn, uint32_t addr, const void* data, uint32_t size);

// CMD53 in block mode, one 512-byte block of `size` bytes of `data` and
// zeros after them.
bool sdio_write_block(uint8_t fn, uint32_t addr, const void* data, uint32_t size);

// Microseconds of the LP core's clock, wrapping.
uint32_t lp_micros(void);

#pragma once

// The HP core's side of the SDMMC controller while the Bluetooth gamepad's
// LP core program uses the co-processor's slot. The controller runs one
// command at a time for both slots, so the SD card's driver on the HP core
// and the LP core take turns through a lock in bt_shared_t (Peterson's, as
// the cores share no atomic instructions on LP SRAM). The HP core's
// interrupts from the controller are on only while the HP core has it: the
// LP core polls the status bits the interrupt handler would clear. Each
// side leaves the controller as the other expects it: the LP core waits for
// the HP core's last command, the controller's own CMD12 included, and
// empties the FIFO when it is done; the HP core starts the IDMAC over when
// it takes the controller back.
//
// Every transaction and slot removal on the controller takes the lock,
// whatever the slot: the link wraps sdmmc_host_do_transaction() and
// sdmmc_host_deinit_slot(). ESP-Hosted's go through it too, so a packet it
// still sends after the LP program has started only waits for its turn.
#include "bt_shared.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sd_protocol_types.h"

namespace gbx::platform::sdmmc_share {

// Routes the other host functions of an SD card's sdmmc_host_t that reach
// the controller through lock() and unlock().
void wrap(sdmmc_host_t& host);

// Takes the controller for the HP core, recursively, for code that reaches
// it other than through the host functions, such as adding a slot.
void lock();
void unlock();

// Before the LP program starts, with `slot` the co-processor's: from then on
// the HP core takes the lock from it. `reader` is the HP core's task that
// reads from the co-processor when its SDIO interrupt comes, ESP-Hosted's:
// what the co-processor sends is the LP program's now. It is suspended where
// it waits for the interrupt, and stays there until ESP-Hosted deletes it as
// it stops; the interrupt stays masked. False when the task does not come to
// wait.
bool attach(bt_shared_t* shared, int slot, TaskHandle_t reader);
// After the LP program has stopped or been stopped.
void detach();

}  // namespace gbx::platform::sdmmc_share

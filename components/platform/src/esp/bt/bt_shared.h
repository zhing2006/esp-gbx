#pragma once

// What the HP core and the Bluetooth gamepad's LP core program (lp/) share.
// The struct is the LP program's bt_shared, in LP SRAM; the HP core reaches
// it as ulp_bt_shared. While the LP program runs, each field has one
// writer, except the SDMMC lock.
#include <stdint.h>

#include "../hid_gamepad.h"

#define BT_SHARED_VERSION 1
#define BT_MAX_INPUTS 4

// What pairing leaves for a reconnection without a host stack.
typedef struct {
    uint8_t addr_type;  // HCI address type of the identity address: 0 public, 1 random
    uint8_t addr[6];    // little-endian, as HCI carries it
    uint8_t resolve;  // the gamepad advertises with resolvable private addresses
    uint8_t irk[16];  // its identity resolving key, which resolves them
    uint8_t ltk[16];  // the key the central encrypts the link with
    uint64_t rand;
    uint16_t ediv;
    uint8_t key_size;
    uint8_t input_count;
    struct {
        uint16_t value;  // the input report's value handle
        uint16_t cccd;   // its Client Characteristic Configuration descriptor
        uint8_t report_id;
        uint8_t reserved;
    } inputs[BT_MAX_INPUTS];
    hid_layout_t layout;
} bt_bond_t;

// What the LP program is doing, in bt_shared_t.state.
enum {
    BT_STATE_OFF,
    BT_STATE_STARTING,    // resetting and setting up the controller
    BT_STATE_WAITING,     // waiting for the gamepad to connect
    BT_STATE_SECURING,    // encrypting the link with the bond's key
    BT_STATE_SUBSCRIBING, // turning on the input reports
    BT_STATE_READY,       // reading the input reports
    BT_STATE_STOPPED,     // stopped on request; it has let go of the SDMMC controller
    BT_STATE_FAILED,      // the co-processor stopped answering; see error
};

// Why the LP program failed or what last went wrong, in bt_shared_t.error.
enum {
    BT_ERROR_NONE,
    BT_ERROR_BUS,       // an SDIO command failed
    BT_ERROR_TIMEOUT,   // the controller did not answer an HCI command
    BT_ERROR_COMMAND,   // the controller refused an HCI command
    BT_ERROR_KEY,       // the gamepad refused the bond's key: it was paired again elsewhere
    BT_ERROR_STALLED,   // it kept the SDMMC controller, and the HP core stopped it
};

typedef struct {
    // Written by the HP core before it starts the LP core.
    uint32_t version;
    uint8_t slot;         // the co-processor's SDMMC slot
    uint8_t d1_gpio;      // its D1, which it pulls low to interrupt
    uint8_t aligned;      // frames from the co-processor are 4-byte aligned (SDIO SW_AGGR)
    uint8_t reserved;
    bt_bond_t bond;

    // The SDMMC lock, Peterson's: index 0 is the HP core, 1 the LP core.
    volatile uint32_t want[2];
    volatile uint32_t turn;

    // Written by the HP core while the LP program runs.
    volatile uint32_t stop;  // finish, let go of the controller and halt

    // Written by the LP program, and by the HP core once it has stopped it.
    volatile uint32_t state;
    volatile uint32_t error;
    volatile uint32_t held;              // keys held, the bits of keys::read()
    volatile uint8_t presses[16];        // per key bit, how many times it went down, wrapping
    volatile uint32_t bus_arg, bus_status;  // the last failed SDIO command's argument and RINTSTS
} bt_shared_t;

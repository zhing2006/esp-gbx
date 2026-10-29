#pragma once

// The gamepad's link at the HCI level, after pairing: setting up the
// controller, connecting to the bonded gamepad, encrypting with the bond's
// key, turning on its input reports and turning them into keys, and the
// answers the gamepad expects on the way.
#include <stdbool.h>
#include <stdint.h>

void hci_start(void);

// Takes a packet from the co-processor, see hosted_poll().
void hci_receive(const uint8_t* packet, uint16_t size);

// Moves on with timeouts and sends what is waiting. False once it has
// stopped on the HP core's request.
bool hci_run(void);

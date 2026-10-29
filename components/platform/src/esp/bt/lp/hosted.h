#pragma once

// HCI packets to and from the co-processor in ESP-Hosted frames over SDIO,
// the protocol of the espressif/esp_hosted host driver
// (host/mcu/eh_host_mcu_transport/src/eh_host_bus_sdio.c): a 12-byte header
// before each packet, the co-processor's slave registers for its
// interrupts, how many bytes it has sent and how many receive buffers it
// has, and transfers addressed so that they end at 0x1F800.
#include <stdbool.h>
#include <stdint.h>

// Picks up the counts where the HP core's driver left them: it has read
// everything the co-processor sent and has nothing in flight. False if the
// co-processor does not answer.
bool hosted_attach(void);

// Reads what the co-processor has sent, if it asks or `check` is set, and
// hands each HCI packet to `deliver`, its H4 type first; `deliver` runs
// while the LP core has the SDMMC controller and must not send. 0 if
// nothing came, 1 if packets came, -1 if the bus failed.
int hosted_poll(void (*deliver)(const uint8_t* packet, uint16_t size), bool check);

// Sends an HCI packet, `type` its H4 type. False while the co-processor has
// no free receive buffer, or if the bus failed; hosted_bus_failed() tells
// which.
bool hosted_send(uint8_t type, const uint8_t* body, uint16_t size);
bool hosted_bus_failed(void);

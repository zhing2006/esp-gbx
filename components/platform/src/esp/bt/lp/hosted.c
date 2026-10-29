#include "hosted.h"

#include <string.h>

#include "sdio.h"

// The co-processor's slave registers, function 1, and their bits
// (port/os/include/eh_host_port_sdio_reg.h in esp_hosted).
#define FN 1
#define REG_TOKEN_RDATA 0x44
#define REG_INT_RAW 0x50
#define REG_PACKET_LEN 0x60
#define REG_INT_CLR 0xD4
#define END_ADDR 0x1F800
#define LEN_MASK 0xFFFFFu
#define TOKEN_MASK 0xFFFu

// The frame header (common/eh_common/include/eh_common_header.h): the
// interface type and number, flags, the payload's length and offset, a
// byte-sum checksum, a sequence number, a byte the host leaves 0, and the
// HCI packet type. From the co-processor the H4 type is also the payload's
// first byte, so the packet goes on whole.
#define HEADER 12
#define IF_HCI 4

// A read takes one CMD53 for each word (see sdio.c), CHUNK bytes of them
// between two looks for whole frames.
#define CHUNK 256
#define RX_SIZE 1024
#define TX_SIZE 64

static uint32_t received;  // bytes read of what the co-processor sent, mod 2^20
static uint32_t filled;    // its receive buffers the host has filled, mod 2^12
static uint32_t credits;   // its free receive buffers when last read
static uint16_t sequence;
static uint32_t skip;      // bytes left of a frame too large to keep
static bool failed;
static uint8_t rx[RX_SIZE] __attribute__((aligned(4)));
static uint8_t tx[HEADER + TX_SIZE] __attribute__((aligned(4)));

static uint32_t le32(const uint8_t* p) {
    return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24;
}

static uint16_t le16(const uint8_t* p) {
    return (uint16_t)(p[0] | p[1] << 8);
}

static void put16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static bool read_reg(uint32_t addr, uint32_t* value) {
    uint8_t bytes[4];
    if (!sdio_read_bytes(FN, addr, bytes, sizeof bytes)) return false;
    *value = le32(bytes);
    return true;
}

static bool read_token(uint32_t* token) {
    if (!read_reg(REG_TOKEN_RDATA, token)) return false;
    *token = *token >> 16 & TOKEN_MASK;
    return true;
}

// Clears the interrupts that are raised and reads the length sent so far.
static bool read_length(uint32_t* length) {
    uint32_t raw = 0;
    bool ok = read_reg(REG_INT_RAW, &raw);
    if (ok && raw) ok = sdio_write_bytes(FN, REG_INT_CLR, &raw, 4);
    if (ok) ok = read_reg(REG_PACKET_LEN, length);
    *length &= LEN_MASK;
    return ok;
}

bool hosted_attach(void) {
    uint32_t length = 0;
    uint32_t token = 0;
    sdio_acquire();
    const bool ok = read_length(&length) && read_token(&token);
    sdio_release();
    if (!ok) return false;
    received = length;
    // With nothing in flight the co-processor has all its buffers back; one
    // of them is enough to go on with, and the count catches up as it frees
    // the buffers the LP core fills.
    filled = (token - 1) & TOKEN_MASK;
    credits = 1;
    return true;
}

// Hands on the whole frames at the start of rx; keeps the rest.
static uint32_t parse(uint32_t fill, void (*deliver)(const uint8_t*, uint16_t)) {
    uint32_t at = 0;
    while (fill - at >= HEADER) {
        const uint8_t* frame = rx + at;
        const uint16_t size = le16(frame + 2);
        if (size == 0 || le16(frame + 4) != HEADER) {
            // The end of an aggregate, padding, or a stream out of step:
            // nothing more to find in this read.
            at = fill;
            break;
        }
        const uint32_t whole = HEADER + size;
        if (whole > RX_SIZE - CHUNK) {
            skip = whole - (fill - at);
            at = fill;
            break;
        }
        if (fill - at < whole) break;
        if ((frame[0] & 0x0F) == IF_HCI) deliver(frame + HEADER, size);
        at += bt_shared.aligned ? (whole + 3) & ~3u : whole;
    }
    if (at > fill) at = fill;
    memmove(rx, rx + at, fill - at);
    return fill - at;
}

int hosted_poll(void (*deliver)(const uint8_t* packet, uint16_t size), bool check) {
    if (!check && !sdio_interrupted()) return 0;
    uint32_t length = 0;
    sdio_acquire();
    bool ok = read_length(&length);
    // The length counts every byte sent, so it also covers a packet whose
    // interrupt was cleared together with the one before it.
    const uint32_t total = ok ? (length - received) & LEN_MASK : 0;
    uint32_t fill = 0;
    skip = 0;
    for (uint32_t offset = 0; ok && offset < total; offset += CHUNK) {
        uint32_t valid = total - offset < CHUNK ? total - offset : CHUNK;
        ok = sdio_read_bytes(FN, END_ADDR - total + offset, rx + fill, valid);
        if (skip) {
            const uint32_t dropped = skip < valid ? skip : valid;
            memmove(rx + fill, rx + fill + dropped, valid - dropped);
            skip -= dropped;
            valid -= dropped;
        }
        fill = parse(fill + valid, deliver);
    }
    if (ok) received = (received + total) & LEN_MASK;
    sdio_release();
    if (!ok) {
        failed = true;
        return -1;
    }
    return total ? 1 : 0;
}

bool hosted_send(uint8_t type, const uint8_t* body, uint16_t size) {
    if (size > TX_SIZE) return true;  // nothing the LP program sends is this large
    memset(tx, 0, HEADER);
    tx[0] = IF_HCI;
    put16(tx + 2, size);
    put16(tx + 4, HEADER);
    put16(tx + 8, sequence);
    tx[11] = type;
    memcpy(tx + HEADER, body, size);
    uint16_t sum = 0;
    for (uint32_t i = 0; i < HEADER + size; ++i) sum = (uint16_t)(sum + tx[i]);
    put16(tx + 6, sum);

    sdio_acquire();
    bool ok = true;
    if (credits == 0) {
        uint32_t token = 0;
        ok = read_token(&token);
        credits = (token - filled) & TOKEN_MASK;
    }
    const bool room = ok && credits > 0;
    if (room) ok = sdio_write_block(FN, END_ADDR - (HEADER + size), tx, HEADER + size);
    sdio_release();
    if (!ok) {
        failed = true;
        return false;
    }
    if (!room) return false;
    filled = (filled + 1) & TOKEN_MASK;
    --credits;
    ++sequence;
    return true;
}

bool hosted_bus_failed(void) {
    return failed;
}

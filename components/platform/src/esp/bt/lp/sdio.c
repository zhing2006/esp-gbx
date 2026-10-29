#include "sdio.h"

#include "riscv/csr.h"
#include "soc/gpio_struct.h"
#include "soc/sdmmc_struct.h"
#include "ulp_lp_core_cpu_freq_shared.h"

// The controller is a Synopsys DesignWare Mobile Storage Host. Commands go
// through CMDARG and CMD, data through the FIFO at 0x200, and RINTSTS
// reports the progress whether or not an interrupt is enabled. On the P4 a
// read of the FIFO register returns the word at its head without removing
// it, from the HP core as from the LP core; writes do add words. So a read
// moves one word, and only a write moves a block. The controller's DMA
// cannot reach LP SRAM: given a descriptor there, it suspends. Registers are
// read and written whole: the LP core's compiler options lack
// -fstrict-volatile-bitfields, without which a bitfield may be read a byte
// at a time.

#define CTRL_FIFO_RESET (1u << 1)
#define CTRL_DMA_ENABLE (1u << 5)
#define CTRL_USE_IDMAC (1u << 25)

#define CMD_RESPONSE (1u << 6)
#define CMD_CHECK_CRC (1u << 8)
#define CMD_DATA (1u << 9)
#define CMD_WRITE (1u << 10)
#define CMD_WAIT_PREVIOUS (1u << 13)
#define CMD_USE_HOLD (1u << 29)
#define CMD_START (1u << 31)

#define INT_RE (1u << 1)
#define INT_CMD_DONE (1u << 2)
#define INT_DTO (1u << 3)
#define INT_RCRC (1u << 6)
#define INT_DCRC (1u << 7)
#define INT_RTO (1u << 8)
#define INT_DRTO (1u << 9)
#define INT_HTO (1u << 10)
#define INT_FRUN (1u << 11)
#define INT_HLE (1u << 12)
#define INT_SBE (1u << 13)
#define INT_EBE (1u << 15)
#define INT_CMD_ERRORS (INT_RE | INT_RCRC | INT_RTO | INT_HLE)
#define INT_DATA_ERRORS (INT_DCRC | INT_DRTO | INT_HTO | INT_FRUN | INT_SBE | INT_EBE)

// R5's response flags (SDIO 3.0, 5.2): COM_CRC_ERROR, ILLEGAL_COMMAND, ERROR,
// FUNCTION_NUMBER and OUT_OF_RANGE.
#define R5_ERRORS 0xCB00u

#define STATUS_FIFO_FULL (1u << 3)
#define STATUS_COMMAND_FSM(status) ((status) >> 4 & 0xF)
#define STATUS_DATA_MC_BUSY (1u << 10)
#define STATUS_FIFO_COUNT(status) ((status) >> 17 & 0x1FFF)

#define BLOCK 512
#define COMMAND_US 10000
#define DATA_US 100000

static inline void fence(void) {
    __asm__ volatile("fence iorw, iorw" ::: "memory");
}

uint32_t lp_micros(void) {
    static uint32_t last, micros, rest;
    const uint32_t cycle = RV_READ_CSR(mcycle);
    const uint32_t scaled = rest + (cycle - last) * LP_CORE_CYCLES_PER_US_DENOM;
    last = cycle;
    micros += scaled / LP_CORE_CYCLES_PER_US_NUM;
    rest = scaled % LP_CORE_CYCLES_PER_US_NUM;
    return micros;
}

void sdio_acquire(void) {
    bt_shared.want[1] = 1;
    fence();
    bt_shared.turn = 0;
    fence();
    while (bt_shared.want[0] && bt_shared.turn == 0) {
    }
    fence();
    // The HP core's driver turns DMA on again for each of its transfers.
    // The IDMAC is off too, so that it reads no descriptor while the LP
    // core uses the FIFO.
    SDMMC.ctrl.val &= ~(CTRL_DMA_ENABLE | CTRL_USE_IDMAC);
    SDMMC.bmod.val = 0;
}

void sdio_release(void) {
    // A read leaves its word in the FIFO, which the HP core's driver would
    // then send or read ahead of its own data.
    SDMMC.ctrl.val |= CTRL_FIFO_RESET;
    const uint32_t start = lp_micros();
    while ((SDMMC.ctrl.val & CTRL_FIFO_RESET) && lp_micros() - start < COMMAND_US) {
    }
    SDMMC.rintsts.val = ~0u;
    fence();
    bt_shared.want[1] = 0;
}

bool sdio_interrupted(void) {
    const uint32_t pin = bt_shared.d1_gpio;
    const uint32_t level = pin < 32 ? GPIO.in.val >> pin : GPIO.in1.val >> (pin - 32);
    return !(level & 1);
}

static bool expired(uint32_t start, uint32_t us) {
    return lp_micros() - start > us;
}

// Waits for the controller to finish what the HP core's driver started. Its
// driver lets go of the lock once a multi-block transfer's data has come,
// and the controller then sends CMD12 on its own.
static bool idle(uint32_t start, uint32_t us) {
    for (;;) {
        const uint32_t status = SDMMC.status.val;
        if (!(SDMMC.cmd.val & CMD_START) && !(status & STATUS_DATA_MC_BUSY) && STATUS_COMMAND_FSM(status) == 0) {
            return true;
        }
        if (expired(start, us)) return false;
    }
}

static bool send(uint32_t cmd, uint32_t arg) {
    const uint32_t start = lp_micros();
    if (!idle(start, DATA_US)) return false;
    // Also what the controller reported of the HP core's last command.
    SDMMC.rintsts.val = ~0u;
    SDMMC.cmdarg = arg;
    SDMMC.cmd.val = cmd | CMD_START | CMD_USE_HOLD | CMD_WAIT_PREVIOUS | (uint32_t)bt_shared.slot << 16;
    while (SDMMC.cmd.val & CMD_START) {
        if (expired(start, COMMAND_US)) return false;
    }
    return true;
}

static bool fail(uint32_t status) {
    bt_shared.bus_arg = SDMMC.cmdarg;
    bt_shared.bus_status = status;
    return false;
}

// Waits for the response; false after an error or a timeout.
static bool response(void) {
    const uint32_t start = lp_micros();
    uint32_t status;
    while (!((status = SDMMC.rintsts.val) & (INT_CMD_DONE | INT_CMD_ERRORS))) {
        if (expired(start, COMMAND_US)) return fail(status | 1u << 31);
    }
    if ((status & INT_CMD_ERRORS) || (SDMMC.resp[0] & R5_ERRORS)) return fail(status | (SDMMC.resp[0] & R5_ERRORS) << 8);
    return true;
}

static bool cmd52(uint32_t arg, uint8_t* value) {
    if (!send(52 | CMD_RESPONSE | CMD_CHECK_CRC, arg) || !response()) return false;
    if (value) *value = (uint8_t)SDMMC.resp[0];
    return true;
}

bool sdio_read_byte(uint8_t fn, uint32_t addr, uint8_t* value) {
    return cmd52((uint32_t)fn << 28 | (addr & 0x1FFFF) << 9, value);
}

bool sdio_write_byte(uint8_t fn, uint32_t addr, uint8_t value) {
    return cmd52(1u << 31 | (uint32_t)fn << 28 | (addr & 0x1FFFF) << 9 | value, NULL);
}

// Aborts a transfer that failed partway (SDIO 3.0, 6.9: the ASx bits of
// CCCR 0x06), so the card takes commands again.
static void abort_transfer(uint8_t fn) {
    SDMMC.ctrl.val |= CTRL_FIFO_RESET;
    const uint32_t start = lp_micros();
    while ((SDMMC.ctrl.val & CTRL_FIFO_RESET) && !expired(start, COMMAND_US)) {
    }
    cmd52(1u << 31 | 0x06 << 9 | fn, NULL);
}

// `bytes` go on the bus; a write sends `size` bytes of `data` and zeros
// after them, a read keeps `size` bytes.
static bool transfer(bool write, uint8_t fn, uint32_t addr, uint8_t* data, uint32_t size, uint32_t bytes,
                     bool block) {
    uint32_t start = lp_micros();
    if (!idle(start, DATA_US)) return fail(1u << 29);
    SDMMC.ctrl.val |= CTRL_FIFO_RESET;
    while (SDMMC.ctrl.val & CTRL_FIFO_RESET) {
        if (expired(start, DATA_US)) return fail(1u << 28);
    }
    SDMMC.blksiz.val = block ? BLOCK : bytes;
    SDMMC.bytcnt.val = bytes;
    // The count is a block count in block mode; in byte mode 0 means 512.
    const uint32_t arg = (uint32_t)write << 31 | (uint32_t)fn << 28 | (uint32_t)block << 27 | 1u << 26 |
                         (addr & 0x1FFFF) << 9 | (block ? 1 : bytes & 0x1FF);
    const uint32_t cmd = 53 | CMD_RESPONSE | CMD_CHECK_CRC | CMD_DATA | (write ? CMD_WRITE : 0);
    if (!send(cmd, arg)) {
        fail(1u << 30);
        abort_transfer(fn);
        return false;
    }
    if (!response()) {
        abort_transfer(fn);
        return false;
    }
    const uint32_t words = (bytes + 3) / 4;
    uint32_t done = 0;
    uint32_t status = 0;
    start = lp_micros();
    for (;;) {
        status = SDMMC.rintsts.val;
        if (status & INT_DATA_ERRORS) break;
        if (write) {
            while (done < words && !(SDMMC.status.val & STATUS_FIFO_FULL)) {
                uint32_t word = 0;
                for (uint32_t k = 0; k < 4; ++k) {
                    const uint32_t at = done * 4 + k;
                    if (at < size) word |= (uint32_t)data[at] << 8 * k;
                }
                SDMMC.buffifo.val = word;
                ++done;
            }
        } else {
            for (uint32_t count = STATUS_FIFO_COUNT(SDMMC.status.val); count && done < words; --count, ++done) {
                const uint32_t word = SDMMC.buffifo.val;
                for (uint32_t k = 0; k < 4; ++k) {
                    const uint32_t at = done * 4 + k;
                    if (at < size) data[at] = (uint8_t)(word >> 8 * k);
                }
            }
        }
        if ((status & INT_DTO) && (write || done == words || !STATUS_FIFO_COUNT(SDMMC.status.val))) break;
        if (expired(start, DATA_US)) {
            status |= INT_DRTO;
            break;
        }
    }
    if ((status & INT_DATA_ERRORS) || done < words) {
        fail(status | done << 20);
        abort_transfer(fn);
        return false;
    }
    return true;
}

bool sdio_read_bytes(uint8_t fn, uint32_t addr, void* data, uint32_t size) {
    uint8_t* out = data;
    for (uint32_t at = 0; at < size; at += 4) {
        const uint32_t n = size - at < 4 ? size - at : 4;
        if (!transfer(false, fn, addr + at, out + at, n, n, false)) return false;
    }
    return true;
}

bool sdio_write_bytes(uint8_t fn, uint32_t addr, const void* data, uint32_t size) {
    return transfer(true, fn, addr, (uint8_t*)data, size, (size + 3) & ~3u, false);
}

bool sdio_write_block(uint8_t fn, uint32_t addr, const void* data, uint32_t size) {
    return transfer(true, fn, addr, (uint8_t*)data, size, BLOCK, true);
}

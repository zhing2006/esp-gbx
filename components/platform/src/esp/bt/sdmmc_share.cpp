#include "sdmmc_share.h"

#include "driver/sdmmc_host.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal/sdmmc_ll.h"
#include "soc/sdmmc_reg.h"
#include "ulp_lp_core.h"

namespace gbx::platform::sdmmc_share {
namespace {

constexpr const char* kTag = "sdmmc_share";
// The LP core has the controller for one poll of the co-processor, well
// under a millisecond. Much longer means it has stopped while it had it.
constexpr int64_t kGiveUpUs = 500'000;
// For the co-processor's reading task to wait for it again after its last
// answer.
constexpr int64_t kParkUs = 1'000'000;

bt_shared_t* lp = nullptr;  // while the LP program may use the controller
int depth = 0;              // of lock() in the task that has the mutex
// The driver's interrupt masks, cleared while the LP core may have the
// controller: CTRL's global enable does not keep the IDMAC's interrupt
// from the CPU, and the handler would take the LP core's status bits.
uint32_t intmask = 0, idinten = 0;

inline void fence() {
    __asm__ volatile("fence iorw, iorw" ::: "memory");
}

SemaphoreHandle_t the_mutex() {
    static StaticSemaphore_t buffer;
    static SemaphoreHandle_t mutex = xSemaphoreCreateRecursiveMutexStatic(&buffer);
    return mutex;
}

void take_from_lp() {
    lp->want[0] = 1;
    fence();
    lp->turn = 1;
    fence();
    const int64_t start = esp_timer_get_time();
    while (lp->want[1] && lp->turn == 1) {
        if (esp_timer_get_time() - start > kGiveUpUs) {
            ESP_LOGE(kTag, "the LP core kept the SDMMC controller; stopping it");
            ulp_lp_core_stop();
            // As the LP program leaves it when it fails, for the gamepad's
            // task to start it over.
            lp->held = 0;
            lp->error = BT_ERROR_STALLED;
            lp->state = BT_STATE_FAILED;
            lp = nullptr;
            break;
        }
    }
    fence();
    // The driver starts each transfer with the IDMAC at its first
    // descriptor, which holds only while the IDMAC has not moved since the
    // driver's last transfer. The LP core's programmed I/O goes through the
    // FIFO, so the IDMAC, the DMA interface and the FIFO start over.
    SDMMC.ctrl.val |= SDHOST_FIFO_RESET | SDHOST_DMA_RESET;
    const int64_t reset = esp_timer_get_time();
    while ((SDMMC.ctrl.val & (SDHOST_FIFO_RESET | SDHOST_DMA_RESET)) && esp_timer_get_time() - reset < 1000) {
    }
    SDMMC.bmod.sw_reset = 1;
    // Status left from before: the LP core's, and the end of the driver's
    // last transfer, which the handler would otherwise report only now and
    // in the middle of the driver's next transaction.
    SDMMC.rintsts.val = ~0u;
    SDMMC.idsts.val = ~0u;
    SDMMC.intmask.val = intmask;
    SDMMC.idinten.val = idinten;
    sdmmc_ll_enable_global_interrupt(&SDMMC, true);
}

void give_to_lp() {
    intmask = SDMMC.intmask.val;
    idinten = SDMMC.idinten.val;
    SDMMC.intmask.val = 0;
    SDMMC.idinten.val = 0;
    sdmmc_ll_enable_global_interrupt(&SDMMC, false);
    fence();
    lp->want[0] = 0;
}

esp_err_t set_bus_width(int slot, size_t width) {
    lock();
    const esp_err_t err = sdmmc_host_set_bus_width(slot, width);
    unlock();
    return err;
}

esp_err_t set_bus_ddr_mode(int slot, bool ddr) {
    lock();
    const esp_err_t err = sdmmc_host_set_bus_ddr_mode(slot, ddr);
    unlock();
    return err;
}

esp_err_t set_card_clk(int slot, uint32_t khz) {
    lock();
    const esp_err_t err = sdmmc_host_set_card_clk(slot, khz);
    unlock();
    return err;
}

esp_err_t set_cclk_always_on(int slot, bool on) {
    lock();
    const esp_err_t err = sdmmc_host_set_cclk_always_on(slot, on);
    unlock();
    return err;
}

esp_err_t set_input_delay(int slot, sdmmc_delay_phase_t phase) {
    lock();
    const esp_err_t err = sdmmc_host_set_input_delay(slot, phase);
    unlock();
    return err;
}

esp_err_t set_input_delayline(int slot, sdmmc_delay_line_t line) {
    lock();
    const esp_err_t err = sdmmc_host_set_input_delayline(slot, line);
    unlock();
    return err;
}

}  // namespace

void wrap(sdmmc_host_t& host) {
    host.set_bus_width = set_bus_width;
    host.set_bus_ddr_mode = set_bus_ddr_mode;
    host.set_card_clk = set_card_clk;
    host.set_cclk_always_on = set_cclk_always_on;
    host.set_input_delay = set_input_delay;
    host.set_input_delayline = set_input_delayline;
}

void lock() {
    xSemaphoreTakeRecursive(the_mutex(), portMAX_DELAY);
    if (depth++ == 0 && lp) take_from_lp();
}

void unlock() {
    if (--depth == 0 && lp) give_to_lp();
    xSemaphoreGiveRecursive(the_mutex());
}

bool attach(bt_shared_t* shared, int slot, TaskHandle_t reader) {
    // The reader waits in sdmmc_io_wait_int(), which unmasks the interrupt
    // just before it blocks; the handler masks it as it wakes the task, and
    // nothing else unmasks it. Suspended with the interrupt unmasked, the
    // task is in that wait, in no transaction and holding no lock.
    const uint32_t io = slot == 0 ? SDMMC_INTMASK_IO_SLOT0 : SDMMC_INTMASK_IO_SLOT1;
    const int64_t start = esp_timer_get_time();
    for (;;) {
        vTaskSuspend(reader);
        while (eTaskGetState(reader) != eSuspended) vTaskDelay(1);  // still running on the other core
        if (SDMMC.intmask.val & io) break;
        vTaskResume(reader);
        if (esp_timer_get_time() - start > kParkUs) return false;
        vTaskDelay(1);
    }
    lock();
    sdmmc_ll_enable_interrupt(&SDMMC, io, false);
    sdmmc_ll_clear_interrupt(&SDMMC, io);
    shared->want[0] = 0;
    shared->want[1] = 0;
    shared->turn = 0;
    fence();
    lp = shared;
    // unlock() turns the interrupt off now that the LP core may have it.
    unlock();
    return true;
}

void detach() {
    lock();
    lp = nullptr;
    unlock();  // with the interrupt on, as without the LP core
}

}  // namespace gbx::platform::sdmmc_share

// The linker sends every other object's calls here (CMakeLists.txt).
extern "C" {
esp_err_t __real_sdmmc_host_do_transaction(int slot, sdmmc_command_t* cmd);
esp_err_t __real_sdmmc_host_deinit_slot(int slot);

esp_err_t __wrap_sdmmc_host_do_transaction(int slot, sdmmc_command_t* cmd) {
    gbx::platform::sdmmc_share::lock();
    const esp_err_t err = __real_sdmmc_host_do_transaction(slot, cmd);
    gbx::platform::sdmmc_share::unlock();
    return err;
}

esp_err_t __wrap_sdmmc_host_deinit_slot(int slot) {
    gbx::platform::sdmmc_share::lock();
    const esp_err_t err = __real_sdmmc_host_deinit_slot(slot);
    gbx::platform::sdmmc_share::unlock();
    return err;
}
}

#include <algorithm>
#include <atomic>
#include <cinttypes>
#include <cstring>

#include "bt_shared.h"
#include "eh_host_feat_bt.h"
#include "eh_host_mcu_transport_init_event.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_hosted.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gamepads.h"
#include "gbx/gamepad.h"
#include "gbx/worker.h"
#include "hid_gamepad.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "sdmmc_share.h"
#include "ulp_bt.h"
#include "ulp_lp_core.h"

// The Bluetooth gamepad on the HP core. The board's C5 or C6 is the
// Bluetooth controller, reached over SDIO through ESP-Hosted. The LP core's
// program (lp/) has the link while it runs: it connects to the saved
// gamepad, reads its reports and leaves the keys in bt_shared, where read()
// takes them. The HP core does what happens once. At startup it brings
// ESP-Hosted up and hands the SDIO slot to the LP program. To pair, it takes
// the slot back and resets the co-processor, which forgets what the LP
// program did, runs NimBLE to find, pair with and read a gamepad, saves what
// the LP program needs in NVS, then resets the co-processor again for the LP
// program. One task does it all, on core 0, the UI's, at priority 2 as the
// USB gamepad's tasks; it and NimBLE's task have their stacks in PSRAM.
// NVS goes through a worker with an internal stack: writing flash turns the
// cache off. NimBLE keeps its bonds in RAM and its memory in PSRAM.

extern "C" {
// esp_hosted's NimBLE transport (examples/common_components/
// esp_hosted_bt_host_stack), without its controller calls, which a
// co-processor before the version RPC does not answer.
esp_err_t eh_bt_bind_nimble(void);
void eh_bt_unbind_nimble(void);
void ble_store_config_init(void);
}

extern const uint8_t lp_bin_start[] asm("_binary_ulp_bt_bin_start");
extern const uint8_t lp_bin_end[] asm("_binary_ulp_bt_bin_end");

namespace gbx::platform {
namespace {

using gamepad::Failure;
using gamepad::Pairing;

constexpr const char* kTag = "bt-gamepad";
constexpr BaseType_t kCore = 0;
constexpr UBaseType_t kPriority = 2;
constexpr uint32_t kStackBytes = 6144;
constexpr uint32_t kNvsStackBytes = 3072;

// Milliseconds.
constexpr uint32_t kSearchMs = 60000;
constexpr uint32_t kConnectMs = 10000;
constexpr uint32_t kSecureMs = 30000;  // SMP's own timeout (Vol 3 Part H, 3.4)
constexpr uint32_t kRequestMs = 10000;
constexpr uint32_t kSyncMs = 5000;
constexpr uint32_t kStopMs = 1000;
constexpr uint32_t kWatchMs = 1000;
constexpr uint32_t kRestartMs = 10000;

constexpr const char* kNamespace = "gamepad";
constexpr const char* kKey = "bt";
constexpr uint32_t kSavedVersion = BT_SHARED_VERSION;

// Linux numbers a Game Pad's Button usages from BTN_SOUTH
// (drivers/hid/hid-input.c): A, B, C, X, Y, Z, TL, TR, TL2, TR2, Select,
// Start, Mode, the bottom face button first and the right one second.
// Android takes that order, and Bluetooth gamepads made for it follow it.
// A and B go by position, as on the USB gamepads: the right face button is
// A, the bottom one B.
constexpr uint16_t kButtonKeys[HID_BUTTONS] = {
    HID_KEY_B, HID_KEY_A, 0, 0, 0, 0, HID_KEY_L, HID_KEY_R, 0, 0, HID_KEY_SELECT, HID_KEY_START, HID_KEY_MENU};

// What NVS keeps.
struct Saved {
    uint32_t version;
    bt_bond_t bond;
    char name[sizeof(gamepad::Status::name)];
};

// Bits of the task's notification.
constexpr uint32_t kPairBit = 1, kCancelBit = 2;

std::atomic<TaskHandle_t> task{nullptr};
Worker task_worker;

// The task's.
Saved saved{};     // version 0 when nothing is paired
bool hosted_up = false;
uint64_t restart_at = 0;

// read()'s, on the app's task.
uint32_t seen_generation = 0;
uint8_t seen_presses[16] = {};

std::atomic<bool> running{false};  // the LP program has the slot
std::atomic<uint32_t> generation{0};  // of LP program starts
std::atomic<bool> cancelling{false};

portMUX_TYPE status_lock = portMUX_INITIALIZER_UNLOCKED;
gamepad::Status status_now;

bt_shared_t* shared() {
    return reinterpret_cast<bt_shared_t*>(&ulp_bt_shared);
}

void set_status(Pairing pairing, Failure failure = Failure::none, const char* name = nullptr) {
    portENTER_CRITICAL(&status_lock);
    status_now.pairing = pairing;
    status_now.failure = failure;
    status_now.paired = saved.version != 0;
    const char* shown = name ? name : saved.name;
    std::strncpy(status_now.name, shown, sizeof status_now.name - 1);
    status_now.name[sizeof status_now.name - 1] = 0;
    portEXIT_CRITICAL(&status_lock);
}

// NVS, on a worker with an internal stack.

struct NvsJob {
    Saved* saved;
    bool write;
    bool ok;
};

void nvs_job(void* context) {
    auto& job = *static_cast<NvsJob*>(context);
    job.ok = false;
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        // The partition holds nothing else to keep.
        err = nvs_flash_erase();
        if (err == ESP_OK) err = nvs_flash_init();
    }
    nvs_handle_t handle;
    if (err == ESP_OK) err = nvs_open(kNamespace, job.write ? NVS_READWRITE : NVS_READONLY, &handle);
    if (err != ESP_OK) {
        if (err != ESP_ERR_NVS_NOT_FOUND) ESP_LOGE(kTag, "NVS: %s", esp_err_to_name(err));
        return;
    }
    if (job.write) {
        err = nvs_set_blob(handle, kKey, job.saved, sizeof *job.saved);
        if (err == ESP_OK) err = nvs_commit(handle);
        if (err != ESP_OK) ESP_LOGE(kTag, "NVS: %s", esp_err_to_name(err));
        job.ok = err == ESP_OK;
    } else {
        size_t size = sizeof *job.saved;
        job.ok = nvs_get_blob(handle, kKey, job.saved, &size) == ESP_OK && size == sizeof *job.saved &&
                 job.saved->version == kSavedVersion;
    }
    nvs_close(handle);
}

bool run_nvs(Saved& data, bool write) {
    NvsJob job{&data, write, false};
    Worker worker;
    if (!worker.start(nvs_job, &job, kCore, "bt-nvs", kNvsStackBytes, kPriority, StackMemory::internal)) {
        return false;
    }
    worker.join();
    return job.ok;
}

// ESP-Hosted and the LP program.

// ESP-Hosted sets up the SDMMC controller, unless the SD card's driver has,
// and adds the co-processor's slot as it starts; the controller goes with
// the last slot removed. The SD card's driver sets up and adds its slot
// under sdmmc_share's lock, and so does ESP-Hosted here: of two set-ups at
// once one fails, and a slot removed between the two steps frees the
// controller under them. Removing a slot takes the lock anyway, see
// sdmmc_share.h; esp_hosted_deinit() is not to hold it, as it waits for
// ESP-Hosted's tasks, whose transactions take it.
int hosted_init() {
    sdmmc_share::lock();
    const int rc = esp_hosted_init();
    sdmmc_share::unlock();
    return rc;
}

// Resets the co-processor and brings the link up, Bluetooth on.
bool reset_hosted() {
    if (hosted_up) {
        esp_hosted_deinit();
        hosted_up = false;
    }
    if (hosted_init() != 0 || esp_hosted_connect_to_slave() != 0) {
        ESP_LOGE(kTag, "the co-processor does not answer");
        esp_hosted_deinit();
        return false;
    }
    hosted_up = true;
    // A co-processor before ESP-Hosted's version RPC reports version 0. It
    // turns its controller on at startup and answers no controller RPC.
    if (eh_host_mcu_transport_get_fw_version() == 0) return true;
    esp_err_t err = ESP_FAIL;
    for (int tries = 0; tries < 50; ++tries) {
        err = eh_host_bt_controller_init();
        if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) break;
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (err == ESP_OK || err == ESP_ERR_INVALID_STATE) err = eh_host_bt_controller_enable();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(kTag, "Bluetooth controller: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

bool start_runtime() {
    esp_err_t err = ulp_lp_core_load_binary(lp_bin_start, lp_bin_end - lp_bin_start);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "LP program: %s", esp_err_to_name(err));
        return false;
    }
    // Loading cleared LP SRAM; the program reads this as it starts.
    bt_shared_t* s = shared();
    s->version = BT_SHARED_VERSION;
    s->slot = CONFIG_ESP_HOSTED_HOST_SDIO_SLOT;
    s->d1_gpio = CONFIG_ESP_HOSTED_HOST_SDIO_PIN_D1;
    s->aligned = eh_host_mcu_transport_sdio_sw_aggr_negotiated();
    s->bond = saved.bond;
    // ESP-Hosted's reading task (host/mcu/eh_host_mcu_transport/src/
    // eh_host_bus_sdio.c), which its stop deletes.
    TaskHandle_t reader = xTaskGetHandle("sdio_read");
    if (!reader || !sdmmc_share::attach(s, CONFIG_ESP_HOSTED_HOST_SDIO_SLOT, reader)) {
        ESP_LOGE(kTag, "ESP-Hosted is still reading from the co-processor");
        return false;
    }
    ulp_lp_core_cfg_t config = {};
    config.wakeup_source = ULP_LP_CORE_WAKEUP_SOURCE_HP_CPU;
    config.skip_lp_rom_boot = true;
    err = ulp_lp_core_run(&config);
    if (err != ESP_OK) {
        sdmmc_share::detach();
        ESP_LOGE(kTag, "LP program: %s", esp_err_to_name(err));
        return false;
    }
    generation.fetch_add(1, std::memory_order_relaxed);
    running.store(true, std::memory_order_release);
    return true;
}

void stop_runtime() {
    if (!running.load(std::memory_order_relaxed)) return;
    running.store(false, std::memory_order_relaxed);
    bt_shared_t* s = shared();
    s->stop = 1;
    const int64_t start = esp_timer_get_time();
    while (s->state != BT_STATE_STOPPED && s->state != BT_STATE_FAILED &&
           esp_timer_get_time() - start < kStopMs * 1000) {
        vTaskDelay(1);
    }
    ulp_lp_core_stop();
    sdmmc_share::detach();
}

// Resets the co-processor and starts the LP program for the saved gamepad.
// When either fails, watch() tries again later.
void restart() {
    restart_at = esp_timer_get_time() + kRestartMs * 1000ull;
    if (reset_hosted()) start_runtime();
}

// Restarts the LP program for the saved gamepad when it has failed or did
// not start.
void watch() {
    if (!saved.version) return;
    if (running.load(std::memory_order_relaxed)) {
        bt_shared_t* s = shared();
        if (s->state != BT_STATE_FAILED) return;
        ESP_LOGE(kTag, "LP program failed: error %u, SDIO argument %08" PRIx32 " status %08" PRIx32,
                 static_cast<unsigned>(s->error), s->bus_arg, s->bus_status);
        stop_runtime();
    }
    if (esp_timer_get_time() >= restart_at) restart();
}

// Pairing with NimBLE. Its callbacks run on its task and hand results to
// the waits on the gamepad task through `event`.

struct Report {
    uint16_t definition, value, cccd, reference;
    uint8_t properties, id, type;
};
constexpr int kMaxReports = 16;
constexpr size_t kMaxValue = 512;  // the longest attribute value (Vol 3 Part F, 3.2.9)

struct Link {
    SemaphoreHandle_t event = nullptr;
    StaticSemaphore_t event_buffer;
    SemaphoreHandle_t host_done = nullptr;
    StaticSemaphore_t host_done_buffer;
    TaskHandle_t host = nullptr;
    std::atomic<bool> synced{false};
    uint8_t own_type = BLE_OWN_ADDR_PUBLIC;
    volatile int status = 0;
    uint16_t connection = BLE_HS_CONN_HANDLE_NONE;
    bool found = false;
    ble_addr_t target{};
    char name[sizeof(gamepad::Status::name)] = {};
    uint16_t service_start = 0, service_end = 0;
    Report reports[kMaxReports];
    int report_count = 0;
    Report* described = nullptr;
    uint16_t value_size = 0;
    uint16_t name_handle = 0;
};

Link link;
EXT_RAM_BSS_ATTR uint8_t value[kMaxValue];
const ble_uuid16_t kHidService = BLE_UUID16_INIT(0x1812);
const ble_uuid16_t kDeviceName = BLE_UUID16_INIT(0x2A00);

void finish(int status) {
    link.status = status;
    xSemaphoreGive(link.event);
}

// Waits for finish(); BLE_HS_ETIMEOUT after `ms`, BLE_HS_EAPP once
// cancelled.
int wait(uint32_t ms) {
    const int64_t end = esp_timer_get_time() + ms * 1000ll;
    for (;;) {
        if (cancelling.load(std::memory_order_relaxed)) return BLE_HS_EAPP;
        const int64_t left = end - esp_timer_get_time();
        if (left <= 0) return BLE_HS_ETIMEOUT;
        const uint32_t slice = left > 100000 ? 100 : static_cast<uint32_t>(left / 1000) + 1;
        if (xSemaphoreTake(link.event, pdMS_TO_TICKS(slice)) == pdTRUE) return link.status;
    }
}

void arm() {
    xSemaphoreTake(link.event, 0);
}

void copy_name(const uint8_t* name, size_t size) {
    size = std::min(size, sizeof link.name - 1);
    std::memcpy(link.name, name, size);
    link.name[size] = 0;
}

// A gamepad in pairing mode: connectable and discoverable, with the HID
// service or a HID appearance (Assigned Numbers, 2.6.2) other than those of
// keyboards, mice and the like: generic HID 0x03C0, joystick 0x03C3 or
// gamepad 0x03C4.
bool pairable(const ble_gap_disc_desc& desc, const ble_hs_adv_fields& fields) {
    if (desc.event_type != BLE_HCI_ADV_RPT_EVTYPE_ADV_IND) return false;
    if (!(fields.flags & (BLE_HS_ADV_F_DISC_LTD | BLE_HS_ADV_F_DISC_GEN))) return false;
    if (fields.appearance_is_present && fields.appearance != 0) {
        return fields.appearance == 0x03C0 || fields.appearance == 0x03C3 || fields.appearance == 0x03C4;
    }
    for (int i = 0; i < fields.num_uuids16; ++i) {
        if (ble_uuid_u16(&fields.uuids16[i].u) == 0x1812) return true;
    }
    return false;
}

int on_gap(ble_gap_event* event, void*) {
    switch (event->type) {
        case BLE_GAP_EVENT_DISC: {
            ble_hs_adv_fields fields;
            if (link.found || ble_hs_adv_parse_fields(&fields, event->disc.data, event->disc.length_data) != 0 ||
                !pairable(event->disc, fields)) {
                return 0;
            }
            link.found = true;
            link.target = event->disc.addr;
            if (fields.name) copy_name(fields.name, fields.name_len);
            ble_gap_disc_cancel();
            finish(0);
            return 0;
        }
        case BLE_GAP_EVENT_DISC_COMPLETE:
            finish(link.found ? 0 : BLE_HS_ETIMEOUT);
            return 0;
        case BLE_GAP_EVENT_CONNECT:
            if (event->connect.status == 0) link.connection = event->connect.conn_handle;
            finish(event->connect.status);
            return 0;
        case BLE_GAP_EVENT_DISCONNECT:
            link.connection = BLE_HS_CONN_HANDLE_NONE;
            finish(BLE_HS_ENOTCONN);
            return 0;
        case BLE_GAP_EVENT_ENC_CHANGE:
            finish(event->enc_change.status);
            return 0;
        case BLE_GAP_EVENT_REPEAT_PAIRING: {
            // A bond NimBLE kept from an earlier pairing in this boot.
            ble_gap_conn_desc desc;
            if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
                ble_store_util_delete_peer(&desc.peer_id_addr);
            }
            return BLE_GAP_REPEAT_PAIRING_RETRY;
        }
        default:
            return 0;
    }
}

void on_sync() {
    ble_hs_util_ensure_addr(0);
    ble_hs_id_infer_auto(0, &link.own_type);
    link.synced.store(true);
}

void on_reset(int reason) {
    ESP_LOGW(kTag, "NimBLE reset: %d", reason);
}

void host_loop(void*) {
    nimble_port_run();
    xSemaphoreGive(link.host_done);
    vTaskSuspend(nullptr);
}

void nimble_stop();

// Binds NimBLE to ESP-Hosted's HCI and starts it; nothing to stop when it
// fails.
bool nimble_start() {
    if (!link.event) {
        link.event = xSemaphoreCreateBinaryStatic(&link.event_buffer);
        link.host_done = xSemaphoreCreateBinaryStatic(&link.host_done_buffer);
    }
    if (eh_bt_bind_nimble() != ESP_OK) return false;
    if (nimble_port_init() != ESP_OK) {
        eh_bt_unbind_nimble();
        return false;
    }
    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;
    // Just Works: a gamepad has no display or keyboard.
    ble_hs_cfg.sm_io_cap = BLE_SM_IO_CAP_NO_IO;
    ble_hs_cfg.sm_bonding = 1;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_hs_cfg.sm_our_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    ble_store_config_init();
    ble_store_clear();
    link.synced.store(false);
    xSemaphoreTake(link.host_done, 0);
    if (xTaskCreatePinnedToCoreWithCaps(host_loop, "nimble", CONFIG_BT_NIMBLE_HOST_TASK_STACK_SIZE, nullptr,
                                        kPriority, &link.host, kCore,
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
        link.host = nullptr;
        nimble_port_deinit();
        eh_bt_unbind_nimble();
        return false;
    }
    const int64_t end = esp_timer_get_time() + kSyncMs * 1000ll;
    while (!link.synced.load() && esp_timer_get_time() < end) vTaskDelay(pdMS_TO_TICKS(10));
    if (link.synced.load()) return true;
    ESP_LOGE(kTag, "NimBLE did not start");
    nimble_stop();
    return false;
}

// Unbound first: NimBLE ends its connection as it stops, as Remote User
// Terminated Connection, and an Xbox Wireless Controller turns off on that.
// Unbound, the command fails at once, and the reset of the co-processor
// that follows ends the link; the gamepad then reconnects.
void nimble_stop() {
    eh_bt_unbind_nimble();
    if (link.host) {
        nimble_port_stop();
        xSemaphoreTake(link.host_done, portMAX_DELAY);
        while (eTaskGetState(link.host) != eSuspended) vTaskDelay(1);
        vTaskDeleteWithCaps(link.host);
        link.host = nullptr;
    }
    nimble_port_deinit();
}

uint16_t uuid16(const ble_uuid_any_t& uuid) {
    return uuid.u.type == BLE_UUID_TYPE_16 ? ble_uuid_u16(&uuid.u) : 0;
}

int on_service(uint16_t, const ble_gatt_error* error, const ble_gatt_svc* service, void*) {
    if (error->status == 0) {
        if (!link.service_start) {
            link.service_start = service->start_handle;
            link.service_end = service->end_handle;
        }
        return 0;
    }
    finish(error->status == BLE_HS_EDONE ? 0 : error->status);
    return 0;
}

int on_characteristic(uint16_t, const ble_gatt_error* error, const ble_gatt_chr* chr, void*) {
    if (error->status == 0) {
        if (link.report_count < kMaxReports) {
            Report& r = link.reports[link.report_count++];
            r = {};
            r.definition = chr->def_handle;
            r.value = chr->val_handle;
            r.properties = chr->properties;
            // Only the Report characteristics (0x2A4D) and the Report Map
            // (0x2A4B) matter; the type field keeps which.
            const uint16_t uuid = uuid16(chr->uuid);
            r.type = uuid == 0x2A4D ? 0xFF : uuid == 0x2A4B ? 0xFE : 0;
        }
        return 0;
    }
    finish(error->status == BLE_HS_EDONE ? 0 : error->status);
    return 0;
}

int on_descriptor(uint16_t, const ble_gatt_error* error, uint16_t, const ble_gatt_dsc* dsc, void*) {
    if (error->status == 0) {
        const uint16_t uuid = uuid16(dsc->uuid);
        if (uuid == 0x2902) link.described->cccd = dsc->handle;
        if (uuid == 0x2908) link.described->reference = dsc->handle;
        return 0;
    }
    finish(error->status == BLE_HS_EDONE ? 0 : error->status);
    return 0;
}

int on_read(uint16_t, const ble_gatt_error* error, ble_gatt_attr* attr, void* is_long) {
    if (error->status == 0 && attr) {
        const uint16_t size = std::min<uint16_t>(OS_MBUF_PKTLEN(attr->om), kMaxValue - link.value_size);
        os_mbuf_copydata(attr->om, 0, size, value + link.value_size);
        link.value_size += size;
        if (!is_long) finish(0);
        return 0;
    }
    finish(error->status == BLE_HS_EDONE ? 0 : error->status);
    return 0;
}

int read_value(uint16_t handle, bool is_long) {
    link.value_size = 0;
    arm();
    const int rc = is_long ? ble_gattc_read_long(link.connection, handle, 0, on_read, reinterpret_cast<void*>(1))
                           : ble_gattc_read(link.connection, handle, on_read, nullptr);
    return rc ? rc : wait(kRequestMs);
}

int on_name_handle(uint16_t, const ble_gatt_error* error, ble_gatt_attr* attr, void*) {
    if (error->status == 0 && attr) {
        if (!link.name_handle) link.name_handle = attr->handle;
        return 0;
    }
    finish(error->status == BLE_HS_EDONE ? 0 : error->status);
    return 0;
}

// GAP's Device Name (Vol 3 Part C, 12.1): found by its type, then read
// whole, as a Read By Type Response holds only ATT_MTU - 4 bytes of it.
int read_name() {
    link.name_handle = 0;
    arm();
    int rc = ble_gattc_read_by_uuid(link.connection, 1, 0xFFFF, &kDeviceName.u, on_name_handle, nullptr);
    if (rc == 0) rc = wait(kRequestMs);
    if (rc != 0) return rc;
    return link.name_handle ? read_value(link.name_handle, true) : BLE_HS_ENOENT;
}

// The HID service (HID over GATT Profile 1.0, 2.6): its reports, what each
// is, and the report map, into the bond.
Failure read_gamepad(Saved& out) {
    link.service_start = link.service_end = 0;
    link.report_count = 0;
    arm();
    int rc = ble_gattc_disc_svc_by_uuid(link.connection, &kHidService.u, on_service, nullptr);
    if (rc == 0) rc = wait(kRequestMs);
    if (rc != 0 || !link.service_start) {
        if (rc != BLE_HS_EAPP) ESP_LOGW(kTag, "no HID service: %d", rc);
        return rc == BLE_HS_EAPP ? Failure::none : Failure::unsupported;
    }
    arm();
    rc = ble_gattc_disc_all_chrs(link.connection, link.service_start, link.service_end, on_characteristic, nullptr);
    if (rc == 0) rc = wait(kRequestMs);
    for (int i = 0; i < link.report_count && rc == 0; ++i) {
        const uint16_t end = i + 1 < link.report_count ? link.reports[i + 1].definition - 1 : link.service_end;
        if (end <= link.reports[i].value) continue;
        link.described = &link.reports[i];
        arm();
        rc = ble_gattc_disc_all_dscs(link.connection, link.reports[i].value, end, on_descriptor, nullptr);
        if (rc == 0) rc = wait(kRequestMs);
    }
    if (rc != 0) {
        if (rc != BLE_HS_EAPP) ESP_LOGW(kTag, "HID characteristics: %d", rc);
        return rc == BLE_HS_EAPP ? Failure::none : Failure::refused;
    }

    // Report Reference (HID over GATT 1.0, 2.6.2): the report's ID and type,
    // 1 for input.
    for (int i = 0; i < link.report_count; ++i) {
        Report& r = link.reports[i];
        if (r.type != 0xFF) continue;
        r.type = 0;
        if (!r.reference) continue;
        rc = read_value(r.reference, false);
        if (rc == BLE_HS_EAPP) return Failure::none;
        if (rc == 0 && link.value_size >= 2) {
            r.id = value[0];
            r.type = value[1];
        }
    }
    hid_layout_t layout{};
    bool have_map = false;
    for (int i = 0; i < link.report_count && !have_map; ++i) {
        if (link.reports[i].type != 0xFE) continue;
        rc = read_value(link.reports[i].value, true);
        if (rc == BLE_HS_EAPP) return Failure::none;
        if (rc != 0) {
            ESP_LOGW(kTag, "report map: %d", rc);
            return Failure::refused;
        }
        layout = hid::parse(value, link.value_size, kButtonKeys);
        have_map = true;
    }
    if (!have_map || !hid::has_fields(layout)) {
        ESP_LOGW(kTag, "no gamepad in the report map (%u bytes)", have_map ? link.value_size : 0u);
        return Failure::unsupported;
    }

    bt_bond_t& bond = out.bond;
    bond.layout = layout;
    bond.input_count = 0;
    for (int i = 0; i < link.report_count && bond.input_count < BT_MAX_INPUTS; ++i) {
        const Report& r = link.reports[i];
        if (r.type != 1 || !r.cccd || !(r.properties & BLE_GATT_CHR_PROP_NOTIFY)) continue;
        if (layout.ids && r.id != layout.report) continue;
        auto& input = bond.inputs[bond.input_count++];
        input.value = r.value;
        input.cccd = r.cccd;
        input.report_id = r.id;
    }
    if (bond.input_count == 0) {
        ESP_LOGW(kTag, "no input report to subscribe to");
        return Failure::unsupported;
    }

    if (!link.name[0]) {
        rc = read_name();
        if (rc == BLE_HS_EAPP) return Failure::none;
        if (rc == 0) copy_name(value, link.value_size);
    }
    std::memcpy(out.name, link.name, sizeof out.name);
    return Failure::none;
}

// The keys the LP program encrypts the link with, from NimBLE's store.
bool read_keys(bt_bond_t& bond) {
    ble_gap_conn_desc desc;
    if (ble_gap_conn_find(link.connection, &desc) != 0) return false;
    ble_store_key_sec key = {};
    key.peer_addr = desc.peer_id_addr;
    ble_store_value_sec sec;
    if (ble_store_read_peer_sec(&key, &sec) != 0 || !sec.ltk_present) return false;
    bond.addr_type = desc.peer_id_addr.type;
    std::memcpy(bond.addr, desc.peer_id_addr.val, 6);
    std::memcpy(bond.ltk, sec.ltk, 16);
    bond.ediv = sec.ediv;
    bond.rand = sec.rand_num;
    bond.key_size = sec.key_size;
    bond.resolve = sec.irk_present;
    if (sec.irk_present) std::memcpy(bond.irk, sec.irk, 16);
    return true;
}

// Finds, connects to, pairs with and reads a gamepad. Failure::none with
// cancelling set when cancelled.
Failure find_and_pair(Saved& out) {
    set_status(Pairing::searching, Failure::none, "");
    link.found = false;
    link.name[0] = 0;
    link.connection = BLE_HS_CONN_HANDLE_NONE;
    ble_gap_disc_params scan = {};
    scan.filter_duplicates = 1;
    arm();
    int rc = ble_gap_disc(link.own_type, kSearchMs, &scan, on_gap, nullptr);
    if (rc != 0) return Failure::radio;
    rc = wait(kSearchMs + 2000);
    if (rc != 0 || !link.found) {
        ble_gap_disc_cancel();
        return rc == BLE_HS_EAPP ? Failure::none : Failure::not_found;
    }
    set_status(Pairing::connecting, Failure::none, link.name);
    vTaskDelay(pdMS_TO_TICKS(50));  // for the scan to end in the controller
    arm();
    rc = ble_gap_connect(link.own_type, &link.target, kConnectMs, nullptr, on_gap, nullptr);
    if (rc == 0) rc = wait(kConnectMs + 2000);
    if (rc != 0) {
        ble_gap_conn_cancel();
        if (rc != BLE_HS_EAPP) ESP_LOGW(kTag, "connection: %d", rc);
        return rc == BLE_HS_EAPP ? Failure::none : Failure::refused;
    }
    arm();
    rc = ble_gap_security_initiate(link.connection);
    if (rc == 0) rc = wait(kSecureMs);
    Failure failure = Failure::none;
    if (rc != 0) {
        if (rc != BLE_HS_EAPP) ESP_LOGW(kTag, "pairing: %d", rc);
        failure = rc == BLE_HS_EAPP ? Failure::none : Failure::refused;
    } else {
        failure = read_gamepad(out);
        if (failure == Failure::none && !cancelling.load() && !read_keys(out.bond)) {
            ESP_LOGW(kTag, "no key from the pairing");
            failure = Failure::refused;
        }
    }
    return failure;
}

void pair_now() {
    stop_runtime();
    Saved paired{};
    Failure failure = Failure::none;
    if (!cancelling.load()) {
        failure = Failure::radio;
        if (reset_hosted() && nimble_start()) {
            failure = find_and_pair(paired);
            nimble_stop();
        }
    }
    const bool cancelled = cancelling.load();
    if (!cancelled && failure == Failure::none) {
        paired.version = kSavedVersion;
        if (run_nvs(paired, true)) {
            saved = paired;
        } else {
            failure = Failure::storage;
        }
    }
    // The co-processor may keep a link, the LP program's or NimBLE's, which
    // its reset ends without a Disconnect, see nimble_stop(). It starts over
    // for the LP program too, which does not pick up what NimBLE did. Its
    // controller keeps no bonds.
    if (saved.version) {
        restart();
    } else if (hosted_up && reset_hosted()) {
        esp_hosted_deinit();
        hosted_up = false;
    }
    if (cancelled) {
        set_status(Pairing::idle);
    } else if (failure == Failure::none) {
        set_status(Pairing::paired);
    } else {
        ESP_LOGW(kTag, "pairing failed: %d", static_cast<int>(failure));
        set_status(Pairing::failed, failure);
    }
    cancelling.store(false);
}

void run(void*) {
    task.store(xTaskGetCurrentTaskHandle());
    if (!run_nvs(saved, false)) saved = {};
    set_status(Pairing::idle);
    // Without a saved gamepad the co-processor waits for a pairing.
    watch();
    for (;;) {
        uint32_t bits = 0;
        xTaskNotifyWait(0, ~0u, &bits, pdMS_TO_TICKS(kWatchMs));
        if (bits & kPairBit) pair_now();
        watch();
    }
}

}  // namespace

namespace bt_gamepad {

bool start() {
    // The LP core runs on through a reset of the HP core, and with it the
    // program on the SDMMC controller, which the SD card is about to have.
    ulp_lp_core_stop();
    if (task_worker.start(run, nullptr, kCore, "bt-gamepad", kStackBytes, kPriority, StackMemory::external)) {
        return true;
    }
    ESP_LOGE(kTag, "no memory for the task");
    return false;
}

uint16_t read() {
    if (!running.load(std::memory_order_acquire)) return 0;
    const uint32_t g = generation.load(std::memory_order_relaxed);
    if (g != seen_generation) {
        seen_generation = g;
        std::memset(seen_presses, 0, sizeof seen_presses);
    }
    const bt_shared_t* s = shared();
    uint16_t keys = static_cast<uint16_t>(s->held);
    for (int k = 0; k < 16; ++k) {
        const uint8_t presses = s->presses[k];
        if (presses != seen_presses[k]) {
            seen_presses[k] = presses;
            keys |= 1 << k;
        }
    }
    return keys;
}

}  // namespace bt_gamepad

namespace gamepad {

bool bluetooth() {
    return true;
}

bool pair() {
    if (!task.load()) return false;
    portENTER_CRITICAL(&status_lock);
    const Pairing pairing = status_now.pairing;
    const bool busy = pairing == Pairing::starting || pairing == Pairing::searching || pairing == Pairing::connecting;
    if (!busy) {
        status_now.pairing = Pairing::starting;
        status_now.failure = Failure::none;
    }
    portEXIT_CRITICAL(&status_lock);
    if (busy) return false;
    cancelling.store(false);
    xTaskNotify(task.load(), kPairBit, eSetBits);
    return true;
}

void cancel() {
    portENTER_CRITICAL(&status_lock);
    const Pairing pairing = status_now.pairing;
    if (pairing == Pairing::paired || pairing == Pairing::failed) {
        status_now.pairing = Pairing::idle;
        status_now.failure = Failure::none;
    }
    portEXIT_CRITICAL(&status_lock);
    if (pairing == Pairing::starting || pairing == Pairing::searching || pairing == Pairing::connecting) {
        cancelling.store(true);
        if (TaskHandle_t t = task.load()) xTaskNotify(t, kCancelBit, eSetBits);
    }
}

Status status() {
    portENTER_CRITICAL(&status_lock);
    Status out = status_now;
    portEXIT_CRITICAL(&status_lock);
    out.connected = running.load(std::memory_order_relaxed) && shared()->state == BT_STATE_READY;
    return out;
}

}  // namespace gamepad
}  // namespace gbx::platform

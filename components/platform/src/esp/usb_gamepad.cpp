#include <algorithm>
#include <atomic>
#include <cstdint>

#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "gamepads.h"
#include "hid_gamepad.h"
#include "usb/usb_helpers.h"
#include "usb/usb_host.h"

// A HID gamepad on the board's USB host port presses the keys of
// keys::read(). Two tasks run on core 0, the UI's, at priority 2: above the
// GBA's renderer, below LVGL, the present task and the GB's worker, as a
// report a few milliseconds late is still in time for the app, which reads
// the keys once an update. One runs the USB Host Library. The other is its
// only client and does the rest, from opening a device and reading its
// report descriptor to reading the reports and closing the device when it
// goes, in the order the library's events come; no other task touches that
// state, which the library frees when a device goes. The library's interrupt
// is on core 0 too: start() installs it there. The stacks are in PSRAM, as
// LVGL's is: internal RAM goes to the JIT code pools, and these tasks never
// touch flash.
//
// The keys come from the hat switch, X and Y and the buttons that the report
// descriptor declares in a Game Pad or Joystick collection, see
// hid_gamepad.h. One gamepad at a time: a device that connects while another
// is open stays unused until it is plugged in again.
namespace gbx::platform::usb_gamepad {

namespace {

constexpr const char* kTag = "gamepad";
constexpr BaseType_t kCore = 0;
constexpr UBaseType_t kPriority = 2;
constexpr uint32_t kStackBytes = 4096;
// The longest report descriptor read; a longer one is parsed as far as this.
constexpr uint16_t kMaxDescriptor = 1024;

// The keys of the Button page's usages 1 to 13, in the order Sony's gamepads
// and most D-input gamepads number them: the left, bottom, right and top face
// buttons, L1, R1, L2, R2, Select (Share or Create), Start (Options), the
// stick clicks and the home button. A and B go by position, as on the PC's
// keyboard: the right face button is A, the bottom one B.
constexpr uint16_t kButtonKeys[HID_BUTTONS] = {
    0, HID_KEY_B, HID_KEY_A, 0, HID_KEY_L, HID_KEY_R, 0, 0, HID_KEY_SELECT, HID_KEY_START, 0, 0, HID_KEY_MENU};

// The keys an input report presses; false for a report of another ID.
bool decode(const hid_layout_t& layout, const uint8_t* report, size_t size, uint16_t& keys) {
    if (layout.ids) {
        if (size == 0 || report[0] != layout.report) return false;
        ++report;
        --size;
    }
    keys = hid_decode(&layout, report, size);
    return true;
}

// A HID interface of a configuration: its number, its interrupt IN
// endpoint and the length of its report descriptor.
struct Interface {
    uint8_t number = 0;
    uint8_t endpoint = 0;
    uint16_t packet = 0;  // the endpoint's wMaxPacketSize
    uint16_t report_bytes = 0;
};

// Finds the next HID interface from `offset` in a configuration descriptor
// that has an interrupt IN endpoint and a report descriptor (HID 1.11, 6.2.1);
// `offset` then points past it.
bool next_hid_interface(const usb_config_desc_t* config, int& offset, Interface& found) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(config);
    const int total = config->wTotalLength;
    bool hid = false;
    Interface candidate;
    while (offset + 2 <= total) {
        const uint8_t* d = bytes + offset;
        const uint8_t length = d[0], type = d[1];
        if (length < 2 || offset + length > total) break;
        if (type == USB_B_DESCRIPTOR_TYPE_INTERFACE && length >= 9) {
            if (hid && candidate.endpoint && candidate.report_bytes) break;
            hid = d[3] == 0 && d[5] == USB_CLASS_HID;  // alternate setting 0
            candidate = {.number = d[2]};
        } else if (hid && type == 0x21 && length >= 9 && d[6] == 0x22) {  // HID descriptor
            candidate.report_bytes = static_cast<uint16_t>(d[7] | d[8] << 8);
        } else if (hid && type == USB_B_DESCRIPTOR_TYPE_ENDPOINT && length >= 7 && (d[2] & 0x80) &&
                   (d[3] & 3) == 3 && !candidate.endpoint) {
            candidate.endpoint = d[2];
            candidate.packet = static_cast<uint16_t>((d[4] | d[5] << 8) & 0x7ff);
        }
        offset += length;
    }
    if (!hid || !candidate.endpoint || !candidate.report_bytes) return false;
    found = candidate;
    return true;
}

std::atomic<uint16_t> held{0};
std::atomic<uint16_t> pressed{0};  // went down since the last read()

void set_keys(uint16_t keys) {
    const uint16_t old = held.exchange(keys, std::memory_order_relaxed);
    if (const uint16_t down = keys & ~old) pressed.fetch_or(down, std::memory_order_relaxed);
    if (keys != old) ESP_LOGI(kTag, "keys %03x", keys);
}

// The rest belongs to the client task.
usb_host_client_handle_t client = nullptr;
usb_transfer_t* control = nullptr;  // reads report descriptors
usb_transfer_t* input = nullptr;    // reads reports, sized for the open gamepad
bool control_busy = false, input_busy = false;
bool descriptor_read = false;  // control has finished since the last advance()
uint8_t new_address = 0;       // a device that connected while none was open

// The open device.
struct Device {
    usb_device_handle_t handle = nullptr;
    int search = 0;  // where the next HID interface is looked for
    int claimed = -1;
    Interface interface;
    hid_layout_t layout{};
    bool reading = false;
    bool gone = false;
    bool done = false;  // has no gamepad
} device;

void on_control(usb_transfer_t*) {
    control_busy = false;
    descriptor_read = true;
}

void on_input(usb_transfer_t* transfer) {
    input_busy = false;
    if (device.gone || transfer->status != USB_TRANSFER_STATUS_COMPLETED) {
        // A stall or an error ends the reads; the keys go up.
        if (!device.gone && transfer->status != USB_TRANSFER_STATUS_NO_DEVICE) {
            ESP_LOGW(kTag, "report read failed (%d), the gamepad stops", transfer->status);
        }
        set_keys(0);
        return;
    }
    uint16_t keys;
    if (decode(device.layout, transfer->data_buffer, transfer->actual_num_bytes, keys)) set_keys(keys);
    input_busy = usb_host_transfer_submit(transfer) == ESP_OK;
}

void on_event(const usb_host_client_event_msg_t* event, void*) {
    if (event->event == USB_HOST_CLIENT_EVENT_NEW_DEV) {
        if (!device.handle) new_address = event->new_dev.address;
        return;
    }
    if (event->event != USB_HOST_CLIENT_EVENT_DEV_GONE || event->dev_gone.dev_hdl != device.handle) return;
    device.gone = true;
    set_keys(0);
    if (input_busy) {
        // The library has flushed the endpoint, but a read the last report
        // resubmitted may still wait on it.
        usb_host_endpoint_halt(device.handle, device.interface.endpoint);
        usb_host_endpoint_flush(device.handle, device.interface.endpoint);
    }
}

// Claims the next HID interface and asks for its report descriptor; marks
// the device done when there is none left.
void next_interface() {
    const usb_config_desc_t* config = nullptr;
    Interface found;
    while (usb_host_get_active_config_descriptor(device.handle, &config) == ESP_OK &&
           next_hid_interface(config, device.search, found)) {
        if (usb_host_interface_claim(client, device.handle, found.number, 0) != ESP_OK) continue;
        device.claimed = found.number;
        device.interface = found;
        auto* setup = reinterpret_cast<usb_setup_packet_t*>(control->data_buffer);
        setup->bmRequestType =
            USB_BM_REQUEST_TYPE_DIR_IN | USB_BM_REQUEST_TYPE_TYPE_STANDARD | USB_BM_REQUEST_TYPE_RECIP_INTERFACE;
        setup->bRequest = USB_B_REQUEST_GET_DESCRIPTOR;
        setup->wValue = 0x22 << 8;  // the report descriptor (HID 1.11, 7.1.1)
        setup->wIndex = found.number;
        setup->wLength = std::min(found.report_bytes, kMaxDescriptor);
        // The data stage takes whole packets of EP0, as the library's own
        // requests do; kMaxDescriptor is a whole number of any EP0 size.
        const usb_device_desc_t* descriptor = nullptr;
        usb_host_get_device_descriptor(device.handle, &descriptor);
        const int packet0 = descriptor ? descriptor->bMaxPacketSize0 : 64;
        control->num_bytes = USB_SETUP_PACKET_SIZE + usb_round_up_to_mps(setup->wLength, packet0);
        control->device_handle = device.handle;
        control->bEndpointAddress = 0;
        control->callback = on_control;
        if (usb_host_transfer_submit_control(client, control) == ESP_OK) {
            control_busy = true;
            return;
        }
        usb_host_interface_release(client, device.handle, found.number);
        device.claimed = -1;
    }
    device.done = true;
}

// Reads reports of the claimed interface, in transfers of one packet, as an
// IN transfer must be a whole number of them.
bool start_reading() {
    const size_t bytes = device.interface.packet;
    if (input && input->data_buffer_size < bytes) {
        usb_host_transfer_free(input);
        input = nullptr;
    }
    if (!input && usb_host_transfer_alloc(bytes, 0, &input) != ESP_OK) return false;
    input->device_handle = device.handle;
    input->bEndpointAddress = device.interface.endpoint;
    input->num_bytes = static_cast<int>(bytes);
    input->callback = on_input;
    input_busy = usb_host_transfer_submit(input) == ESP_OK;
    return input_busy;
}

void use_descriptor() {
    const int bytes = control->actual_num_bytes - USB_SETUP_PACKET_SIZE;
    device.layout = control->status == USB_TRANSFER_STATUS_COMPLETED && bytes > 0
                        ? hid::parse(control->data_buffer + USB_SETUP_PACKET_SIZE, static_cast<size_t>(bytes),
                                     kButtonKeys)
                        : hid_layout_t{};
    if (hid::has_fields(device.layout) && start_reading()) {
        device.reading = true;
        const usb_device_desc_t* descriptor = nullptr;
        usb_host_get_device_descriptor(device.handle, &descriptor);
        int buttons = 0;
        for (const hid_field_t& button : device.layout.buttons) buttons += button.bits != 0;
        ESP_LOGI(kTag, "%04x:%04x: gamepad, report %u, hat %d, stick %d, %d buttons",
                 descriptor ? descriptor->idVendor : 0, descriptor ? descriptor->idProduct : 0,
                 device.layout.report, device.layout.hat.bits != 0, device.layout.x.bits && device.layout.y.bits,
                 buttons);
        return;
    }
    usb_host_interface_release(client, device.handle, device.claimed);
    device.claimed = -1;
    next_interface();
}

// Runs after the library's callbacks, which must not release what they run on.
void advance() {
    if (!device.handle && new_address) {
        const uint8_t address = new_address;
        new_address = 0;
        if (usb_host_device_open(client, address, &device.handle) != ESP_OK) {
            device = {};
            return;
        }
        next_interface();
    }
    if (descriptor_read) {
        descriptor_read = false;
        if (!device.gone) use_descriptor();
    }
    if (device.handle && (device.gone || device.done) && !control_busy && !input_busy) {
        if (device.claimed >= 0) {
            usb_host_endpoint_clear(device.handle, device.interface.endpoint);
            usb_host_interface_release(client, device.handle, device.claimed);
        }
        usb_host_device_close(client, device.handle);
        if (device.reading) ESP_LOGI(kTag, "gamepad gone");
        device = {};
    }
}

EXT_RAM_BSS_ATTR StackType_t client_stack[kStackBytes / sizeof(StackType_t)];
EXT_RAM_BSS_ATTR StackType_t host_stack[kStackBytes / sizeof(StackType_t)];
StaticTask_t client_task, host_task;

void client_loop(void*) {
    for (;;) {
        usb_host_client_handle_events(client, portMAX_DELAY);
        advance();
    }
}

void host_loop(void*) {
    for (;;) {
        uint32_t flags = 0;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
    }
}

// Undoes start() after `err`. The root port is still off, so no device has
// connected, and only the client's deregistration has left an event, which
// usb_host_uninstall() wants taken first.
bool fail(esp_err_t err) {
    if (client) {
        usb_host_client_deregister(client);
        client = nullptr;
        uint32_t flags = 0;
        usb_host_lib_handle_events(0, &flags);
    }
    usb_host_transfer_free(control);
    control = nullptr;
    const esp_err_t undone = usb_host_uninstall();
    ESP_LOGE(kTag, "USB host: %s%s", esp_err_to_name(err),
             undone == ESP_OK ? "" : ", and the library stays installed");
    return false;
}

}  // namespace

// The app's task runs on core 0 (ESP_MAIN_TASK_AFFINITY), where the library
// then allocates its interrupt. What can fail comes before the root port
// gets power and the tasks start, so a failure undoes it all.
bool start() {
    if (xPortGetCoreID() != kCore) {
        ESP_LOGE(kTag, "USB host: started on core %d", xPortGetCoreID());
        return false;
    }
    usb_host_config_t host = {};
    host.root_port_unpowered = true;
    host.intr_flags = ESP_INTR_FLAG_LOWMED;
    esp_err_t err = usb_host_install(&host);
    if (err != ESP_OK) {
        ESP_LOGE(kTag, "USB host: %s", esp_err_to_name(err));
        return false;
    }
    usb_host_client_config_t config = {};
    config.max_num_event_msg = 4;
    config.async.client_event_callback = on_event;
    err = usb_host_transfer_alloc(USB_SETUP_PACKET_SIZE + kMaxDescriptor, 0, &control);
    if (err == ESP_OK) err = usb_host_client_register(&config, &client);
    if (err == ESP_OK) err = usb_host_lib_set_root_port_power(true);
    if (err != ESP_OK) return fail(err);
    xTaskCreateStaticPinnedToCore(client_loop, "usb-client", kStackBytes, nullptr, kPriority, client_stack,
                                  &client_task, kCore);
    xTaskCreateStaticPinnedToCore(host_loop, "usb-host", kStackBytes, nullptr, kPriority, host_stack, &host_task,
                                  kCore);
    return true;
}

uint16_t read() {
    return held.load(std::memory_order_relaxed) | pressed.exchange(0, std::memory_order_relaxed);
}

}  // namespace gbx::platform::usb_gamepad

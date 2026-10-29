#include "hci.h"

#include <string.h>

#include "hosted.h"
#include "sdio.h"

// Section numbers are of the Bluetooth Core Specification 5.4: HCI is
// Vol 4 Part E, L2CAP Vol 3 Part A, ATT Vol 3 Part F and SMP Vol 3 Part H.

#define H4_COMMAND 0x01
#define H4_ACL 0x02
#define H4_EVENT 0x04

#define OP_DISCONNECT 0x0406                 // 7.1.6
#define OP_SET_EVENT_MASK 0x0C01             // 7.3.1
#define OP_RESET 0x0C03                      // 7.3.2
#define OP_LE_SET_EVENT_MASK 0x2001          // 7.8.1
#define OP_LE_READ_BUFFER_SIZE 0x2002        // 7.8.2
#define OP_LE_CREATE_CONNECTION 0x200D       // 7.8.12
#define OP_LE_CONNECTION_UPDATE 0x2013       // 7.8.18
#define OP_LE_ENABLE_ENCRYPTION 0x2019       // 7.8.24
#define OP_LE_PARAMETER_REPLY 0x2020         // 7.8.31
#define OP_LE_ADD_TO_RESOLVING_LIST 0x2027   // 7.8.38
#define OP_LE_CLEAR_RESOLVING_LIST 0x2029    // 7.8.40
#define OP_LE_SET_RESOLUTION 0x202D          // 7.8.44
#define OP_LE_SET_PRIVACY_MODE 0x204E        // 7.8.77

#define EV_DISCONNECTION 0x05        // 7.7.5
#define EV_ENCRYPTION_CHANGE 0x08    // 7.7.8
#define EV_COMMAND_COMPLETE 0x0E     // 7.7.14
#define EV_COMMAND_STATUS 0x0F       // 7.7.15
#define EV_HARDWARE_ERROR 0x10       // 7.7.16
#define EV_COMPLETED_PACKETS 0x13    // 7.7.19
#define EV_LE_META 0x3E              // 7.7.65
#define LE_CONNECTION 0x01           // 7.7.65.1
#define LE_PARAMETER_REQUEST 0x06    // 7.7.65.6
#define LE_ENHANCED_CONNECTION 0x0A  // 7.7.65.10

#define ERR_UNKNOWN_CONNECTION 0x02
#define ERR_KEY_MISSING 0x06
#define ERR_AUTHENTICATION 0x05
#define ERR_USER_ENDED 0x13

#define CID_ATT 0x0004
#define CID_SIGNALING 0x0005
#define CID_SMP 0x0006

#define NO_HANDLE 0xFFFF

// Milliseconds.
#define COMMAND_MS 2000      // for the controller to answer a command
#define SECURING_MS 10000    // for the link to be encrypted
#define REQUEST_MS 5000      // for the gamepad to answer an ATT request
#define RETRY_MS 1000        // after the controller refused something
#define REJECTED_MS 10000    // after the gamepad refused the key
#define CHECK_MS 50          // between reads of the co-processor's length register

// Packets wait here for the controller's credits. The largest is LE Add
// Device To Resolving List.
#define QUEUE 4
#define PACKET_MAX 44

typedef struct {
    uint8_t type;
    uint8_t size;
    uint8_t data[PACKET_MAX];
} packet_t;

static packet_t queue[QUEUE];
static uint8_t queue_head, queue_count;
static uint8_t command_credits = 1;
static uint8_t acl_credits = 1;
static uint8_t acl_sent;       // ACL packets the controller has not completed

static uint32_t now;           // milliseconds, from lp_micros()
static uint32_t micros_last, micros_rest;

static uint8_t setup_step;
static uint16_t awaited;       // the command whose answer the state waits for, or 0
static uint32_t awaited_since;
static uint32_t deadline;      // for the state's step, when `timed` is set
static bool timed;
static uint32_t retry_at;      // a connection is not tried before then
static uint16_t handle = NO_HANDLE;
static bool connecting;        // LE Create Connection is pending
static uint8_t subscribed;     // CCCDs written
static bool att_waiting;       // for the answer to an ATT request
static uint32_t check_at;

static uint16_t le16(const uint8_t* p) {
    return (uint16_t)(p[0] | p[1] << 8);
}

static void put16(uint8_t* p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static bool after(uint32_t time) {
    return (int32_t)(now - time) >= 0;
}

static void set_state(uint32_t state) {
    bt_shared.state = state;
}

static uint8_t* enqueue(uint8_t type, uint8_t size) {
    if (queue_count == QUEUE || size > PACKET_MAX) return NULL;
    packet_t* p = &queue[(queue_head + queue_count++) % QUEUE];
    p->type = type;
    p->size = size;
    return p->data;
}

// Queues a command; `params` may be NULL to fill in the returned space.
static uint8_t* command(uint16_t op, const void* params, uint8_t size) {
    uint8_t* p = enqueue(H4_COMMAND, (uint8_t)(3 + size));
    if (!p) return NULL;
    put16(p, op);
    p[2] = size;
    if (params) memcpy(p + 3, params, size);
    return p + 3;
}

// A command the state waits for.
static void await(uint16_t op, const void* params, uint8_t size) {
    awaited = op;
    awaited_since = now;
    command(op, params, size);
}

static void l2cap(uint16_t cid, const void* payload, uint8_t size) {
    uint8_t* p = enqueue(H4_ACL, (uint8_t)(8 + size));
    if (!p) return;
    put16(p, handle & 0x0FFF);  // PB 00: the first packet, not flushable (5.4.2)
    put16(p + 2, (uint16_t)(4 + size));
    put16(p + 4, size);
    put16(p + 6, cid);
    memcpy(p + 8, payload, size);
}

static void disconnect(uint8_t reason) {
    uint8_t p[3];
    put16(p, handle);
    p[2] = reason;
    command(OP_DISCONNECT, p, sizeof p);
}

static void set_keys(uint16_t keys) {
    uint16_t down = keys & ~(uint16_t)bt_shared.held;
    for (int k = 0; down; ++k, down >>= 1) {
        if (down & 1) ++bt_shared.presses[k];
    }
    bt_shared.held = keys;
}

// Setup: one command at a time, each after the answer to the one before.
static void setup_next(void) {
    const bt_bond_t* bond = &bt_shared.bond;
    for (;;) {
        switch (setup_step++) {
            case 0:
                await(OP_RESET, NULL, 0);
                return;
            case 1: {
                // The default events (7.3.1), Encryption Key Refresh Complete
                // and LE Meta.
                static const uint8_t mask[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0x9F, 0x00, 0x20};
                await(OP_SET_EVENT_MASK, mask, sizeof mask);
                return;
            }
            case 2: {
                // LE Connection Complete, Connection Update Complete, Remote
                // Connection Parameter Request, and Enhanced Connection
                // Complete, which replaces the first with address resolution.
                uint8_t mask[8] = {0x25, bond->resolve ? 0x02 : 0x00};
                await(OP_LE_SET_EVENT_MASK, mask, sizeof mask);
                return;
            }
            case 3:
                await(OP_LE_READ_BUFFER_SIZE, NULL, 0);
                return;
            case 4:
                if (!bond->resolve) break;
                await(OP_LE_CLEAR_RESOLVING_LIST, NULL, 0);
                return;
            case 5: {
                if (!bond->resolve) break;
                uint8_t p[39] = {bond->addr_type};
                memcpy(p + 1, bond->addr, 6);
                memcpy(p + 7, bond->irk, 16);  // the local IRK stays 0: the LP core has no privacy of its own
                await(OP_LE_ADD_TO_RESOLVING_LIST, p, sizeof p);
                return;
            }
            case 6: {
                if (!bond->resolve) break;
                // Device privacy mode (7.8.77) also takes the identity
                // address, should the gamepad advertise with it.
                uint8_t p[8] = {bond->addr_type};
                memcpy(p + 1, bond->addr, 6);
                p[7] = 1;
                await(OP_LE_SET_PRIVACY_MODE, p, sizeof p);
                return;
            }
            case 7: {
                if (!bond->resolve) break;
                static const uint8_t on = 1;
                await(OP_LE_SET_RESOLUTION, &on, 1);
                return;
            }
            default:
                set_state(BT_STATE_WAITING);
                return;
        }
    }
}

static void restart_setup(uint32_t error) {
    bt_shared.error = error;
    // A command the controller never answered takes the credit with it.
    // The ACL credits come again with LE Read Buffer Size.
    queue_count = 0;
    command_credits = 1;
    acl_sent = 0;
    setup_step = 0;
    awaited = 0;
    handle = NO_HANDLE;
    connecting = false;
    set_keys(0);
    set_state(BT_STATE_STARTING);
    retry_at = now + RETRY_MS;
}

static void connect(void) {
    const bt_bond_t* bond = &bt_shared.bond;
    uint8_t p[25] = {0};
    put16(p, 0x0030);   // scan interval 30 ms
    put16(p + 2, 0x0030);  // window 30 ms: scan all the time
    p[4] = 0;           // the peer address below, not the filter accept list
    p[5] = bond->resolve ? bond->addr_type + 2 : bond->addr_type;  // identity types are 2 and 3
    memcpy(p + 6, bond->addr, 6);
    p[12] = 0;          // own public address
    put16(p + 13, 6);   // connection interval 7.5 to 15 ms
    put16(p + 15, 12);
    put16(p + 17, 0);   // latency
    put16(p + 19, 200); // supervision timeout 2 s
    await(OP_LE_CREATE_CONNECTION, p, sizeof p);
    connecting = true;
}

static void subscribe_next(void) {
    const bt_bond_t* bond = &bt_shared.bond;
    if (subscribed >= bond->input_count) {
        att_waiting = false;
        timed = false;
        bt_shared.error = BT_ERROR_NONE;
        set_state(BT_STATE_READY);
        return;
    }
    // Write Request (3.4.5.1) of 0x0001 to the CCCD: notifications on. A
    // bonded gamepad may keep it from the last connection, but the write
    // does not depend on that.
    uint8_t p[5] = {0x12};
    put16(p + 1, bond->inputs[subscribed++].cccd);
    put16(p + 3, 0x0001);
    l2cap(CID_ATT, p, sizeof p);
    att_waiting = true;
    timed = true;
    deadline = now + REQUEST_MS;
}

static void on_connected(uint8_t status, uint16_t link) {
    connecting = false;
    if (status) {
        retry_at = now + RETRY_MS;
        return;
    }
    handle = link;
    subscribed = 0;
    set_state(BT_STATE_SECURING);
    timed = true;
    deadline = now + SECURING_MS;
    const bt_bond_t* bond = &bt_shared.bond;
    uint8_t p[28];
    put16(p, handle);
    for (int i = 0; i < 8; ++i) p[2 + i] = (uint8_t)(bond->rand >> 8 * i);
    put16(p + 10, bond->ediv);
    memcpy(p + 12, bond->ltk, 16);
    command(OP_LE_ENABLE_ENCRYPTION, p, sizeof p);
}

// The controller has dropped the link's ACL packets, and its credits are
// back without Number Of Completed Packets (4.3). Those still queued are
// dropped here.
static void drop_acl(void) {
    acl_credits = (uint8_t)(acl_credits + acl_sent);
    acl_sent = 0;
    uint8_t kept = 0;
    for (uint8_t i = 0; i < queue_count; ++i) {
        const packet_t* p = &queue[(queue_head + i) % QUEUE];
        if (p->type == H4_ACL) continue;
        if (kept != i) queue[(queue_head + kept) % QUEUE] = *p;
        ++kept;
    }
    queue_count = kept;
}

static void on_disconnected(void) {
    drop_acl();
    handle = NO_HANDLE;
    att_waiting = false;
    timed = false;
    set_keys(0);
    if (bt_shared.state != BT_STATE_STOPPED) {
        set_state(BT_STATE_WAITING);
        if (!after(retry_at)) return;  // the key was refused
        retry_at = now + 100;
    }
}

static void on_command_done(uint16_t op, uint8_t status, const uint8_t* ret, uint8_t size) {
    if (op == OP_LE_READ_BUFFER_SIZE && status == 0 && size >= 3) {
        acl_credits = ret[2] ? ret[2] : 1;
    }
    if (op != awaited) {
        if (op == OP_LE_ENABLE_ENCRYPTION && status) disconnect(ERR_AUTHENTICATION);
        return;
    }
    awaited = 0;
    if (op == OP_LE_CREATE_CONNECTION) {
        if (status) {
            connecting = false;
            retry_at = now + RETRY_MS;
        }
        return;
    }
    // Setup. A controller without privacy mode (before 5.0) still resolves.
    if (status && op != OP_LE_SET_PRIVACY_MODE) {
        restart_setup(BT_ERROR_COMMAND);
        return;
    }
    setup_next();
}

static void receive_event(const uint8_t* e, uint16_t size) {
    if (size < 2 || size < 2u + e[1]) return;
    const uint8_t code = e[0];
    const uint8_t* p = e + 2;
    const uint8_t n = e[1];
    switch (code) {
        case EV_COMMAND_COMPLETE:
            if (n < 4) return;
            command_credits = p[0] ? 1 : 0;
            on_command_done(le16(p + 1), p[3], p + 4, (uint8_t)(n - 4));
            return;
        case EV_COMMAND_STATUS: {
            if (n < 4) return;
            command_credits = p[1] ? 1 : 0;
            // A pending LE Create Connection ends with LE Connection
            // Complete; the controller has only taken it.
            const uint16_t op = le16(p + 2);
            if (op == OP_LE_CREATE_CONNECTION && p[0] == 0) {
                if (op == awaited) awaited = 0;
                return;
            }
            on_command_done(op, p[0], NULL, 0);
            return;
        }
        case EV_COMPLETED_PACKETS:
            // Only the link's: a count for one that has ended is already
            // back.
            for (uint8_t i = 0; i < p[0] && 1u + 4u * (i + 1u) <= n; ++i) {
                if ((le16(p + 1 + 4 * i) & 0x0FFF) != handle) continue;
                uint16_t count = le16(p + 3 + 4 * i);
                if (count > acl_sent) count = acl_sent;
                acl_credits = (uint8_t)(acl_credits + count);
                acl_sent = (uint8_t)(acl_sent - count);
            }
            return;
        case EV_DISCONNECTION:
            if (n >= 4 && p[0] == 0 && le16(p + 1) == handle) on_disconnected();
            return;
        case EV_ENCRYPTION_CHANGE:
            if (n < 4 || le16(p + 1) != handle || bt_shared.state != BT_STATE_SECURING) return;
            if (p[0] == 0 && p[3]) {
                set_state(BT_STATE_SUBSCRIBING);
                subscribe_next();
                return;
            }
            // Key missing: the gamepad has dropped the bond, as after
            // pairing with another host. Try again much later.
            if (p[0] == ERR_KEY_MISSING) {
                bt_shared.error = BT_ERROR_KEY;
                retry_at = now + REJECTED_MS;
            }
            disconnect(ERR_AUTHENTICATION);
            return;
        case EV_HARDWARE_ERROR:
            restart_setup(BT_ERROR_COMMAND);
            return;
        case EV_LE_META:
            if (n < 1) return;
            if ((p[0] == LE_CONNECTION || p[0] == LE_ENHANCED_CONNECTION) && n >= 4) {
                on_connected(p[1], le16(p + 2));
            } else if (p[0] == LE_PARAMETER_REQUEST && n >= 11 && le16(p + 1) == handle) {
                // Accept the gamepad's connection parameters as they are.
                uint8_t* r = command(OP_LE_PARAMETER_REPLY, NULL, 14);
                if (r) {
                    memcpy(r, p + 1, 10);
                    memset(r + 10, 0, 4);
                }
            }
            return;
        default:
            return;
    }
}

static void report(uint16_t value, const uint8_t* data, uint16_t size) {
    const bt_bond_t* bond = &bt_shared.bond;
    for (uint8_t i = 0; i < bond->input_count; ++i) {
        if (bond->inputs[i].value != value) continue;
        if (bond->layout.ids && bond->inputs[i].report_id != bond->layout.report) return;
        set_keys(hid_decode(&bond->layout, data, size));
        return;
    }
}

static void att_error(uint8_t request, uint16_t attribute, uint8_t code) {
    uint8_t p[5] = {0x01, request};  // Error Response (3.4.1.1)
    put16(p + 2, attribute);
    p[4] = code;
    l2cap(CID_ATT, p, sizeof p);
}

// The gamepad's ATT requests to the LP core, which has no attributes, and
// its answers and notifications.
static void receive_att(const uint8_t* p, uint16_t size) {
    if (size < 1) return;
    const uint8_t op = p[0];
    const uint16_t attribute = size >= 3 ? le16(p + 1) : 0;
    switch (op) {
        case 0x1B:  // Handle Value Notification (3.4.7.1)
            if (size >= 3) report(attribute, p + 3, (uint16_t)(size - 3));
            return;
        case 0x1D: {  // Handle Value Indication: confirm it (3.4.7.3)
            static const uint8_t confirmation = 0x1E;
            l2cap(CID_ATT, &confirmation, 1);
            if (size >= 3) report(attribute, p + 3, (uint16_t)(size - 3));
            return;
        }
        case 0x13:  // Write Response
        case 0x01:  // Error Response: subscribe to the rest anyway
            if (att_waiting && bt_shared.state == BT_STATE_SUBSCRIBING) subscribe_next();
            return;
        case 0x02: {  // Exchange MTU Request: keep the default 23 (3.4.2.2)
            static const uint8_t mtu[3] = {0x03, 23, 0};
            l2cap(CID_ATT, mtu, sizeof mtu);
            return;
        }
        case 0x04:  // Find Information
        case 0x06:  // Find By Type Value
        case 0x08:  // Read By Type
        case 0x10:  // Read By Group Type
            att_error(op, attribute, 0x0A);  // Attribute Not Found
            return;
        case 0x0A:  // Read
        case 0x0C:  // Read Blob
        case 0x12:  // Write Request
        case 0x16:  // Prepare Write Request
            att_error(op, attribute, 0x01);  // Invalid Handle
            return;
        case 0x0E:  // Read Multiple
        case 0x18:  // Execute Write
        case 0x20:  // Read Multiple Variable Length
            att_error(op, 0, 0x06);  // Request Not Supported
            return;
        default:
            // Commands, such as Write Command, need no answer.
            return;
    }
}

static void receive_signaling(const uint8_t* p, uint16_t size) {
    if (size < 4) return;
    const uint8_t code = p[0], id = p[1];
    if (code == 0x12 && size >= 12) {
        // Connection Parameter Update Request (4.20): accept, then ask the
        // controller for the parameters.
        const uint8_t response[6] = {0x13, id, 2, 0, 0, 0};
        l2cap(CID_SIGNALING, response, sizeof response);
        uint8_t* c = command(OP_LE_CONNECTION_UPDATE, NULL, 14);
        if (c) {
            put16(c, handle);
            memcpy(c + 2, p + 4, 8);
            memset(c + 10, 0, 4);
        }
        return;
    }
    if (code == 0x14 && size >= 6) {
        // LE Credit Based Connection Request (4.22): no protocol is there.
        uint8_t response[14] = {0x15, id, 10, 0};
        response[12] = 0x02;  // SPSM not supported
        l2cap(CID_SIGNALING, response, sizeof response);
        return;
    }
    // Command Reject (4.1) for other requests; responses and indications
    // need nothing.
    if (code == 0x01 || code == 0x13 || code == 0x15 || code == 0x16 || code == 0x18 || code == 0x07) return;
    const uint8_t reject[6] = {0x01, id, 2, 0, 0, 0};
    l2cap(CID_SIGNALING, reject, sizeof reject);
}

static void receive_smp(const uint8_t* p, uint16_t size) {
    if (size < 1) return;
    // Pairing belongs to the HP core. A Security Request (3.6.7) needs no
    // answer: the LP core encrypts every connection anyway.
    if (p[0] == 0x01) {
        static const uint8_t failed[2] = {0x05, 0x05};  // Pairing Failed: not supported
        l2cap(CID_SMP, failed, sizeof failed);
    }
}

static void receive_acl(const uint8_t* p, uint16_t size) {
    if (size < 8 || (le16(p) & 0x0FFF) != handle) return;
    // A continuing fragment (PB 01) or an L2CAP packet longer than this
    // one: nothing the gamepad sends is that long.
    if ((le16(p) >> 12 & 3) == 1) return;
    const uint16_t length = le16(p + 2), payload = le16(p + 4), cid = le16(p + 6);
    if (4u + length > size || 4u + payload > length) return;
    const uint8_t* body = p + 8;
    if (cid == CID_ATT) receive_att(body, payload);
    else if (cid == CID_SIGNALING) receive_signaling(body, payload);
    else if (cid == CID_SMP) receive_smp(body, payload);
}

void hci_receive(const uint8_t* packet, uint16_t size) {
    if (size < 1) return;
    if (packet[0] == H4_EVENT) receive_event(packet + 1, (uint16_t)(size - 1));
    else if (packet[0] == H4_ACL) receive_acl(packet + 1, (uint16_t)(size - 1));
}

void hci_start(void) {
    set_state(BT_STATE_STARTING);
    setup_step = 0;
    setup_next();
}

static void flush(void) {
    while (queue_count) {
        packet_t* p = &queue[queue_head];
        if (p->type == H4_COMMAND && !command_credits) return;
        if (p->type == H4_ACL && !acl_credits) return;
        if (!hosted_send(p->type, p->data, p->size)) return;
        if (p->type == H4_COMMAND) {
            command_credits = 0;
        } else {
            --acl_credits;
            ++acl_sent;
        }
        queue_head = (uint8_t)((queue_head + 1) % QUEUE);
        --queue_count;
    }
}

bool hci_run(void) {
    const uint32_t micros = lp_micros();
    const uint32_t elapsed = micros_rest + (micros - micros_last);
    micros_last = micros;
    now += elapsed / 1000;
    micros_rest = elapsed % 1000;

    const int polled = hosted_poll(hci_receive, after(check_at));
    if (after(check_at)) check_at = now + CHECK_MS;
    if (polled < 0 || hosted_bus_failed()) {
        bt_shared.error = BT_ERROR_BUS;
        set_keys(0);
        set_state(BT_STATE_FAILED);
        return false;
    }

    // A stop leaves the link up: the HP core resets the co-processor next,
    // which ends it without a Disconnect. An Xbox Wireless Controller turns
    // off when the central ends the link as Remote User Terminated
    // Connection, and the gamepad is to reconnect.
    if (bt_shared.stop) {
        set_keys(0);
        set_state(BT_STATE_STOPPED);
        return false;
    }
    const uint32_t state = bt_shared.state;
    if (awaited && after(awaited_since + COMMAND_MS)) {
        restart_setup(BT_ERROR_TIMEOUT);
    } else if (state == BT_STATE_STARTING && !awaited && setup_step == 0 && after(retry_at)) {
        setup_next();
    } else if (state == BT_STATE_WAITING && handle == NO_HANDLE && !connecting && !awaited && after(retry_at)) {
        connect();
    } else if (timed && after(deadline) && handle != NO_HANDLE) {
        // Encryption or the gamepad's answer took too long.
        timed = false;
        disconnect(ERR_USER_ENDED);
    }
    flush();
    return true;
}

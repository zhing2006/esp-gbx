// The Bluetooth gamepad's runtime on the LP core, see bt_gamepad.cpp for
// what the HP core does before it starts this. It polls the co-processor
// every POLL_US, handles what comes and writes the keys to bt_shared until
// the HP core asks it to stop, then returns, which halts the LP core.
#include "hci.h"
#include "hosted.h"
#include "sdio.h"
#include "ulp_lp_core_utils.h"

#define POLL_US 250

bt_shared_t bt_shared;

int main(void) {
    if (bt_shared.version != BT_SHARED_VERSION || !hosted_attach()) {
        bt_shared.error = BT_ERROR_BUS;
        bt_shared.state = BT_STATE_FAILED;
        return 0;
    }
    hci_start();
    while (hci_run()) ulp_lp_core_delay_us(POLL_US);
    return 0;
}

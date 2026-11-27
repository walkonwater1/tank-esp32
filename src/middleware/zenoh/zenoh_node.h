#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Start the Zenoh-pico task.  It waits for Wi-Fi, connects to
// CONFIG_ZENOH_CONNECT_ENDPOINT, and exposes tank/cmd_vel,
// tank/battery_state and tank/status.
void ZenohStart(void);

#ifdef __cplusplus
}
#endif

#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Start the micro-ROS UDP task.  The task waits for Wi-Fi, then connects to
// the agent configured by CONFIG_MICRO_ROS_AGENT_IP / PORT.
void MicrorosStart(void);

#ifdef __cplusplus
}
#endif

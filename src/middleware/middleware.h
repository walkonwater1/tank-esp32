#pragma once

#ifdef __cplusplus
extern "C" {
#endif

// Start the middleware backend selected by CONFIG_ROBOT_MIDDLEWARE_*.
void RobotMiddlewareStart(void);

// Compile-time backend identity, reported to TSP telemetry.
const char *TankMiddlewareBackend(void);  // "microros" / "zenoh" / "none"
const char *TankMiddlewareLabel(void);    // "micro-ROS" / "Zenoh" / "未启用"
const char *TankMiddlewareLibrary(void);  // e.g. "zenoh-pico 1.9.0"

#ifdef __cplusplus
}
#endif

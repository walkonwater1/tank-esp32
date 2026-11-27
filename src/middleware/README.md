# middleware

设备与外部中间件 / 通信协议之间的接入层，不承载具体业务功能。

- `microros/`：micro-ROS（XRCE-DDS）节点，通过 Wi-Fi UDP 连接 ROS 2 agent，
  暴露 `/cmd_vel`、`/battery_state`、`/tank/status`。
- `zenoh/`：zenoh-pico 节点，通过 TCP 连接 Zenoh router，暴露
  `tank/cmd_vel`、`tank/battery_state`、`tank/status`。
- `tsp/`：独立的新元 TSP MQTT 客户端（遥测、命令回执、OTA 下发）。
- `protocols/`：小智云端的 MQTT / WebSocket 协议栈。
- `middleware.c`：按 `CONFIG_ROBOT_MIDDLEWARE_*` 选择 ROS 2 后端启动。

和现有目录的边界：

| 目录 | 职责 |
|---|---|
| `middleware/` | ROS 2 / TSP / 云端协议等对外通信 |
| `service/` | 设备内服务，如 MCP、OTA、settings、system |
| `device/` | 音频、显示、运动、LED 等硬件抽象 |

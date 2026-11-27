# middleware

设备与外部中间件之间的桥接层，不承载具体业务功能。

- `microros/`：micro-ROS（XRCE-DDS）节点，通过 Wi-Fi UDP 连接 ROS 2 agent，
  暴露 `/cmd_vel`、`/battery_state`、`/tank/status`。
- `zenoh/`：zenoh-pico 节点，通过 TCP 连接 Zenoh router，暴露
  `tank/cmd_vel`、`tank/battery_state`、`tank/status`。
- `middleware.c`：按 `CONFIG_ROBOT_MIDDLEWARE_*` 选择一个后端启动。

和现有目录的边界：

| 目录 | 职责 |
|---|---|
| `middleware/` | ROS 2 / micro-ROS 等外部中间件接入 |
| `protocols/` | 应用层协议，如小智 MQTT、WebSocket |
| `service/` | 设备内服务，如 MCP、OTA、settings、system |
| `device/` | 音频、显示、LED 等硬件抽象 |

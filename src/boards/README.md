# 板级入口

`dev` 分支只维护当前小车使用的 `yuyi-a2` 板型。当前只有这一种板型，这里只放板级入口和配置：

- `yuyi-a2.cc`：板级初始化、外设组装。
- `config.h` / `config.json`：引脚、外设参数和构建名。
- `common/`：Wi-Fi、按键、系统复位等共享基础实现，不代表额外板型。

功能实现按职责放在各自目录：

| 位置 | 内容 |
|---|---|
| `src/device/display/` | OLED / emoji / robo eyes |
| `src/device/motion/` | 电机、运动队列、坦克控制、电机自检 |
| `src/middleware/tsp/` | TSP MQTT 遥测与命令 |
| `src/middleware/protocols/` | 小智云端 MQTT / WebSocket 协议 |

其他上游开发板适配已从此分支移除。如需重新适配其他硬件，应从上游工程单独建立分支，不要在当前小车板型上复用不同的 GPIO 配置。

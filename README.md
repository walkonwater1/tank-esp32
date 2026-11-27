# tank-esp32

宇意 A2 V1 双履带小车的 **ESP32-S3 固件**，基于小智（xiaozhi）2.0.3。

- MCU：ESP32-S3，16 MB Flash，8 MB Octal PSRAM
- 显示：SSD1306 128×64 OLED
- 音频：ES8311 + ES7210，语音唤醒与小智云端连接
- 电源：LGS4056 充电管理，电压 / 电量 / 充电 / 充满状态上报
- 底盘：双履带 PWM，6 个 MCP 运动工具
- 联网：Wi-Fi + 新元 TSP MQTT（`esp32-robot/nex_001`）

## 当前能力

- OLED 表情动画，底部单行显示最新 ASR 文字，长句滚动、10 秒后隐藏。
- TSP 遥测：电池、Wi-Fi、MCP 工具清单、CPU、内部 RAM、PSRAM、Flash、音量、固件版本、OTA 状态。
- TSP 下行命令：前进、后退、左转、右转、停止、跳舞、音量查询/设置、上报周期、`upgrade_firmware`。
- A/B OTA：写入非运行分区，失败可回滚；TSP 平台下发 app 镜像 URL。
- 上电自走默认关闭，只有显式定义 `TANK_ENABLE_POWER_ON_DEMO` 才启用演示动作。

## 目录

```text
tank-esp32/
├── src/
│   ├── main.cc
│   ├── app/          # application / device state
│   ├── assets/       # 提示音、语言资源
│   ├── boards/       # 当前板级实现（平铺）+ common/
│   ├── device/       # audio / display / led
│   ├── middleware/   # micro-ROS / ROS 2 接入
│   ├── protocols/    # MQTT / WebSocket 协议
│   └── service/      # mcp / ota / settings / system
├── config/           # sdkconfig、sdkconfig.defaults*、partitions/
├── docs/
│   ├── firmware/     # 板级改动、接线、编译烧录、验证
│   ├── flashing/     # 厂商烧录资料 + backups/ 本地固件备份
│   ├── hardware/     # AI 主板原理图、3D 打印结构件
│   └── upstream/     # 上游小智 README
└── scripts/          # 资源生成、打包、发布脚本
```

## 编译与烧录

```bash
. $IDF_PATH/export.sh

# 首次拉取：Zenoh 后端使用 git submodule
git submodule update --init --recursive

idf.py set-target esp32s3
idf.py build

# 只更新 app 分区，保留配网 / 资源 / NVS
idf.py -p /dev/ttyACM0 app-flash monitor
```

`CMakeLists.txt` 固定使用 `yuyi-a2` 板型，构建配置读取 `config/sdkconfig`。

## 本地中间件（micro-ROS / Zenoh）

固件在 `middleware/` 下提供两个互斥后端，用 `menuconfig` 的
`Robot middleware → Local middleware backend` 选择，编译时只链接其中一个：

| 后端 | 传输 | 命令入口 | 遥测出口 |
|---|---|---|---|
| micro-ROS | Wi-Fi UDP，agent 默认 `8888` | `/cmd_vel` | `/battery_state`、`/tank/status` |
| Zenoh | zenoh-pico TCP，router 默认 `7447` | `tank/cmd_vel` | `tank/battery_state`、`tank/status` |

micro-ROS agent：

```bash
docker run -it --rm --net=host microros/micro-ros-agent:humble udp4 --port 8888 -v6
```

Zenoh router：

```bash
zenohd
```

两端都复用板载 Wi-Fi，不重复初始化网络。`cmd_vel` 超时 300 ms 自动停车；断网或
对端消失时不会持续运动。详细配置和对比方法见 [docs/middleware.md](docs/middleware.md)。

## 文档

- [板级改动与验证](docs/firmware/README.md)
- [烧录资料](docs/flashing/README.md)
- [硬件资料](docs/hardware/README.md)
- [上游小智说明](docs/upstream/README.md)
- [构建配置说明](config/README.md)
- [middleware 后端与对比方法](docs/middleware.md)
- [串口控制协议](https://github.com/walkonwater1/tank-ros2/blob/dev/docs/protocol.md)
- [整机路线](https://github.com/walkonwater1/tank-ros2/blob/dev/docs/roadmap.md)

ROS 2 工作区、整机文档和跨仓库任务卡在另一个仓库：
[walkonwater1/tank-ros2](https://github.com/walkonwater1/tank-ros2)。

## 许可证

上游小智项目与本仓库厂商分支均为 MIT。本仓库自有代码的许可证待定。

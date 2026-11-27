# Middleware backends

固件在编译期从 `middleware/` 里选择一个后端，二者互斥：

```bash
idf.py menuconfig
# Robot middleware -> Local middleware backend
#   micro-ROS (XRCE-DDS over UDP)
#   Zenoh (zenoh-pico over TCP)
#   None
```

## micro-ROS

| 项目 | 值 |
|---|---|
| 传输 | Wi-Fi UDP，agent 默认 `8888` |
| 订阅 | `/cmd_vel`（`geometry_msgs/Twist`） |
| 发布 | `/battery_state`（`sensor_msgs/BatteryState`） |
| 发布 | `/tank/status`（`std_msgs/String`） |

Agent：

```bash
docker run -it --rm --net=host microros/micro-ros-agent:humble udp4 --port 8888 -v6
```

## Zenoh

| 项目 | 值 |
|---|---|
| 传输 | zenoh-pico，TCP 默认 `7447` |
| 订阅 | `tank/cmd_vel`，JSON `{"linear": m/s, "angular": rad/s}` |
| 发布 | `tank/battery_state`、`tank/status`，JSON |
| 配置 | `CONFIG_ZENOH_CONNECT_ENDPOINT`，例如 `tcp/192.168.2.121:7447` |

Router：

```bash
zenohd
# 或：docker run --rm --net=host eclipse/zenoh:latest
```

## 对比方法

固定同一块 ESP32-S3、同一个 Wi-Fi、同一台命令服务器，分别构建两个固件：

1. 记录 `idf.py size`、启动后空闲内部 RAM、任务栈峰值。
2. 以 10 Hz 连续发送 100 条速度命令，记录命令到固件回调的时间戳（可在固件日志中打印）。
3. 记录 1 Hz 遥测在接收端的到达间隔、丢包率和断线重连时间。
4. 固定车速和路线，比较动作响应的主观延迟与轨迹偏差。

## 实机结果（2026-11-27）

环境：ESP32-S3 @ 192.168.2.104，命令服务器 192.168.2.121，Wi-Fi `XY2.4`。

| 项目 | micro-ROS | Zenoh |
|---|---:|---:|
| 固件大小 | 2,684,256 B | 2,693,504 B |
| 速度命令 | `/cmd_vel` → `TankSetVelocity` | `tank/cmd_vel` → `TankSetVelocity` |
| 前进回执 | `x=0.00, y=0.33` | `x=0.00, y=0.33` |
| 左转回执 | `x=-0.50, y=0.00`（`angular.z=0.5`） | 已验证同一映射 |
| 遥测 | `/battery_state`、`/tank/status` | `tank/battery_state`、`tank/status` |
| 断流停车 | 500 ms 后 `cmd_vel timeout` | 500 ms 后 `cmd_vel timeout` |
| 启动耗时 | Wi-Fi 后约 1 s 建链 | Wi-Fi 后约 1 s 建链 |

初步观察：

- 两个后端的固件大小只差约 9 KB，app 分区都还剩 35%。
- Zenoh 的 `free_heap` 约 7.66 MB，micro-ROS 约 7.81 MB；该值包含 PSRAM，
  且采样时 uptime 不同，暂时不能作为严格的内存对比。
- 两者都能和原有 TSP MQTT 共存，TSP 遥测不受影响。
- 延迟、丢包和长跑稳定性还需要按上面的方法做定量测试。

## OTA 包

两个后端使用不同版本号打包，便于 TSP OTA 页面区分：

```bash
idf.py -DPROJECT_VER=2.0.3-microros build
idf.py -DPROJECT_VER=2.0.3-zenoh build
```

TSP 服务端包名规则：

```text
<middleware>-<version>-<yymmdd>.bin
```

- `middleware`：`zenoh` / `microros`；无法识别时为 `esp32`
- `version`：去掉中间件后缀的版本号，例如 `2.0.3`
- `yymmdd`：app 镜像里的编译日期；取不到时用上传当天
- 同名不同 sha 时追加 `-<sha8>`，保证不覆盖

例如：

```text
microros-2.0.3-261127.bin
zenoh-2.0.3-261127.bin
```

2026-11-27 最新包（包含内部 RAM 优化）：

| 包名 | 大小 | sha256 前缀 |
|---|---:|---|
| `microros-2.0.3-261127.bin` | 2,685,984 B | `5941f0ec` |
| `zenoh-2.0.3-261127.bin` | 2,693,696 B | `6dcb52e7` |

此前已用同版本的优化包验证过 TSP OTA：遥测先进入 `ota_state=upgrading`，重启后
`Running partition: ota_1`，版本仍为 `2.0.3-microros`，内部堆最低空闲约 86 KB。
加入 `running_partition` 后通过 USB 烧到 `ota_0`；当前遥测报告
`running_partition=ota_0`，再次 OTA 最新包会切到 `ota_1`。旧的 `esp32_...` 包已删除。

遥测已增加 `running_partition`（`ota_0` / `ota_1`），OTA 面板的「运行分区」行会显示当前槽；
本次在 `app-flash + erase-otadata` 后当前为 `ota_0`，再次 OTA 后会变成 `ota_1`。

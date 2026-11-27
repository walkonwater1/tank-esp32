/*
 * SPDX-FileCopyrightText: 2023-2024 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

// 初始化坦克运动控制系统
void TankControlInit(void);

// 发送前进命令（3秒后自动停止）
bool TankMoveForward(void);
bool TankMoveForwardFor(int duration_ms);

// 发送后退命令（3秒后自动停止）
bool TankMoveBackward(void);
bool TankMoveBackwardFor(int duration_ms);

// 发送左转命令（3秒后自动停止）
bool TankTurnLeft(void);
bool TankTurnLeftFor(int duration_ms);

// 发送右转命令（3秒后自动停止）
bool TankTurnRight(void);
bool TankTurnRightFor(int duration_ms);

// 连续速度命令（ROS 2 /cmd_vel）：linear 单位 m/s，angular 单位 rad/s。
// 命令 300 ms 后自动失效，调用方应以约 10 Hz 持续刷新。
bool TankSetVelocity(float linear_mps, float angular_radps);

// 发送停止命令
bool TankStop(void);

// 发送舞蹈命令
void TankDance(void);

#ifdef __cplusplus
}
#endif

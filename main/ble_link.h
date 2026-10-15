/*
 * 蓝牙 BLE 遥控（NimBLE，外设角色，不加密）
 *
 * 服务 UUID      9e5d0001-3c1a-4b8e-9f2d-850e0a1c0001
 *   命令 ...0002  Write：      [op u8][seq u8][value u16 LE]
 *                              op 1 = 设定占空比（value = 千分比，0 = 停止）
 *                              op 2 = 定时（value = 分钟，0 = 取消）
 *                              op 3 = 停止
 *   状态 ...0003  Read/Notify：20 字节，格式见 remote.c pack_status()
 *
 * 线程模型：NimBLE 主机任务中收到的命令通过 remote 模块的命令队列交给主循环执行。
 * 安全：未加密、未配对，附近任何人都能连接并控制红外输出。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#define BLE_LINK_STATUS_LEN 20

// 初始化 NimBLE 并注册服务；sync 后开始广播。name 为广播名
esp_err_t ble_link_start(const char *name);

bool ble_link_running(void);
bool ble_link_connected(void);

// 打开/关闭广播（已连接时忽略）
void ble_link_set_adv(bool on);

// 发布状态：更新 Read 值，已订阅时发送 Notify
void ble_link_publish(const uint8_t status[BLE_LINK_STATUS_LEN]);

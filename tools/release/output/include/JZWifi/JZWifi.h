#ifndef JZ_WIFI_H_
#define JZ_WIFI_H_

// 原 JZWifi 统一管理类已拆分为多个独立角色类:
//   - JZWifiStation           : 作为站点连接外部 AP (连接)
//   - JZWifiAp                : 本机作为接入点对外提供服务 (做服务)
//   - JZWifiScanner           : 扫描周围 WiFi, 返回 ssid 与信号强度
//   - JZWifiAutoConnectService: 定期扫描并自动连接信号最强的候选网络
// 底层驱动接口与事件转发集中在 JZWifiDriver。本头文件仅作聚合包含, 方便迁移。
#include "JZWifiStation.h"
#include "JZWifiAp.h"
#include "JZWifiScanner.h"
#include "JZWifiAutoConnectService.h"

#endif // !JZ_WIFI_H_

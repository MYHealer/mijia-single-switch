/*
 * miot_config.h — 配置子系统（BLE-only：仅 NVS 初始化）
 *
 * WiFi / 米家凭据子系统已随 BLE 方案切换整体下线（凭据不入库、不编译）。
 * 保留 pmj_config_init：store.c / fcnt_store.c 的 NVS blob 依赖它完成 nvs_flash_init。
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 初始化 NVS（含无可用页时自动擦除重建）。返回 ESP_OK。 */
esp_err_t pmj_config_init(void);

#ifdef __cplusplus
}
#endif

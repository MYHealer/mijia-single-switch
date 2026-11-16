/*
 * wifi_mgr.h — WiFi STA 连接管理
 */
#pragma once

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 连接 WiFi（STA）。
 *   ssid     : 目标 SSID（可为 NULL=用 Kconfig/NVS 默认）
 *   pass     : 密码（可为 NULL=用 Kconfig/NVS 默认）
 *   timeout_s: 等待连接+拿到 IP 的超时秒数
 * 成功返回 ESP_OK，且已持有 IP；失败返回错误码。
 */
esp_err_t wifi_mgr_connect(const char *ssid, const char *pass, int timeout_s);

/* 断开连接（可选调用） */
esp_err_t wifi_mgr_disconnect(void);

/* 是否已联网（拿到 IP） */
bool wifi_mgr_is_connected(void);

/*
 * 诊断扫描：查找指定 SSID，打印其 authmode/信道/RSSI 到日志。
 * 用于排查「找到 AP 但 auth 失败」。
 */
void wifi_mgr_diag_scan_ssid(const char *ssid);

/* 从 Kconfig/默认值构造默认 ssid/pass（可被 NVS 覆盖）。会复制到 out 缓冲区。 */
void wifi_mgr_defaults(char *ssid, size_t ssid_sz, char *pass, size_t pass_sz);

#ifdef __cplusplus
}
#endif
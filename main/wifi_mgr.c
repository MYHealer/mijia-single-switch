/*
 * wifi_mgr.c — WiFi STA 连接管理（事件驱动 + 信号量）
 *
 * 设计：
 *   - 只做 STA。
 *   - 用事件回调把「GOT_IP」/「断开」翻译为二值信号量，供阻塞式 connect 用。
 *   - SSID/密码缺省从 Kconfig 读，可先由调用方经 NVS 覆盖。
 */
#include "wifi_mgr.h"

#include <string.h>
#include <time.h>

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_sntp.h"
#include "esp_log.h"
#include "esp_check.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"

#include "miot_config.h"   /* WiFi Kconfig 宏 */

static const char *TAG = "wifi";

/* WiFi 断连 reason → 可读描述（esp_wifi_types_generic.h 枚举） */
static const char *_wifi_reason_str(int r)
{
    switch (r) {
    case 1:  return "NO_AP_FOUND";
    case 2:  return "AUTH_EXPIRE";
    case 3:  return "AUTH_LEAVE";
    case 4:  return "ASSOC_EXPIRE";
    case 5:  return "ASSOC_TOOMANY";
    case 6:  return "NOT_AUTHED";
    case 7:  return "NOT_ASSOCED";
    case 8:  return "ASSOC_LEAVE";
    case 9:  return "ASSOC_NOT_AUTHED";
    case 10: return "DISASSOC_PWRCAP_BAD";
    case 15: return "4WAY_HANDSHAKE_TIMEOUT";
    case 16: return "GROUP_KEY_UPDATE_TIMEOUT";
    case 17: return "IE_IN_4WAY_DIFFERS";
    case 18: return "GROUP_CIPHER_INVALID";
    case 19: return "PAIRWISE_CIPHER_INVALID";
    case 20: return "AKMP_INVALID";
    case 23: return "MIC_FAILURE";
    case 31: return "AP_PS_POLL_TIMEOUT";
    case 34: return "MODALNONE";
    case 37: return "HANDSHAKE_TIMEOUT";
    case 200: return "WRONG_PASSWORD_LEGACY";
    case 201: return "NO_AP_FOUND_LEGACY";
    case 202: return "CONN_FAIL_LEGACY";
    case 203: return "HANDSHAKE_TIMEOUT_LEGACY";
    default:  return "OTHER";
    }
}

#define WIFI_READY_BIT BIT0   /* 拿到 IP */
#define WIFI_FAIL_BIT  BIT1   /* 连接失败 */

static EventGroupHandle_t s_evt;
static bool s_sntp_started = false;
static int s_fail_cnt = 0;   /* 连续断连计数，退避用 */

/* 幂等 WiFi 栈初始化（netif + event loop + wifi 驱动），只跑一次 */
static esp_err_t wifi_mgr_ensure_inited(void)
{
    static bool s_init_done = false;
    if (s_init_done) return ESP_OK;
    ESP_RETURN_ON_ERROR(esp_netif_init(), "wifi", "netif init failed");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), "wifi",
                        "event loop failed");
    s_init_done = true;
    (void)esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), "wifi", "wifi init failed");
    return ESP_OK;
}

/* ---- 事件回调 ---- */
static void wifi_event_handler(void *arg, esp_event_base_t base,
                               int32_t id, void *data)
{
    if (base == WIFI_EVENT) {
        switch (id) {
        case WIFI_EVENT_STA_START:
            esp_wifi_connect();
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            /* 断连：记录 reason 便于诊断 */
            wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
            ESP_LOGW(TAG, "STA disconnected, reason=%d (%s) rssi_prev=%d",
                     d ? d->reason : -1,
                     d ? _wifi_reason_str(d->reason) : "?",
                     d ? d->rssi : -1);
            /* 退避重连，避免无退避重连把驱动卡死在 auth（记忆教训）。 */
            s_fail_cnt++;
            if (s_fail_cnt >= 8) {
                ESP_LOGW(TAG, "连续 %d 次断连，暂停重连 10s", s_fail_cnt);
                vTaskDelay(pdMS_TO_TICKS(10000));
            } else {
                vTaskDelay(pdMS_TO_TICKS(1500));
            }
            esp_wifi_connect();
            break;
        }
        case WIFI_EVENT_STA_CONNECTED:
            ESP_LOGI(TAG, "STA connected");
            s_fail_cnt = 0;
            break;
        default:
            break;
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&e->ip_info.ip));
        if (s_evt) {
            xEventGroupSetBits(s_evt, WIFI_READY_BIT);
        }
    }
}

/* ---- 诊断扫描 ---- */
static const char *_authmode_str(wifi_auth_mode_t m)
{
    switch (m) {
    case WIFI_AUTH_OPEN:            return "OPEN";
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA_PSK";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2_PSK";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA_WPA2_PSK";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2_ENTERPRISE";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3_PSK";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2_WPA3_PSK";
    case WIFI_AUTH_WAPI_PSK:        return "WAPI_PSK";
    case WIFI_AUTH_OWE:             return "OWE";
#ifdef WIFI_AUTH_WPA3_ENT_192
    case WIFI_AUTH_WPA3_ENT_192:    return "WPA3_ENT_192";
#endif
    default: return "?";
    }
}

void wifi_mgr_diag_scan_ssid(const char *target)
{
    /* 自包含扫描：init → STA 模式无 config start → 扫描 → stop。
       STA 不带网络配置启动时不会自动 connect，scan 不受阻。 */
    if (wifi_mgr_ensure_inited() != ESP_OK) goto out;
    esp_wifi_set_mode(WIFI_MODE_STA);
    /* 清空 config，避免自动 connect 干扰扫描 */
    wifi_config_t blank = {0};
    esp_wifi_set_config(WIFI_IF_STA, &blank);
    esp_err_t e = esp_wifi_start();
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "diag scan: start fail %s", esp_err_to_name(e));
        goto out;
    }
    vTaskDelay(pdMS_TO_TICKS(500));  /* 等驱动稳定 */

    wifi_scan_config_t sc = {0};
    sc.show_hidden = true;
    e = esp_wifi_scan_start(&sc, true);  /* 阻塞 */
    if (e != ESP_OK) {
        ESP_LOGW(TAG, "diag scan: scan_start %s", esp_err_to_name(e));
        goto stop;
    }

    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n == 0) {
        ESP_LOGI(TAG, "diag scan: no APs found");
        goto stop;
    }
    uint16_t got = n;
    wifi_ap_record_t *rec = calloc(n, sizeof(wifi_ap_record_t));
    if (!rec) { ESP_LOGE(TAG, "scan: no mem"); goto stop; }
    esp_wifi_scan_get_ap_records(&got, rec);

    ESP_LOGI(TAG, "diag scan: found %u APs:", (unsigned)got);
    for (int i = 0; i < got; i++) {
        bool match = target && rec[i].ssid[0] &&
                     strcmp((char *)rec[i].ssid, target) == 0;
        if (match || got <= 10) {
            ESP_LOGI(TAG, "  AP[%d] '%s' ch=%d rssi=%d auth=%s%s",
                     i, rec[i].ssid, rec[i].primary,
                     rec[i].rssi, _authmode_str(rec[i].authmode),
                     match ? "  <== TARGET" : "");
        }
    }
    free(rec);

stop:
    esp_wifi_stop();
out:
    return;
}

/* ---- 默认值 ----
 * BLE-only：Kconfig WiFi 默认已随凭据子系统删除，这里返回空串。
 * 回退启用 WiFi 直连时由调用方显式传 ssid/pass（空则 connect 报 empty SSID）。 */
void wifi_mgr_defaults(char *ssid, size_t ssid_sz, char *pass, size_t pass_sz)
{
    snprintf(ssid, ssid_sz, "%s", "");
    snprintf(pass, pass_sz, "%s", "");
}

esp_err_t wifi_mgr_connect(const char *ssid, const char *pass, int timeout_s)
{
    char def_ssid[33] = {0};
    char def_pass[65] = {0};

    if (!ssid || !*ssid) {
        wifi_mgr_defaults(def_ssid, sizeof(def_ssid), def_pass, sizeof(def_pass));
        ssid = def_ssid;
        pass = pass && *pass ? pass : (def_pass[0] ? def_pass : NULL);
    }
    if (!ssid || !*ssid) {
        ESP_LOGE(TAG, "empty SSID");
        return ESP_ERR_INVALID_ARG;
    }

    if (!s_evt) {
        s_evt = xEventGroupCreate();
    }
    xEventGroupClearBits(s_evt, WIFI_READY_BIT | WIFI_FAIL_BIT);

    esp_err_t ierr = wifi_mgr_ensure_inited();
    if (ierr != ESP_OK) return ierr;

    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                               &wifi_event_handler, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                               &wifi_event_handler, NULL);

    wifi_config_t wc = {0};
    strncpy((char *)wc.sta.ssid, ssid, sizeof(wc.sta.ssid) - 1);
    if (pass) {
        strncpy((char *)wc.sta.password, pass, sizeof(wc.sta.password) - 1);
    }
    /* 不设 threshold.authmode：设为 WPA2_PSK 会拒绝 WPA3/SAE 只开的路由。
       默认(OPEN)表示接受任何 auth 模式，兼容家用 WPA2/WPA3 混合。 */
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    /* 强制 802.11 b/g/n：规避部分 AX(He) 路由 auth 帧协商 bug */
    ESP_ERROR_CHECK(esp_wifi_set_protocol(WIFI_IF_STA,
                        WIFI_PROTOCOL_11B | WIFI_PROTOCOL_11G | WIFI_PROTOCOL_11N));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* ★ C3 SuperMini 板级降频（记忆惯例）：
       - PCB 天线大功率易致回波/谐波 → AP 收错帧不回 auth → 降功率到 8.5dBm (34*0.25)
       - 强制 20MHz 频宽，避免 40MHz 扩展信道协商卡死 */
    esp_wifi_set_max_tx_power(34);
    esp_wifi_set_bandwidth(WIFI_IF_STA, WIFI_BW_HT20);
    ESP_LOGI(TAG, "C3 SuperMini: TX power=8.5dBm, BW=HT20 已降频");

    ESP_LOGI(TAG, "connecting to '%s'...", ssid);
    EventBits_t bits = xEventGroupWaitBits(s_evt,
                                           WIFI_READY_BIT | WIFI_FAIL_BIT,
                                           pdFALSE, pdFALSE,
                                           pdMS_TO_TICKS(timeout_s * 1000));
    if (bits & WIFI_READY_BIT) {
        /* 连上 → 尝试 NTP 同步墙钟（MIoT nonce 需要 epoch） */
        if (!s_sntp_started) {
            esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
            esp_sntp_setservername(0, "pool.ntp.org");
            esp_sntp_init();
            s_sntp_started = true;
        }
        int waited = 0;
        while (waited < 30) {
            if (sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
                time_t now = time(NULL);
                ESP_LOGI(TAG, "SNTP 同步完成, epoch=%lld", (long long)now);
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(1000));
            waited++;
        }
        if (waited >= 30) {
            ESP_LOGW(TAG, "SNTP 30s 内未完成; 若时间未同步, MIoT 可能拒绝 nonce");
        }
        return ESP_OK;
    }
    if (bits & WIFI_FAIL_BIT) {
        return ESP_ERR_WIFI_NOT_CONNECT;
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t wifi_mgr_disconnect(void)
{
    esp_wifi_disconnect();
    return ESP_OK;
}

bool wifi_mgr_is_connected(void)
{
    if (!s_evt) return false;
    return (xEventGroupGetBits(s_evt) & WIFI_READY_BIT) != 0;
}
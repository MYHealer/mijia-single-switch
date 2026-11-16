/*
 * main.c — pressure-mijia：压力传感器按压 → 台灯亮/灭（跟随）
 *
 * 流程：
 *   启动 → 配置初始化(NVS) → WiFi STA 连接 → FSR 采样循环
 *     ├─ DO 数字阈值（模块比较器）或 AO 模拟滞回阈值 判定按压
 *     └─ 翻转时下发台灯 on/off（/miotspec/prop/set → prop/get 闭环）
 *
 * 引脚：FSR D0=GPIO1(数字), A0=GPIO2(ADC1_CH2), VCC=3V3, GND=GND
 */
#include <stdio.h>
#include <string.h>
#include <stdbool.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_event.h"
#include "esp_timer.h"
#include "nvs_flash.h"

#include "app_config.h"
#include "fsr.h"
#include "wifi_mgr.h"
#include "miot_config.h"
#include "miot_client.h"
#include "ble_switch.h"
#include "driver/gpio.h"   /* gpio_get_level（fsr 日志） */

static const char *TAG = "pmj";

/* fsr_task 更新的当前压力态（BLE 事件的触发源，WiFi/BLE 两方案共用） */
static volatile bool s_pressure_now = false;

/* =====================================================================
 * ⛔ WiFi 直连米家控灯链路 —— 已注释停用（2026-11-07）
 * 决策：切换为 BLE 方案（C3 伪装米家无线开关 XMWXKG01CM，
 *       按压发 0x4E0C 事件广播 → 米家网关触发自动化开灯）。
 * 恢复方法：删除本块的 #if 0 / #endif 与 app_main 内相应注释即可，
 *          回到已实测通过的「WiFi 直连 set code=0」基线。
 * ===================================================================== */
#if 0
/* ---- 米家台灯控制（在专用任务里跑，避免阻塞采样） ---- */
static TaskHandle_t s_light_task = NULL;

/* fsr_task 更新的当前压力态；light_task 轮询检测翻转 */
static volatile bool s_last_sent_power = false;
static volatile bool s_last_sent_valid = false;

/* 下发台灯 power 状态到米家。最快路径：set 一次，server code==0 即成功。
   去掉 prop_get 闭环等待 → 「马上亮 / 松手马上灭」。 */
static bool miot_set_power(const miot_auth_t *auth, const char *did,
                          bool target, const char *tag)
{
    for (int attempt = 0; attempt <= MIOT_OPEN_LIGHT_RETRY; attempt++) {
        miot_prop_result_t r;
        esp_err_t err = miot_prop_set(auth, did, 2, 1, target, &r);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "%s prop_set 网络失败: %s",
                     tag, esp_err_to_name(err));
            vTaskDelay(pdMS_TO_TICKS(600));
            continue;
        }
        if (r.code == 0) {
            ESP_LOGI(TAG, "%s → 台灯%s 成功 (code=0)", tag,
                     target ? "亮" : "灭");
            return true;
        }
        ESP_LOGW(TAG, "%s prop_set server code=%d，重试", tag, r.code);
        vTaskDelay(pdMS_TO_TICKS(600));
    }
    ESP_LOGE(TAG, "%s 失败（重试耗尽）", tag);
    return false;
}

/* light 任务：轮询压力态，翻转时下发台灯 on/off */
static void light_task_impl(void *arg)
{
    (void)arg;
    for (;;) {
        /* 装配凭据（一次，成功后缓存供循环用；失败重试） */
        miot_auth_t auth;
        bool have = false;
        pmj_miot_get(auth.serviceToken, sizeof(auth.serviceToken),
                     auth.ssecurity, sizeof(auth.ssecurity),
                     auth.userId, sizeof(auth.userId),
                     auth.cUserId, sizeof(auth.cUserId), &have);
        if (!have) {
            ESP_LOGE(TAG, "米家凭据缺失：请运行 extract_credentials.ps1 或 NVS 写入");
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        if (!auth.userId[0] || strcmp(auth.userId, "0") == 0) {
            ESP_LOGE(TAG, "userId 无效: '%s'", auth.userId);
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }
        char did[MIOT_DID_MAX] = "313344343";   /* TestLight 台灯 */

        /* 初始：跟随当前压力态下发一次，建立基线 */
        s_last_sent_power = s_pressure_now;
        s_last_sent_valid = true;
        ESP_LOGI(TAG, "台灯初始跟随压力态：power=%d",
                 s_pressure_now ? 1 : 0);
        miot_set_power(&auth, did, s_pressure_now, "init");

        for (;;) {
            bool cur = s_pressure_now;
            if (!s_last_sent_valid || cur != s_last_sent_power) {
                s_last_sent_power = cur;
                s_last_sent_valid = true;
                bool ok = miot_set_power(&auth, did, cur,
                                         cur ? "press" : "release");
                ESP_LOGI(TAG, "状态跟随：压力=%s → 台灯%s %s",
                         cur ? "有" : "无", cur ? "亮" : "灭",
                         ok ? "✔" : "✖");
            }
            vTaskDelay(pdMS_TO_TICKS(20));   /* 20ms 轮询，翻转检测近实时 */
        }
    }
}
#endif /* WiFi 直连控灯链路停用 */

/* ---- FSR 采样 + 按压检测 ---- */
static void fsr_task(void *arg)
{
    (void)arg;
    esp_err_t err = fsr_init(FSR_DO_PRESSED_LEVEL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "FSR 初始化失败: %s", esp_err_to_name(err));
        vTaskDelay(pdMS_TO_TICKS(5000));
        return;
    }
    ESP_LOGI(TAG, "FSR 初始化完成 (D0=GPIO1 数字, A0=GPIO2/ADC1_CH2)");

    int64_t last_log_ms = 0;
    bool prev_press = false;
    for (;;) {
        bool press = fsr_pressed(FSR_AO_ON_ADC, FSR_AO_OFF_ADC);
        s_pressure_now = press;
        int64_t now = esp_timer_get_time() / 1000;

        /* 按下沿立即触发单击（最初版本，用户实测米家绑定+事件都成功）。
         * 松开沿判定会因 FSR 按住不放/DO 抖动导致「按下没反应」或重复触发。 */
        if (press && !prev_press) {
            ble_switch_button_press();
        }
        prev_press = press;

        /* 状态日志：每 5s 打一次 AO 原始值，方便调阈值 */
        if (now - last_log_ms >= 5000) {
            last_log_ms = now;
            int a = fsr_analog_raw();
            int do_lvl = gpio_get_level(FSR_DO_GPIO);
            ESP_LOGI(TAG, "fsr: DO=%d AO=%d press=%d (on_adc=%d off_adc=%d)",
                     do_lvl, a, press ? 1 : 0,
                     FSR_AO_ON_ADC, FSR_AO_OFF_ADC);
        }

        vTaskDelay(pdMS_TO_TICKS(SAMPL_MS));
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "pressure-mijia 启动（WiFi 直连已停用 → BLE 方案迁移中）");
    // miot_client_init();   /* ⛔ WiFi 直连停用 */

    /* 配置子系统（NVS） */
    esp_err_t err = pmj_config_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS init failed: %s", esp_err_to_name(err));
        return;
    }

    /* ---- ⛔ WiFi 段已注释停用（切换 BLE 方案） ----
    char ssid[33], pass[65];
    pmj_wifi_get(ssid, sizeof(ssid), pass, sizeof(pass));
    ESP_LOGI(TAG, "目标 WiFi '%s'，先诊断扫描", ssid);
    wifi_mgr_diag_scan_ssid(ssid);      // 自包含扫描：打印 AP authmode/信道
    ESP_LOGI(TAG, "连接 WiFi '%s'...", ssid);
    err = wifi_mgr_connect(ssid, pass, 30);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "WiFi 连接失败: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "WiFi 就绪");
    }
    */

    /* ---- ⛔ WiFi 直连灯光任务已注释停用 ----
    // 栈 16KB：TLS 握手耗栈大（曾 8KB 触发栈保护 panic）
    xTaskCreatePinnedToCore(light_task_impl, "light", 16384, NULL, 1, &s_light_task, 0);
    */

    /* FSR 采样任务（保留：将来驱动 BLE 事件） */
    xTaskCreatePinnedToCore(fsr_task, "fsr", 4096, NULL, 2, NULL, 0);

    /* ---- ⛔ WiFi 自测块已注释停用 ----
#if CONFIG_PMJ_SELFTEST_AUTO_LIGHT_ON_BOOT
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "自测：5s 后模拟按压 → 台灯应 亮→灭");
        vTaskDelay(pdMS_TO_TICKS(5000));
        s_pressure_now = true;
        vTaskDelay(pdMS_TO_TICKS(3000));
        s_pressure_now = false;
    }
#endif
    */
    (void)err;

    /* BLE：米家无线开关（XMWXKG01CM）可连接广播 —— 米家扫描可见 */
    esp_err_t berr = ble_switch_start();
    if (berr != ESP_OK) {
        ESP_LOGE(TAG, "BLE start failed: %s", esp_err_to_name(berr));
    }
}
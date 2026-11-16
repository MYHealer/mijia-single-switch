/*
 * miot_config.c — 配置子系统（BLE-only：仅 NVS 初始化）
 *
 * 历史说明：本文件曾承载 WiFi SSID/密码与米家 serviceToken/ssecurity 的
 * 「NVS > Kconfig > credentials_auto.h」三级读取。切换 BLE 方案后整套下线：
 * Kconfig.projbuild 已删、credentials_auto.h 已删且 gitignore，
 * 真实凭据只存在于本机 NVS（不随仓库传播）。
 */
#include "miot_config.h"

#include "nvs_flash.h"

esp_err_t pmj_config_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        err = nvs_flash_init();
    }
    return err;
}

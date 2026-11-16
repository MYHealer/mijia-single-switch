/*
 * fsr.c — AO/DO 压感模块驱动（GPIO 数字 + ADC 模拟）
 *
 * 引脚（app_config.h）：
 *   A0 → GPIO2 = ADC1_CH2
 *   D0 → GPIO1
 *
 * 判定策略：
 *   - DO 数字：电平 == 按压电平 → 按压（模块比较器已定阈值，最可靠）
 *   - AO 模拟：ADC 值带滞回（ON 进入阈 / OFF 释放阈）
 *   - 融合：DO 或 AO 任一成立即按压；若 DO 未接/不稳可只靠 AO。
 */
#include "fsr.h"

#include <string.h>

#include "esp_log.h"
#include "esp_adc/adc_oneshot.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "app_config.h"

static const char *TAG = "fsr";

/* DO 按压电平（极性可配） */
static uint8_t s_do_level = 0;
static bool s_inited = false;

/* ADC 句柄/通道 */
static adc_oneshot_unit_handle_t s_adc = NULL;
static bool s_adc_built = false;

/* ---- 初始化 ---- */
esp_err_t fsr_init(uint8_t do_level)
{
    if (s_inited)
        return ESP_ERR_INVALID_STATE;

    s_do_level = FSR_DO_PRESSED_LEVEL;
    if (do_level) s_do_level = do_level;

    /* D0：普通输入（无上下拉，模块输出或推或泄） */
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << FSR_DO_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t err = gpio_config(&io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "gpio DO config fail: %s", esp_err_to_name(err));
        return err;
    }

    /* A0：ADC1 单次采样 */
    adc_oneshot_unit_init_cfg_t uinit = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    err = adc_oneshot_new_unit(&uinit, &s_adc);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc unit fail: %s", esp_err_to_name(err));
        return err;
    }
    adc_oneshot_chan_cfg_t ccfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    err = adc_oneshot_config_channel(s_adc, ADC_CHANNEL_2, &ccfg);  /* GPIO2 */
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "adc chan fail: %s", esp_err_to_name(err));
        adc_oneshot_del_unit(s_adc);
        return err;
    }
    s_adc_built = true;
    s_inited = true;

    ESP_LOGI(TAG, "FSR init: DO=GPIO%d lvl=%d  AO=GPIO%d(ADC1_CH2) 12bit/12dB",
             (int)FSR_DO_GPIO, s_do_level, (int)FSR_AO_GPIO);

    /* 首组读数丢弃（ADC 上电稳定） */
    for (int i = 0; i < 8; i++) {
        int v = 0;
        if (adc_oneshot_read(s_adc, ADC_CHANNEL_2, &v) == ESP_OK) {
            /* 预热 */
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return ESP_OK;
}

bool fsr_digital_pressed(void)
{
    int lvl = gpio_get_level(FSR_DO_GPIO);
    return (lvl == (int)s_do_level);
}

int fsr_analog_raw(void)
{
    if (!s_adc_built)
        return 0;
    int v = 0;
    if (adc_oneshot_read(s_adc, ADC_CHANNEL_2, &v) != ESP_OK)
        return 0;
    if (v < 0) v = 0;
    return v;
}

/* 中位数平滑（FSR_AO_SMOOTH_N 个样本） */
static int analog_median(int n)
{
    if (n < 1) n = 1;
    if (n > 9) n = 9;
    int buf[9];
    for (int i = 0; i < n; i++) {
        buf[i] = fsr_analog_raw();
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    for (int i = 0; i < n - 1; i++)
        for (int j = i + 1; j < n; j++)
            if (buf[j] < buf[i]) {
                int t = buf[i];
                buf[i] = buf[j];
                buf[j] = t;
            }
    return buf[n / 2];
}

/*
 * 融合判定（滞回，有状态）：
 *   - DO 电平 == 按压电平 → 按压
 *   - AO：进入按压需 a<=on_adc；已按压则需 a 回升 > off_adc 才释放
 * 返回 true = 当前按压。
 */
static bool s_last_press = false;   /* 上一刻状态，滞回用 */

bool fsr_pressed(int on_adc, int off_adc)
{
    /* DO 主判据（模块比较器已定阈值，最可靠） */
    bool do_p = fsr_digital_pressed();

    /* AO 兜底：滞回 */
    int a = analog_median(FSR_AO_SMOOTH_N);

    if (do_p) {
        s_last_press = true;
        return true;
    }
    if (s_last_press) {
        /* 已按压：需 AO 明显回升才释放 */
        if (a >= off_adc) {
            s_last_press = false;
            return false;
        }
        return true;   /* 回升不足，维持按压 */
    } else {
        /* 未按压：AO 明显下降才进入 */
        if (a <= on_adc) {
            s_last_press = true;
            return true;
        }
        return false;
    }
}

void fsr_deinit(void)
{
    if (s_adc_built) {
        adc_oneshot_del_unit(s_adc);
        s_adc = NULL;
        s_adc_built = false;
    }
    s_inited = false;
}
/*
 * fsr.h — AO/DO 压感模块驱动接口
 *
 * 模块：FSR/薄膜压力模块（4 脚：D0 / A0 / VCC / GND）
 *   D0 数字比较器输出 → 普通 GPIO 读电平
 *   A0 模拟分压输出    → C3 ADC 读 0~4095
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "driver/gpio.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * 初始化 FSR：配 DO 为输入、AO 为 ADC1 通道。
 *   do_level: 「按压」时 DO 的电平值（0/1），不传则用 app_config 默认。
 * 成功返回 ESP_OK。
 */
esp_err_t fsr_init(uint8_t do_level);

/* 读取 DO 数字状态：true=按压（电平匹配「按压电平」），false=无压力 */
bool fsr_digital_pressed(void);

/* 读取 AO 模拟原始 ADC 值（0~4095），取中位数平滑。 */
int fsr_analog_raw(void);

/*
 * 融合判定（滞回，有状态）：DO 电平 或 AO 降到 on_adc 以下判按压；
 * 已按压后需 AO 回升 > off_adc 才释放。
 *   on_adc：按压进入 AO 阈值（值小=压大）
 *   off_adc：释放 AO 阈值（回升超过才释放）
 * 返回 true = 按压。
 */
bool fsr_pressed(int on_adc, int off_adc);

/* 释放驱动（注销 ADC） */
void fsr_deinit(void);

#ifdef __cplusplus
}
#endif
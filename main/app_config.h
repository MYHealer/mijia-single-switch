/*
 * app_config.h — pressure-mijia 硬件平台配置
 *
 * 板型：ESP32-C3 SuperMini
 * 压力传感器：AO/DO 型压感模块（FSR/薄膜压力模块，带比较器，4 脚）
 *   - 1 = D0  数字输出（模块内比较器定阈值，按压翻转电平）
 *   - 2 = A0  模拟输出（随压力变化的分压电压）
 *   - 3 = VCC（3.3V 或 5V）
 *   - 4 = GND
 *
 * 语义：按压 → DO 翻转 或 AO 电压升高 → 台灯亮；无压力 → 台灯灭。
 */
#pragma once

/* ---- 模块引脚 ----
 * A0 → GPIO2  (ADC1_CH2)：模拟读取压力强度
 * D0 → GPIO1  (普通输入)：数字阈值判定（更稳）
 * VCC → 3V3（或模块要求 5V 则外接）
 * GND → GND（共地）
 */
#define FSR_AO_GPIO   GPIO_NUM_2   /* A0 模拟，ADC1_CH2 */
#define FSR_DO_GPIO   GPIO_NUM_1   /* D0 数字 */

/* ---- DO 判定 ---- */
/* DO 极性依模块而异。常见：无压力 = 高电平，按下 = 低电平。
   若拿反，把下值颠倒即可。 */
#define FSR_DO_PRESSED_LEVEL  0   /* 判为「按压」的 DO 电平（0 或 1） */

/* ---- AO 判定（模拟阈值）----
 * 特性：空载 AO 接近满量程（实测约 4095），按压后 FSR 电阻降低 → AO 电压下降。
 * 所以「按压」= AO 降到阈值以下。
 *   进入：AO <= FSR_AO_ON_ADC     （值越小，压越大）
 *   释放：AO 回升 >= FSR_AO_OFF_ADC（滞回，防临界抖动）
 * 默认经验值：空载 4095，手指按压往往降到 2500 以下。可按实测调。
 */
#define FSR_AO_ON_ADC      2500     /* 按压进入：AO 须降到此值（含）以下 */
#define FSR_AO_OFF_ADC      3000    /* 释放判定：AO 回升到此值以上 */

/* ---- 采样/判定参数 ---- */
#define SAMPL_MS            50      /* 采样周期（20Hz） */

/* ---- 去抖 ---- */
#define PRESS_DEBOUNCE_MS    150     /* 触发后去抖封锁 */
#define FSR_AO_SMOOTH_N      4       /* AO 平滑中位数窗口 */

/* ---- 开灯动作 ---- */
#define MIOT_OPEN_LIGHT_RETRY  2     /* 每次状态翻转最多补发几次 */
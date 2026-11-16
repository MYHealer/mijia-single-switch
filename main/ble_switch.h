/**
 * @file ble_switch.h
 * @brief 米家无线开关（蓝牙版 XMWXKG01CM）BLE 层 —— 阶段一：可连接广播（扫描可见）
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * 启动 NimBLE host、GATT 服务（FE95 四特征）+ auth 状态机 + hello 广播。
 * 幂等保护无（只应调一次）。返回后 host 异步初始化，sync 回调里开广播。
 */
esp_err_t ble_switch_start(void);

/**
 * FSR 单击钩子（米家绑定流程「请按一下设备」）：
 * 未绑定态 = 开唤醒窗口 + 刷新广播（frame_cnt 跳变，等价真机按键触发广播）；
 * 已绑定态 = 发 0x4E0C 单击事件加密广播（2.5s 快广播）→ 网关 → 米家自动化。
 * 任意任务上下文可调。
 */
void ble_switch_button_press(void);

/**
 * FSR 长按钩子：已绑定态发 0x4E0E 长按事件加密广播；未绑定态同单击。
 * 任意任务上下文可调。
 */
void ble_switch_button_hold(void);

#ifdef __cplusplus
}
#endif
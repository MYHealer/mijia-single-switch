/*
 * miot_client.h — 米家 MIoT HTTP（api.mijia.tech /app/miclaw）加密客户端
 *
 * 移植自 02-工具脚本/miot_client.py（asar postEncrypted 反推）。
 *   host       api.mijia.tech
 *   prefix     /app/miclaw
 *   sid        mijia
 *   algorithm  ENCRYPT-RC4
 *   UA         MiClaw/1.0
 *
 * 核心端点：
 *   POST /miotspec/prop/set   设属性（开灯）
 *   POST /miotspec/prop/get   读属性（闭环））
 *   POST /miotspec/action     设备动作
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define MIOT_DID_MAX     24
#define MIOT_TOKEN_MAX   512
#define MIOT_SSEC_MAX    128
#define MIOT_UID_MAX     48
#define MIOT_CUID_MAX    64

typedef struct {
    char serviceToken[MIOT_TOKEN_MAX];
    char ssecurity[MIOT_SSEC_MAX];
    char userId[MIOT_UID_MAX];
    char cUserId[MIOT_CUID_MAX];
} miot_auth_t;

/*
 * 单设备属性操作结果。
 */
typedef struct {
    bool   ok;                /* code==0 或 -7(no data) 等可接受态 */
    int    code;
    char   msg[64];
    int64_t update_time;       /* 可选 */
    int64_t value_int;         /* get 数值型值（bool → 0/1） */
    bool   value_bool;
    bool   has_value;
} miot_prop_result_t;

/*
 * 初始化加密上下文（无状态状态机）。幂等，可重复调用。
 */
void miot_client_init(void);

/*
 * 设置设备属性（/miotspec/prop/set）。
 *   did/siid/piid/value：标准 MIoT 属性定位。value 传字符串，会自动按 JSON 化。
 * 成功返回 ESP_OK 且在 r 里给出 server code。
 */
esp_err_t miot_prop_set(const miot_auth_t *auth,
                        const char *did, int siid, int piid,
                        bool value, miot_prop_result_t *r);

/*
 * 查询设备属性（/miotspec/prop/get）。
 */
esp_err_t miot_prop_get(const miot_auth_t *auth,
                        const char *did, int siid, int piid,
                        miot_prop_result_t *r);

#ifdef __cplusplus
}
#endif
/**
 * @file store.h
 * @brief 绑定身份 NVS 持久化（AGENTS.md S3.1）
 */
#pragma once

#include <stdbool.h>
#include "miot_ble/miot_auth.h"

#ifdef __cplusplus
extern "C" {
#endif

void store_save(void *user, const miot_auth_identity_t *id);
bool store_load(void *user, miot_auth_identity_t *id);

#ifdef __cplusplus
}
#endif
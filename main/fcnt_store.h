/**
 * @file fcnt_store.h
 * @brief frame_cnt 掉电持久化（防加密广播被网关判重放丢弃）
 */
#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 读回上次 frame_cnt；无记录时返回 1（起点偏移在调用方加）。 */
uint32_t fcnt_load(void);
void fcnt_save(uint32_t v);

#ifdef __cplusplus
}
#endif

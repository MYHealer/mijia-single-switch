/**
 * @file fcnt_store.c
 * @brief frame_cnt 掉电持久化 —— 修「烧录后单击不上云」的根因。
 *
 * 绑定后加密广播帧的 nonce 含 frame_cnt，网关按「见过的最大 cnt」做防重放。
 * 每次烧录 hard reset 让 cnt 回到 1，低于网关水位 → 所有事件帧被判重放丢弃，
 * 表现为「串口在发 0x4E0C，米家设备日志/自动化毫无反应」。
 *
 * ① 启动时从 NVS 读回，保证跨重启单调；
 * ② 无记录或低于 FLOOR 时从 FLOOR 起跳，一次越过历史遗留的高水位
 *    （本项目实测设备曾连续跑到 cnt≈1280，之后每次烧录归 1）。
 */
#include "fcnt_store.h"
#include "nvs.h"

/* 高于任何历史水位；后续由 NVS 持久化，不再回跳 */
#define FCNT_FLOOR  100000u

static const char *NS = "pmjbtn", *KEY = "fcnt";

uint32_t fcnt_load(void)
{
    nvs_handle_t h;
    uint32_t v = 0;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return FCNT_FLOOR;
    if (nvs_get_u32(h, KEY, &v) != ESP_OK) v = 0;
    nvs_close(h);
    if (v < FCNT_FLOOR) v = FCNT_FLOOR;
    return v;
}

void fcnt_save(uint32_t v)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u32(h, KEY, v);
    nvs_commit(h);
    nvs_close(h);
}

/**
 * @file store.c
 * @brief 绑定身份 NVS 持久化（AGENTS.md S3.1 原样）
 *        否则每次重启都要重新绑定。
 */
#include "store.h"

#include "nvs.h"

/* ★命名空间必须独立（上游 btn1_adv README 踩坑 4）：WiFi 直连链路与
 * multi_dev 等其他工程用 "miot"/"id"，复用会读到别的设备的 did/bind_key，
 * 米家会把这台「开关」记到别人名下。整片擦除后本工程独占 pmjbtn。 */
static const char *NS = "pmjbtn", *KEY = "ident";

void store_save(void *user, const miot_auth_identity_t *id)
{
    (void)user;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, KEY, id, sizeof(*id));
    nvs_commit(h);
    nvs_close(h);
}

bool store_load(void *user, miot_auth_identity_t *id)
{
    (void)user;
    nvs_handle_t h;
    size_t len = sizeof(*id);
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    bool ok = (nvs_get_blob(h, KEY, id, &len) == ESP_OK &&
               len == sizeof(*id));
    nvs_close(h);
    return ok;
}
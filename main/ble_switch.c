/**
 * @file ble_switch.c
 * @brief 米家无线开关（蓝牙版 XMWXKG01CM / PID 0x2387）BLE 层
 *
 * 修复版（2026-11-15，移植上游 esp32/btn1_adv 实测金样）：
 *   上游 README「踩过的坑」第 2 条：未绑定 hello 帧必须带 solicited 位
 *   （FC 高字节 0x2A）。缺位时米家 App 点添加后永远不发起连接，30s 报
 *   「连接超时」，设备端一个 CONNECT 事件都没有 —— 正是本项目「扫到但
 *   连不上、GAP CONNECT=0」的根因。AGENTS.md 通用手册的 30 58 是温湿度计
 *   形态，对按键类设备不适用。
 *
 * 合入的 8 项金样修复：
 *   1. hello 帧 FC 高字节 0x2A（v2 | registered=0 | authMode=2 | solicited=1）
 *   2. adv 显式带 16-bit Service UUID 列表 0xFE95（Android ScanFilter 匹配）
 *   3. scan rsp 带 0x038F V2 信标帧（PID/MAC/0x20 未注册）+ 设备名
 *   4. 帧内 MAC 用 LSB 序（nonce = mac||pid||cnt||counter_ext）
 *   5. 0x0010 写入按认证命令白名单分流：短命令→auth，ag2 数据帧→spec
 *   6. spec notify 出口改 0x0010（App 只订阅 0010/0019，不订阅 001A）
 *   7. 连接期间停广播、断开才恢复（diag 不再无条件刷新）
 *   8. ble_svc_gatt_init() + 首选 MTU 247
 *
 * 阶段三（绑定后）预留：用 identity.bind_key 作 beacon_key 发 0x4E0C
 * 单击事件帧（frame_v5_nomac）→ 网关 → 米家自动化「单击→台灯」。
 */
#include "ble_switch.h"

#include <string.h>

#include "esp_log.h"
#include "esp_mac.h"                /* esp_read_mac（MAC LSB 序） */
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_att.h"
#include "host/util/util.h"
#include "os/os_mbuf.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

#include "miot_ble/miot_beacon.h"
#include "miot_ble/miot_auth.h"
#include "miot_ble/miot_device.h"
#include "miot_ble/miot_profiles.h"
#include "store.h"
#include "fcnt_store.h"

extern void miot_port_esp32_crypto_init(void);

static const char *TAG = "blesw";

#define BLE_SW_PID   0x2387                 /* XMWXKG01CM（pdid 9095） */
#define BLE_SW_NAME  "XMWXKG01CM"
#define ADV_PERIOD_MS   3000               /* 心跳刷新周期 */
#define WAKE_WINDOW_MS  120000             /* 单击后唤醒窗口（solicited 有效期） */
#define REPEAT_MS       2500               /* 事件帧重复广播时长（网关轮询去重用） */
#define V_BATT          87                 /* 常态电量心跳 % */

static uint8_t  s_own_addr_type = BLE_ADDR_PUBLIC;
static uint32_t s_frame_cnt = 1;
static uint8_t  s_mac_air[6];              /* LSB 序，帧内 MAC */
static uint8_t  s_frame[32];               /* 当前广播帧 */
static uint8_t  s_flen;
static uint32_t s_wake_until;              /* 唤醒窗口截止（ms） */
static int      s_was_bound;

/* 阶段三按键事件：0=无，否则 0x4E0C/0x4E0E（载荷 1B，单键恒 1） */
static uint16_t s_btn_obj;
static uint32_t s_btn_until;               /* 事件重复广播截止（ms） */

/* ================= 绑定链 ================= */
static miot_auth_t   s_auth;
static miot_device_t s_dev;
static miot_prop_t  *s_tbl;
static size_t        s_cnt;
static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_mtu = BLE_ATT_MTU_DFLT;
static uint16_t s_h10, s_h19, s_h1a, s_h1b;
static uint8_t  s_wbuf[512];
static bool     s_keys_set = false;
static bool     s_diag_started = false;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }
static bool win_active(void)
{ return s_wake_until && (int32_t)(s_wake_until - now_ms()) > 0; }

/* 单调递增 + 持久化：nonce 含 cnt，回跳会被网关判重放丢弃（详见 fcnt_store.c） */
static void bump_cnt(void)
{
    s_frame_cnt++;
    if ((s_frame_cnt % 10) == 0) fcnt_save(s_frame_cnt);
}

static int adv_refresh(void);
static void adv_start(void);
static int ble_gap_cb(struct ble_gap_event *event, void *arg);

/* notify：发一个 GATT 特征（带 MTU 保护，金样 notify_chr） */
static int notify_chr(uint16_t handle, const uint8_t *d, size_t n)
{
    if (handle == 0 || s_conn == BLE_HS_CONN_HANDLE_NONE) return -1;
    uint16_t max = s_mtu > 3 ? (uint16_t)(s_mtu - 3) : 20;
    if (n > max) {
        ESP_LOGW(TAG, "notify h=%u len=%u > mtu-3=%u, dropped",
                 handle, (unsigned)n, max);
        return -1;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(d, n);
    if (!om) return -1;
    int rc = ble_gatts_notify_custom(s_conn, handle, om);
    if (rc != 0 && rc != BLE_HS_EDONE) os_mbuf_free_chain(om);
    return (rc == 0 || rc == BLE_HS_EDONE) ? 0 : -1;
}

/* 写入口：把 mbuf 拍平 */
static uint16_t take_flat(struct ble_gatt_access_ctxt *x, uint8_t *buf, size_t cap)
{
    uint16_t len = OS_MBUF_PKTLEN(x->om);
    if (len > cap) return 0;
    if (ble_hs_mbuf_to_flat(x->om, buf, cap, &len) != 0) return 0;
    return len;
}

/* ★金样 is_auth_cmd：0x0010 上认证命令（1B/4B 白名单）走 auth，
 * 其余 ag2 数据帧（首字节 0x00）走 spec。一律转 auth 会吞掉 spec 请求。 */
static int is_auth_cmd(const uint8_t *d, uint16_t len)
{
    if (len == 1 && d[0] == 0xA4) return 1;                    /* 建链 */
    if (len != 4) return 0;
    if (d[1] || d[2] || d[3]) return 0;
    return d[0] == 0x24 || (d[0] & 0x7F) == 0x22;              /* LOGIN / REGISTER */
}

static int cb_10(uint16_t c, uint16_t a, struct ble_gatt_access_ctxt *x, void *g)
{
    (void)c; (void)a; (void)g;
    uint16_t n = take_flat(x, s_wbuf, sizeof(s_wbuf));
    if (n) {
        ESP_LOGI(TAG, "wr 0010 len=%u cmd=%02X", n, s_wbuf[0]);
        if (is_auth_cmd(s_wbuf, n)) miot_auth_on_0010(&s_auth, s_wbuf, n);
        else                        miot_device_on_rx(&s_dev, s_wbuf, n);
    }
    return 0;
}

static int cb_19(uint16_t c, uint16_t a, struct ble_gatt_access_ctxt *x, void *g)
{
    (void)c; (void)a; (void)g;
    uint16_t n = take_flat(x, s_wbuf, sizeof(s_wbuf));
    if (n) miot_auth_on_0019(&s_auth, s_wbuf, n);
    return 0;
}

static int cb_1a(uint16_t c, uint16_t a, struct ble_gatt_access_ctxt *x, void *g)
{
    (void)c; (void)a; (void)g;
    uint16_t n = take_flat(x, s_wbuf, sizeof(s_wbuf));
    if (n) miot_device_on_rx(&s_dev, s_wbuf, n);      /* 001A 与 001B 共用 */
    return 0;
}

/* FE95 小米 BLE 服务 + 四特征（少一个 App 连不上） */
static ble_uuid16_t u_svc = BLE_UUID16_INIT(0xFE95);

static const struct ble_gatt_chr_def chars[] = {
    { .uuid = BLE_UUID16_DECLARE(0x0010), .access_cb = cb_10, .val_handle = &s_h10,
      .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY },
    { .uuid = BLE_UUID16_DECLARE(0x0019), .access_cb = cb_19, .val_handle = &s_h19,
      .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY },
    { .uuid = BLE_UUID16_DECLARE(0x001A), .access_cb = cb_1a, .val_handle = &s_h1a,
      .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY },
    { .uuid = BLE_UUID16_DECLARE(0x001B), .access_cb = cb_1a, .val_handle = &s_h1b,
      .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP },
    { 0 }
};

static const struct ble_gatt_svc_def svcs[] = {
    { .type = BLE_GATT_SVC_TYPE_PRIMARY, .uuid = &u_svc.u, .characteristics = chars },
    { 0 }
};

/* ---- 回调出口 ----
 * ★spec notify 必须走 0x0010：App 只订阅 0010/0019，从不订阅 001A。 */
static int n_0010(void *u, const uint8_t *d, size_t n)
{ (void)u; return notify_chr(s_h10, d, n); }

static int n_0019(void *u, const uint8_t *d, size_t n)
{ (void)u; return notify_chr(s_h19, d, n); }

static int d_send(void *u, const uint8_t *d, size_t n)
{ (void)u; return notify_chr(s_h10, d, n); }           /* spec → 0010 */

static int rand_fill(void *u, uint8_t *o, size_t n)
{ (void)u; esp_fill_random(o, n); return 0; }

static uint32_t d_time(void *u)
{ (void)u; return now_ms(); }

static miot_auth_port_t s_aport = {
    .notify_0019 = n_0019,
    .notify_0010 = n_0010,
    .save        = store_save,
    .load        = store_load,
    .random      = rand_fill,
};

static miot_port_t s_dport = {
    .send_notify = d_send,
    .time_now    = d_time,
    .random      = rand_fill,
};

/* ---- 广播帧构造 ----
 * 未绑定：hello 帧（FC 高字节 0x2A 置 solicited，App 才发起连接）。
 * 已绑定：v5 nomac 加密帧（FC 0x48/0x59），有按键事件发 0x4E0C/0x4E0E，
 *         常态发 0x100A 电量心跳。唤醒窗口内置 0x5B solicited。 */
static int build_frame(void)
{
    miot_beacon_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.pid = BLE_SW_PID;
    memcpy(cfg.mac_air, s_mac_air, 6);

    if (!s_auth.bound) {
        int fl = miot_beacon_frame_hello(&cfg, s_frame_cnt, 2 /* 后绑定 */,
                                         s_frame, sizeof(s_frame));
        if (fl < 0) {
            ESP_LOGE(TAG, "frame_hello fail %d", fl);
            return fl;
        }
        s_frame[1] = 0x2A;   /* v2 | registered=0 | authMode=2 | solicited=1 */
        s_flen = (uint8_t)fl;
        return fl;
    }

    /* ★beacon_key = REGISTER 时从 ECDH 输出 [12:28] 提取的 bind_key */
    memcpy(cfg.beacon_key, s_auth.id.bind_key, 16);

    uint8_t objs[8];
    size_t n = miot_beacon_obj_u8(objs,
                                  s_btn_obj ? s_btn_obj : MB_OBJ_BATTERY,
                                  s_btn_obj ? 1 : V_BATT);
    int fl = miot_beacon_frame_v5_nomac(&cfg, s_frame_cnt, objs, n,
                                        s_frame, sizeof(s_frame));
    if (fl < 0) {
        ESP_LOGE(TAG, "frame_v5_nomac fail %d (bind_key/crypto?)", fl);
        return fl;
    }
    /* ★唤醒窗口内 0x59 + bit9 = 0x5B，让 App 可再连 */
    if (win_active()) s_frame[1] = 0x5B;
    s_flen = (uint8_t)fl;
    return fl;
}

/* 设备名 + V2 信标帧放 scan response（金样 adv_start） */
static void set_scan_rsp(void)
{
    /* 0x038F V2 信标：0x20=未注册。绑定完成后改成 0x00（阶段三）。 */
    static uint8_t mfr[16];
    int m = 0;
    mfr[m++] = 0x8F; mfr[m++] = 0x03;
    mfr[m++] = 0x30;
    mfr[m++] = (uint8_t)(s_auth.bound ? 0x00 : 0x20);
    mfr[m++] = (uint8_t)(BLE_SW_PID & 0xFF);
    mfr[m++] = (uint8_t)(BLE_SW_PID >> 8);
    mfr[m++] = (uint8_t)s_frame_cnt;
    for (int i = 0; i < 6; i++) mfr[m++] = s_mac_air[i];
    mfr[m++] = 0x08;

    struct ble_hs_adv_fields rsp;
    memset(&rsp, 0, sizeof(rsp));
    rsp.name = (const uint8_t *)BLE_SW_NAME;
    rsp.name_len = (uint8_t)strlen(BLE_SW_NAME);
    rsp.name_is_complete = 1;
    rsp.mfg_data = mfr;
    rsp.mfg_data_len = (uint8_t)m;
    if (ble_gap_adv_rsp_set_fields(&rsp) != 0)
        ESP_LOGW(TAG, "adv_rsp set failed");
}

static void adv_start(void)
{
    if (ble_gap_adv_active()) return;        /* 已在广播，别重复启动（rc=6） */

    static const ble_uuid16_t uuid_fe95 = BLE_UUID16_INIT(0xFE95);

    uint8_t sd[32];
    int n = 0;
    sd[n++] = 0x95; sd[n++] = 0xFE;
    memcpy(sd + n, s_frame, s_flen);
    n += s_flen;

    struct ble_hs_adv_fields f;
    memset(&f, 0, sizeof(f));
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    /* ★显式带 16-bit Service UUID 列表：Android ScanFilter 按 0xFE95 过滤 */
    f.uuids16 = &uuid_fe95;
    f.num_uuids16 = 1;
    f.uuids16_is_complete = 1;
    f.svc_data_uuid16 = sd;
    f.svc_data_uuid16_len = (uint8_t)n;

    int rc = ble_gap_adv_set_fields(&f);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv_set_fields rc=%d (svc_data=%dB)", rc, n);
        return;
    }
    set_scan_rsp();

    struct ble_gap_adv_params p;
    memset(&p, 0, sizeof(p));
    p.conn_mode = BLE_GAP_CONN_MODE_UND;     /* 随时可被 App 连接 */
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    /* ★事件帧期间快广播：网关轮询窗口短，2.5s 重复期内必须至少扫到一帧。
     * 平时未绑定 160/320、绑定后 800 保活即可。 */
    if (s_btn_obj) {
        p.itvl_min = 48;
        p.itvl_max = 80;
    } else if (s_auth.bound) {
        p.itvl_min = 800;
        p.itvl_max = 800;
    } else {
        p.itvl_min = 160;
        p.itvl_max = 320;
    }

    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &p,
                           ble_gap_cb, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY)
        ESP_LOGE(TAG, "adv_start rc=%d", rc);
}

/* ★连接期间不刷新（金样 adv_refresh 首行 gatt_miot_is_connected） */
static int adv_refresh(void)
{
    if (s_conn != BLE_HS_CONN_HANDLE_NONE) return 0;
    if (ble_gap_adv_active()) ble_gap_adv_stop();
    adv_start();
    return 0;
}

/* auth 轮询：devConfirm 重发 + LOGIN 后接会话密钥 + 绑定态/唤醒窗口管理 */
static void ble_poll_task(void *arg)
{
    (void)arg;
    for (;;) {
        miot_auth_poll(&s_auth, now_ms());

        bool in = miot_auth_is_logged_in(&s_auth);
        if (in && !s_keys_set) {
            miot_device_set_session_keys(&s_dev, miot_auth_session_keys(&s_auth));
            s_keys_set = true;
            ESP_LOGI(TAG, "LOGIN ok → spec session keys set");
        } else if (!in && s_keys_set) {
            s_keys_set = false;
        }

        /* 绑定态翻转 → 重建帧（bound=1 时 scan rsp V2 帧改 0x00） */
        if (s_auth.bound != s_was_bound) {
            s_was_bound = s_auth.bound;
            ESP_LOGI(TAG, "*** bind state -> %d ***", s_was_bound);
            if (build_frame() >= 0) adv_refresh();
        }

        /* 唤醒窗口到期 → 清 solicited 位并重建 */
        static bool wake_prev = false;
        bool wake_now = win_active();
        if (wake_prev && !wake_now) {
            ESP_LOGI(TAG, "wake window closed");
            if (build_frame() >= 0) adv_refresh();
        }
        wake_prev = wake_now;

        /* 事件 2.5s 重复期结束 → 回到电量心跳帧 */
        if (s_btn_obj && (int32_t)(now_ms() - s_btn_until) >= 0) {
            ESP_LOGI(TAG, "btn event 0x%04X done -> back to battery frame", s_btn_obj);
            s_btn_obj = 0;
            bump_cnt();
            if (build_frame() >= 0) adv_refresh();
        }

        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

/* GAP 事件回调 */
static int ble_gap_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn = event->connect.conn_handle;
            s_mtu = BLE_ATT_MTU_DFLT;
            ble_gap_adv_stop();              /* ★连接后停广播 */
            ESP_LOGI(TAG, "GAP CONNECT handle=%d → App 已连上，进入绑定流程",
                     s_conn);
        } else {
            ESP_LOGW(TAG, "GAP CONNECT fail status=%d", event->connect.status);
            adv_start();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "GAP DISCONNECT reason=%d → 清会话并重启广播",
                 event->disconnect.reason);
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_keys_set = false;
        miot_auth_on_disconnect(&s_auth);
        miot_device_session_reset(&s_dev);
        adv_start();                         /* ★断开才恢复 */
        break;
    case BLE_GAP_EVENT_MTU:
        s_mtu = event->mtu.value;
        ESP_LOGI(TAG, "GAP MTU -> %u", s_mtu);
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        ESP_LOGI(TAG, "GAP SUBSCRIBE attr=%u notify=%d",
                 event->subscribe.attr_handle, event->subscribe.cur_notify);
        break;
    case BLE_GAP_EVENT_CONN_UPDATE:
        ESP_LOGI(TAG, "GAP CONN_UPDATE");
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        adv_start();
        break;
    default:
        break;
    }
    return 0;
}

/* 周期：诊断 + 每 3s 刷新帧计数（连接期间不刷） */
static void ble_diag_task(void *arg)
{
    (void)arg;
    for (;;) {
        ESP_LOGI(TAG, "diag: adv_active=%d host_synced=%d conn=%d bound=%d cnt=%u btn=0x%04X",
                 ble_gap_adv_active(), ble_hs_synced(),
                 s_conn != BLE_HS_CONN_HANDLE_NONE, miot_auth_is_bound(&s_auth),
                 (unsigned)s_frame_cnt, s_btn_obj);
        /* 事件重复期内保持快广播连续帧，不重建（增量交给 poll_task 到期） */
        if (s_conn == BLE_HS_CONN_HANDLE_NONE && !s_btn_obj) {
            bump_cnt();
            if (build_frame() >= 0) adv_refresh();
        }
        vTaskDelay(pdMS_TO_TICKS(ADV_PERIOD_MS));
    }
}

static void ble_on_sync(void)
{
    ESP_LOGI(TAG, "nimble host synced, MAC %02X:%02X:%02X:%02X:%02X:%02X bound=%d",
             s_mac_air[5], s_mac_air[4], s_mac_air[3],
             s_mac_air[2], s_mac_air[1], s_mac_air[0],
             miot_auth_is_bound(&s_auth));
    s_was_bound = miot_auth_is_bound(&s_auth);
    if (build_frame() >= 0) adv_start();

    if (!s_diag_started) {
        s_diag_started = true;
        xTaskCreatePinnedToCore(ble_diag_task, "ble_diag", 4096, NULL, 1, NULL, 0);
    }
}

static void ble_on_reset(int reason)
{
    ESP_LOGE(TAG, "host reset reason=%d", reason);
}

static void ble_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}

esp_err_t ble_switch_start(void)
{
    /* ★读回 frame_cnt（越过历史水位 + 跨烧录单调），否则网关判重放丢弃事件帧 */
    s_frame_cnt = fcnt_load();
    ESP_LOGI(TAG, "frame_cnt restored = %u", (unsigned)s_frame_cnt);

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    for (int i = 0; i < 6; i++) s_mac_air[i] = mac[5 - i];

    miot_port_esp32_crypto_init();

    /* prepare 预生成 ECDH ~390ms：必须在连接前完成，放这里（启动期阻塞可接受） */
    miot_auth_init(&s_auth, &s_aport);
    miot_auth_prepare(&s_auth);

    button_dev_init(&s_dev, &s_dport, &s_tbl, &s_cnt);   /* 无线开关 spec 档案 */
    ESP_LOGI(TAG, "auth ready: bound=%d props=%u",
             miot_auth_is_bound(&s_auth), (unsigned)s_cnt);

    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init fail: %s", esp_err_to_name(err));
        return err;
    }

    ble_hs_cfg.sync_cb = ble_on_sync;
    ble_hs_cfg.reset_cb = ble_on_reset;

    ble_svc_gap_init();
    ble_svc_gatt_init();                     /* ★1801 Generic Attribute */
    ble_svc_gap_device_name_set(BLE_SW_NAME);
    ble_gatts_count_cfg(svcs);
    ble_gatts_add_svcs(svcs);
    ble_att_set_preferred_mtu(247);          /* ★首选 MTU */

    nimble_port_freertos_init(ble_host_task);

    xTaskCreatePinnedToCore(ble_poll_task, "ble_poll", 4096, NULL, 1, NULL, 0);

    ESP_LOGI(TAG, "nimble host started (name=%s, GATT=FE95/0010/0019/001A/001B, FC=30 2A solicited)",
             BLE_SW_NAME);
    return ESP_OK;
}

/* 按键事件公共入口：绑定后发加密事件帧（2.5s 快广播），未绑定仅唤醒刷新。 */
static void fire_button(uint16_t obj, const char *name)
{
    /* 事件窗口内忽略重复触发：FSR DO 电平会 ~500ms 抖动一次，
     * 每次触发都 adv_refresh() 会 stop/start 打碎 2.5s 快广播，
     * 网关轮询窗口内抓不到连续帧 → 米家设备日志无点击记录。 */
    if (s_auth.bound && s_btn_obj) return;

    s_wake_until = now_ms() + WAKE_WINDOW_MS;
    bump_cnt();
    if (s_auth.bound) {
        s_btn_obj = obj;
        s_btn_until = now_ms() + REPEAT_MS;
    }
    ESP_LOGI(TAG, "FSR %s → event 0x%04X (bound=%d repeat=%dms win=%ds)",
             name, obj, s_auth.bound, s_auth.bound ? REPEAT_MS : 0,
             WAKE_WINDOW_MS / 1000);
    if (build_frame() >= 0) adv_refresh();
}

void ble_switch_button_press(void)
{
    fire_button(MB_OBJ_BTN_CLICK, "单击");
}

void ble_switch_button_hold(void)
{
    fire_button(MB_OBJ_BTN_HOLD, "长按");
}

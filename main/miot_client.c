/*
 * miot_client.c — 米家 MIoT 加密客户端（ESP32-C3 移植）
 *
 * 算法链（与 02-工具脚本/miot_client.py 字节级一致）：
 *   1. nonce = rand8 || int32be(now_ms/60000) → base64
 *   2. session_key = SHA256( b64decode(ssecurity) || b64decode(nonce) )
 *      ss = base64(session_key)
 *   3. 明文参数集 u = { data: <JSON串> }
 *      rc4_hash__ = _sign(POST, path, u, ss)
 *   4. 按字典序对 u 每个值做 RC4-drop1024 加密 → base64（RC4 状态链式）
 *   5. signature = _sign(POST, path, {data:<enc>, rc4_hash__:<enc>}, ss)
 *   6. form = data&rc4_hash__&signature&_nonce&ssecurity（全 urlencode）
 *
 * 传输：POST https://api.mijia.tech/app/miclaw<path>
 *   表单 x-www-form-urlencoded
 *   Cookie: cUserId=..; userId=..; serviceToken=..
 *   MIOT-ENCRYPT-ALGORITHM: ENCRYPT-RC4
 *
 * 响应：body 是 RC4-drop1024 加密的 base64 → b64 解码 → RC4 解密 → JSON。
 */
#include "miot_client.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_tls.h"
#include "esp_crt_bundle.h"
#include "mbedtls/base64.h"
#include "mbedtls/sha1.h"
#include "mbedtls/sha256.h"
#include "cJSON.h"

static const char *TAG = "miot";

/* 调试开关：1 时打印原始响应（敏感，仅调试） */
#define miot_debug  0

#define MIOT_PREFIX "/app/miclaw"
#define MIOT_UA     "MiClaw/1.0"

#define RESP_BUF_MAX  8192
#define FORM_BUF_MAX  6144

/* ============ 静态缓冲区 ============ */
static char s_raw[RESP_BUF_MAX];
static char s_plain[RESP_BUF_MAX];   /* 解密 JSON 文本 (额外 1B 给 NUL) */
static char s_ws[sizeof(s_plain)];   /* 去空白/解码暂存 */

/* ============ base64 ============ */
static void b64_encode_w(const unsigned char *in, size_t in_len,
                         char *out, size_t out_sz)
{
    size_t need = 4 * ((in_len + 2) / 3) + 1;
    if (out_sz < need) { if (out_sz) out[0] = '\0'; return; }
    size_t olen = 0;
    if (mbedtls_base64_encode((unsigned char *)out, out_sz, &olen,
                              in, in_len) == 0)
        out[olen] = '\0';
    else
        out[0] = '\0';
}

static int b64_decode_w(const char *in, unsigned char *out, size_t out_sz)
{
    size_t olen = 0;
    if (mbedtls_base64_decode(out, out_sz, &olen, (const unsigned char *)in,
                              strlen(in)) != 0)
        return -1;
    return (int)olen;
}

/* ============ RC4 + drop1024 ============ */
typedef struct { unsigned char s[256]; unsigned int i, j; } rc4_ctx;

static void rc4_key(rc4_ctx *ctx, const unsigned char *key, size_t key_len)
{
    unsigned int i, j = 0;
    if (key_len == 0) key_len = 1;
    for (i = 0; i < 256; i++) ctx->s[i] = (unsigned char)i;
    for (i = 0; i < 256; i++) {
        j = (j + ctx->s[i] + key[i % key_len]) & 255;
        unsigned char t = ctx->s[i];
        ctx->s[i] = ctx->s[j];
        ctx->s[j] = t;
    }
    ctx->i = ctx->j = 0;
}

static void rc4_crypt(rc4_ctx *ctx, const unsigned char *in,
                      unsigned char *out, size_t len)
{
    for (size_t n = 0; n < len; n++) {
        ctx->i = (ctx->i + 1) & 255;
        ctx->j = (ctx->j + ctx->s[ctx->i]) & 255;
        unsigned char t = ctx->s[ctx->i];
        ctx->s[ctx->i] = ctx->s[ctx->j];
        ctx->s[ctx->j] = t;
        unsigned char k = ctx->s[(ctx->s[ctx->i] + ctx->s[ctx->j]) & 255];
        out[n] = in[n] ^ k;
    }
}

static void rc4_codec(rc4_ctx *ctx, const unsigned char *key, size_t key_len)
{
    rc4_key(ctx, key, key_len);
    unsigned char sink[64];
    memset(sink, 0, sizeof(sink));
    for (int d = 0; d < 16; d++)   /* 丢弃 1024 字节 */
        rc4_crypt(ctx, sink, sink, sizeof(sink));
}

/* ============ nonce ============ */
/* nonce 尾部 4 字节 = int32be(epoch_秒 / 60)。
 * ⚠ 必须用真实墙钟（time()），不是开机计时（esp_timer）——服务端校验时间戳，
 *   否则认证失败。WiFi 连接后需先 NTP 同步（见 wifi_mgr/SNTP）。
 */
static void make_nonce(char *out, size_t out_sz)
{
    unsigned char raw[12];
    for (int i = 0; i < 8; i++) raw[i] = (unsigned char)(esp_random() & 0xFF);
    time_t now = time(NULL);
    if (now <= 0) now = 1; /* 未同步时的安全兜底 */
    uint32_t mins = (uint32_t)(now / 60);
    raw[8]  = (unsigned char)(mins >> 24);
    raw[9]  = (unsigned char)(mins >> 16);
    raw[10] = (unsigned char)(mins >> 8);
    raw[11] = (unsigned char)(mins & 0xFF);
    b64_encode_w(raw, 12, out, out_sz);
}

/* ============ session key ============ */
static int make_session_key(const char *ssecurity, const char *nonce,
                            unsigned char key_out[32])
{
    unsigned char d1[64], d2[64];
    int l1 = b64_decode_w(ssecurity, d1, sizeof(d1));
    int l2 = b64_decode_w(nonce, d2, sizeof(d2));
    if (l1 <= 0 || l2 <= 0 || (size_t)(l1 + l2) > 64) return -1;
    unsigned char concat[64];
    memcpy(concat, d1, l1);
    memcpy(concat + l1, d2, l2);
    mbedtls_sha256(concat, l1 + l2, key_out, 0);
    return 0;
}

/* ============ _sign：按 key 排序后 SHA1-base64 ============ */
static void sign_generic(const char *method, const char *path,
                         const char *keys[], const char *vals[], int n,
                         const char *ss, char out[64])
{
    int idx[8];
    for (int i = 0; i < n; i++) idx[i] = i;
    for (int i = 0; i < n - 1; i++)
        for (int j = i + 1; j < n; j++)
            if (strcmp(keys[idx[j]], keys[idx[i]]) < 0) {
                int t = idx[i]; idx[i] = idx[j]; idx[j] = t;
            }

    size_t cap = strlen(method) + 1 + strlen(path) + 1 + strlen(ss) + 1;
    for (int i = 0; i < n; i++)
        cap += strlen(keys[idx[i]]) + 1 + strlen(vals[idx[i]]) + 1;
    char *buf = (char *)malloc(cap + 1);
    if (!buf) { out[0] = 0; return; }

    size_t off = 0;
    off += (size_t)snprintf(buf + off, cap - off, "%s&%s", method, path);
    for (int i = 0; i < n; i++)
        off += (size_t)snprintf(buf + off, cap - off, "&%s=%s",
                                 keys[idx[i]], vals[idx[i]]);
    off += (size_t)snprintf(buf + off, cap - off, "&%s", ss);

    unsigned char h[20];
    mbedtls_sha1((unsigned char *)buf, off, h);
    b64_encode_w(h, 20, out, 64);
    free(buf);
}

/* ============ urlencode ============ */
static void urlenc(const char *in, char *out, size_t out_sz)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (size_t i = 0; i < strlen(in) && o + 3 < out_sz; i++) {
        unsigned char c = (unsigned char)in[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_' ||
            c == '.' || c == '~') {
            out[o++] = (char)c;
        } else if (c == ' ') {
            out[o++] = '+';
        } else {
            out[o++] = '%';
            out[o++] = hex[c >> 4];
            out[o++] = hex[c & 0xF];
        }
    }
    out[o] = '\0';
}

/* ============ 加密表单构造（上下文回传 nonce）============ */
typedef struct { char nonce[64]; } miot_tx_ctx;

static int build_encrypted_form(const char *stripped_path,
                                const char *ssecurity,
                                const char *data_json,
                                char *form_buf, size_t form_sz,
                                miot_tx_ctx *ctx)
{
    unsigned char session_key[32];
    if (!ctx) return -1;
    make_nonce(ctx->nonce, sizeof(ctx->nonce));
    if (make_session_key(ssecurity, ctx->nonce, session_key) != 0)
        return -1;

    char ss[64];
    b64_encode_w(session_key, 32, ss, sizeof(ss));

    /* rc4_hash__ 在明文 u={data} 上 */
    char rc4_hash[64];
    const char *k1[1] = { "data" };
    const char *v1[1] = { data_json };
    sign_generic("POST", stripped_path, k1, v1, 1, ss, rc4_hash);

    /* RC4 链式加密：data 再 rc4_hash__ */
    rc4_ctx rc4;
    rc4_codec(&rc4, session_key, 32);

    size_t dl = strlen(data_json);
    static char enc_data[4096];   /* .bss，防栈溢出 */
    {
        unsigned char *tmp = (unsigned char *)malloc(dl ? dl : 1);
        if (!tmp) return -1;
        rc4_crypt(&rc4, (const unsigned char *)data_json, tmp, dl);
        b64_encode_w(tmp, dl, enc_data, sizeof(enc_data));
        free(tmp);
    }

    char enc_hash[128];
    {
        size_t hl = strlen(rc4_hash);
        unsigned char tmp2[64];
        rc4_crypt(&rc4, (const unsigned char *)rc4_hash, tmp2, hl);
        b64_encode_w(tmp2, hl, enc_hash, sizeof(enc_hash));
    }

    /* signature 在密文集上 */
    char signature[64];
    const char *k2[2] = { "data", "rc4_hash__" };
    const char *v2[2] = { enc_data, enc_hash };
    sign_generic("POST", stripped_path, k2, v2, 2, ss, signature);

    /* 组装 urlencoded 表单。e[5][4096] 共 20KB —— 必须 static 放 .bss，
       放栈会栈溢出（C3 主任务 8KB 栈在 esp_http_client 内已触发保护 panic）。 */
    static char e[5][4096];
    urlenc(enc_data,   e[0], sizeof(e[0]));
    urlenc(enc_hash,   e[1], sizeof(e[1]));
    urlenc(signature,  e[2], sizeof(e[2]));
    urlenc(ctx->nonce, e[3], sizeof(e[3]));
    urlenc(ssecurity,  e[4], sizeof(e[4]));

    int n = snprintf(form_buf, form_sz,
                     "data=%s&rc4_hash__=%s&signature=%s&_nonce=%s&ssecurity=%s",
                     e[0], e[1], e[2], e[3], e[4]);
    if (n < 0 || (size_t)n >= form_sz) return -1;
    return 0;
}

/* ============ 核心 POST（esp_tls 手写 HTTP）============
 * 放弃 esp_http_client：其对小米系 https（api.micloud/api.mijia）
 * 连接管理有已知兼容问题（esp_http_client 60s 超时 / EXCHANGE_CONNECT
 * 反复失败）。改用 esp_tls 直连 + 手写 HTTP/1.1（内存记忆惯例）。
 */
#include "esp_tls.h"

static char *miot_post_raw(const miot_auth_t *auth, const char *api_path,
                           const char *data_json, size_t *out_len)
{
    /* 签名 path：/app/miclaw<api_path> 去掉首个 /app → /miclaw<api_path> */
    char sign_path[176];
    snprintf(sign_path, sizeof(sign_path), "%s%s", MIOT_PREFIX, api_path);
    const char *p = (strncmp(sign_path, "/app", 4) == 0) ? sign_path + 4
                                                         : sign_path;
    char stripped[176];
    strncpy(stripped, p, sizeof(stripped) - 1);
    stripped[sizeof(stripped) - 1] = '\0';

    miot_tx_ctx ctx;
    static char form[FORM_BUF_MAX];  /* .bss，防栈溢出 */
    if (build_encrypted_form(stripped, auth->ssecurity, data_json,
                             form, sizeof(form), &ctx) != 0) {
        ESP_LOGE(TAG, "encrypt form failed");
        return NULL;
    }

    static char cookie[800];
    snprintf(cookie, sizeof(cookie),
             "cUserId=%s; userId=%s; serviceToken=%s",
             (auth->cUserId[0] ? auth->cUserId : "null"),
             auth->userId, auth->serviceToken);

    /* -------- 手写 HTTP POST（每次请求独立连接，最稳）-------- */
    static char req[8192];
    int req_len = snprintf(req, sizeof(req),
        "POST %s%s HTTP/1.1\r\n"
        "Host: api.mijia.tech\r\n"
        "User-Agent: %s\r\n"
        "Content-Type: application/x-www-form-urlencoded\r\n"
        "x-xiaomi-protocal-flag-cli: PROTOCAL-HTTP2\r\n"
        "MIOT-ENCRYPT-ALGORITHM: ENCRYPT-RC4\r\n"
        "Accept-Encoding: identity\r\n"
        "Cookie: %s\r\n"
        "Connection: close\r\n"
        "Content-Length: %u\r\n"
        "\r\n"
        "%s",
        MIOT_PREFIX, api_path, MIOT_UA, cookie,
        (unsigned)strlen(form), form);
    if (req_len < 0 || req_len >= (int)sizeof(req)) {
        ESP_LOGE(TAG, "req overflow");
        return NULL;
    }

    /* 建一条新连接（不跨请求复用；复用被服务端关闭导致 get 读空）。
       返回语义：esp_tls 1=成功，0=超时，-1=失败。 */
    struct esp_tls *tls = esp_tls_init();
    if (!tls) {
        ESP_LOGE(TAG, "esp_tls init fail");
        return NULL;
    }
    esp_tls_cfg_t tls_cfg = {
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 20000,
        .addr_family = ESP_TLS_AF_INET,   /* C3 禁 IPv6：强制 IPv4 */
    };
    int rc = esp_tls_conn_http_new_sync("https://api.mijia.tech",
                                        &tls_cfg, tls);
    if (rc != 1) {
        ESP_LOGE(TAG, "esp_tls connect fail rc=%d", rc);
        esp_tls_conn_destroy(tls);
        return NULL;
    }

    int w = esp_tls_conn_write(tls, req, req_len);
    if (w != req_len) {
        ESP_LOGE(TAG, "tls write partial %d/%d", w, req_len);
        esp_tls_conn_destroy(tls);
        return NULL;
    }

    /* 读响应头 + 全部 body（Connection: close，读到 EOF） */
    memset(s_raw, 0, sizeof(s_raw));
    size_t got = 0;
    while (got + 1 < sizeof(s_raw)) {
        int r = esp_tls_conn_read(tls, (unsigned char *)(s_raw + got),
                                  (int)(sizeof(s_raw) - 1 - got));
        if (r <= 0) break;
        got += (size_t)r;
    }
    s_raw[got] = '\0';
    esp_tls_conn_destroy(tls);   /* 用毕即关 */
    if (got == 0) {
        ESP_LOGE(TAG, "empty tls resp");
        return NULL;
    }

    if (miot_debug) {
        ESP_LOGI(TAG, "raw resp (%d B): %.500s", (int)got, s_raw);
    }

    /* 定位 body（\r\n\r\n 后） */
    char *body = strstr(s_raw, "\r\n\r\n");
    if (!body) {
        ESP_LOGE(TAG, "no header terminator");
        return NULL;
    }
    body += 4;
    size_t body_len = got - (size_t)(body - s_raw);
    if (body_len == 0) {
        ESP_LOGE(TAG, "empty body");
        return NULL;
    }

    /* de-b64 + RC4 解密 → s_plain */
    /* 去空白 */
    size_t ws_len = 0;
    for (size_t i = 0; i < body_len; i++) {
        char c = body[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r') continue;
        if (ws_len >= sizeof(s_ws) - 1) break;
        s_ws[ws_len++] = c;
    }
    int dlen = b64_decode_w(s_ws, (unsigned char *)s_ws, sizeof(s_ws) - 1);
    if (dlen <= 0) {
        ESP_LOGE(TAG, "resp b64 decode fail");
        return NULL;
    }

    unsigned char sk[32];
    if (make_session_key(auth->ssecurity, ctx.nonce, sk) != 0) {
        ESP_LOGE(TAG, "session key fail");
        return NULL;
    }
    rc4_ctx rc4;
    rc4_codec(&rc4, sk, 32);
    rc4_crypt(&rc4, (unsigned char *)s_ws, (unsigned char *)s_plain, dlen);
    s_plain[dlen] = '\0';

    if (out_len) *out_len = dlen;
    return s_plain;
}

/* ============ JSON 业务封装 ============ */

static char json_buf[2048];

/* 生成 /miotspec/prop/set 的 data JSON：{"params":[{"did":..,"siid":..,"piid":..,"value":..}]} */
static int build_prop_set_json(const char *did, int siid, int piid,
                               bool value, char *out, size_t out_sz)
{
    /* cJSON 构造最稳妥（value bool → JSON true/false） */
    cJSON *root = cJSON_CreateObject();
    if (!root) return -1;
    cJSON *params = cJSON_CreateArray();
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "did", did);
    cJSON_AddNumberToObject(item, "siid", siid);
    cJSON_AddNumberToObject(item, "piid", piid);
    cJSON_AddBoolToObject(item, "value", value);
    cJSON_AddItemToArray(params, item);
    cJSON_AddItemToObject(root, "params", params);
    char *s = cJSON_PrintUnformatted(root);
    if (!s) { cJSON_Delete(root); return -1; }
    int n;
    if (strlen(s) < out_sz)
        n = snprintf(out, out_sz, "%s", s);
    else
        n = -1;
    free(s);
    cJSON_Delete(root);
    return n >= 0 ? 0 : -1;
}

static int build_prop_get_json(const char *did, int siid, int piid,
                               char *out, size_t out_sz)
{
    cJSON *root = cJSON_CreateObject();
    if (!root) return -1;
    cJSON *params = cJSON_CreateArray();
    cJSON *item = cJSON_CreateObject();
    cJSON_AddStringToObject(item, "did", did);
    cJSON_AddNumberToObject(item, "siid", siid);
    cJSON_AddNumberToObject(item, "piid", piid);
    cJSON_AddItemToArray(params, item);
    cJSON_AddItemToObject(root, "params", params);
    cJSON_AddNumberToObject(root, "datasource", 1);
    char *s = cJSON_PrintUnformatted(root);
    if (!s) { cJSON_Delete(root); return -1; }
    int n = snprintf(out, out_sz, "%s", s);
    free(s);
    cJSON_Delete(root);
    return n >= 0 ? 0 : -1;
}

/* 解析 /prop/set 或 /prop/get 响应的 result[0] */
static void parse_prop_result(const char *json_txt, miot_prop_result_t *r)
{
    memset(r, 0, sizeof(*r));
    cJSON *root = cJSON_Parse(json_txt);
    if (!root) return;

    cJSON *result = cJSON_GetObjectItem(root, "result");
    if (result && cJSON_IsArray(result) && cJSON_GetArraySize(result) > 0) {
        cJSON *first = cJSON_GetArrayItem(result, 0);
        if (first) {
            cJSON *code = cJSON_GetObjectItem(first, "code");
            if (code && cJSON_IsNumber(code)) {
                r->code = (int)code->valueint;
                r->ok = (r->code == 0);
            } else {
                r->ok = (cJSON_GetObjectItem(first, "value") != NULL);
            }
            cJSON *msg = cJSON_GetObjectItem(first, "msg");
            if (msg && cJSON_IsString(msg))
                strncpy(r->msg, msg->valuestring, sizeof(r->msg) - 1);

            cJSON *ut = cJSON_GetObjectItem(first, "updateTime");
            if (ut && cJSON_IsNumber(ut)) r->update_time = (int64_t)ut->valuedouble;

            cJSON *val = cJSON_GetObjectItem(first, "value");
            if (val) {
                r->has_value = true;
                if (cJSON_IsBool(val)) {
                    r->value_bool = cJSON_IsTrue(val);
                    r->value_int  = r->value_bool ? 1 : 0;
                } else if (cJSON_IsNumber(val)) {
                    r->value_int  = (int64_t)val->valuedouble;
                    r->value_bool = (r->value_int != 0);
                } else if (cJSON_IsString(val)) {
                    const char *vs = val->valuestring;
                    r->value_bool = (strcmp(vs, "true") == 0 || strcmp(vs, "1") == 0);
                    r->value_int  = atoll(vs);
                }
            }
        }
    } else {
        cJSON *code = cJSON_GetObjectItem(root, "code");
        if (code && cJSON_IsNumber(code)) {
            r->code = (int)code->valueint;
            r->ok = false;
        }
    }
    cJSON_Delete(root);
}

esp_err_t miot_prop_set(const miot_auth_t *auth,
                        const char *did, int siid, int piid,
                        bool value, miot_prop_result_t *r)
{
    if (!auth || !auth->serviceToken[0] || !auth->ssecurity[0] ||
        !did || !did[0] || !r)
        return ESP_ERR_INVALID_ARG;

    if (build_prop_set_json(did, siid, piid, value,
                           json_buf, sizeof(json_buf)) != 0)
        return ESP_ERR_NO_MEM;

    size_t len = 0;
    char *resp = miot_post_raw(auth, "/miotspec/prop/set", json_buf, &len);
    if (!resp) return ESP_FAIL;

    parse_prop_result(resp, r);
    /* esp_err 只表达传输层；业务成败看 r->code（0=成功，1=网关已接收） */
    return ESP_OK;
}

esp_err_t miot_prop_get(const miot_auth_t *auth,
                        const char *did, int siid, int piid,
                        miot_prop_result_t *r)
{
    if (!auth || !auth->serviceToken[0] || !auth->ssecurity[0] ||
        !did || !did[0] || !r)
        return ESP_ERR_INVALID_ARG;

    if (build_prop_get_json(did, siid, piid, json_buf, sizeof(json_buf)) != 0)
        return ESP_ERR_NO_MEM;

    size_t len = 0;
    char *resp = miot_post_raw(auth, "/miotspec/prop/get", json_buf, &len);
    if (!resp) return ESP_FAIL;

    parse_prop_result(resp, r);
    /* 业务成败由调用方看 r.has_value/r.value_*；esp_err 只表达传输层 */
    return ESP_OK;
}

void miot_client_init(void)
{
    /* 静态缓冲清零；连接由 esp_tls 持久句柄按需建立。 */
    memset(s_raw, 0, sizeof(s_raw));
    memset(s_plain, 0, sizeof(s_plain));
    memset(s_ws, 0, sizeof(s_ws));
}
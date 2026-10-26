/*
 * 手机遥控：Wi-Fi 热点 + HTTP
 */
#include "remote.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "sdkconfig.h"
#include "dns_server.h"
#include "ble_link.h"
#include "control.h"
#include "esp_app_desc.h"
#include "ota.h"

#define CMD_TIMEOUT_MS      1000    // 主循环 5 ms 一轮，正常几毫秒内就会回复
#define BLE_NOTIFY_MS       500
#define BLE_RESULT_BAD_ARG  4
#define LINK_PERIOD_MS      200
#define WIFI_RESUME_MS      3000    // 蓝牙断开后延时恢复热点，避免短暂断连反复开关射频
#define BLE_START_GAP_MS    2000    // Wi-Fi 启动后再启动蓝牙，错开两次射频初始化的电流峰值

typedef struct {
    uint32_t seq;
    remote_result_t result;
} reply_t;

static const char *TAG = "remote";

extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

static volatile bool s_running;
static volatile bool s_starting;
static char s_ssid[33];
static volatile int s_clients;
static QueueHandle_t s_cmd_q, s_reply_q;
static uint32_t s_seq;
static remote_status_t s_status;
static esp_netif_t *s_netif;
static volatile bool s_wifi_on;
static char s_ble_name[24];
static volatile uint8_t s_ble_seq, s_ble_result;
static volatile bool s_ble_dirty;
static TickType_t s_ble_t_notify;
static portMUX_TYPE s_status_lock = portMUX_INITIALIZER_UNLOCKED;

/* ---------- 主循环侧 ---------- */

bool remote_is_running(void)
{
    return s_running;
}

bool remote_is_starting(void)
{
    return s_starting;
}

const char *remote_ssid(void)
{
    return s_ssid;
}

const char *remote_password(void)
{
    return CONFIG_REMOTE_WIFI_PASSWORD;
}

int remote_client_count(void)
{
    return s_clients;
}

bool remote_poll(remote_cmd_t *cmd)
{
    return s_cmd_q && xQueueReceive(s_cmd_q, cmd, 0) == pdTRUE;
}

void remote_reply(const remote_cmd_t *cmd, remote_result_t result)
{
    if (cmd->source == REMOTE_SRC_BLE) {
        // 结果随下一次状态通知带回
        s_ble_seq = (uint8_t)cmd->seq;
        s_ble_result = (uint8_t)result;
        s_ble_dirty = true;
        return;
    }
    const reply_t r = {.seq = cmd->seq, .result = result};
    xQueueSend(s_reply_q, &r, 0);
}

#ifdef CONFIG_REMOTE_BLE_ENABLE
// 蓝牙状态包，20 字节小端：
//   [0] 版本 1  [1] 最近命令 seq  [2] 最近命令结果 remote_result_t（BLE_RESULT_BAD_ARG = 参数错）
//   [3] 标志位 bit0 vbus_ok bit1 vbus_9v bit2 vbus_sat bit3 temp_valid bit4 warn bit5 protect
//              bit6 demo bit7 timer_on
//   [4..5] pwm  [6..7] setpoint  [8..9] limit（千分比）  [10..11] vbus mV
//   [12..13] 温度 x10（int16）   [14..15] 定时剩余秒（>65535 时饱和）
//   [16] fault  [17] driver
//   [18] bit0 在运行页，bits1~7 关断阈值的下限（℃）= 板上预警+1，随预警联动。
//        页面把它右移 1 位当范围下限用，别在页面里写死；更早的固件这一字节只有 0/1
//        （高位恒 0），页面据此判断读不到下限、退回固定值 61。
//   [19] 过温关断阈值（℃）
//
// 总长必须 ≤ 20 字节：手机不主动协商 MTU 时 ATT MTU 是 23，一条 Notify 只能带
// MTU-3 = 20 字节，21 字节的包会被协议栈截掉最后一字节（Read 的上限是 MTU-1 = 22，
// 所以连接时那一次 Read 反而看不出问题）。要加字段就得先挤掉一个，或者在 App 里
// requestMtu() 并在 onMtuChanged 之后才订阅——目前没那么做。
_Static_assert(BLE_LINK_STATUS_LEN <= 20, "Notify 装不下超过 20 字节的状态包");
// [18] 高 7 位放关断下限，超了就截断了
_Static_assert(CONTROL_SHUTDOWN_C_MAX < 128, "关断阈值上限超过 [18] 的高 7 位");
// 关断阈值的下限现在跟着预警走（= 预警+1，见 control.h），不是编译期常量。
// 主循环在 remote_publish() 里填进状态快照，这里读那一份；还没发布过就退回绝对下限。
static uint16_t shut_min_now(void)
{
    uint16_t v;
    portENTER_CRITICAL(&s_status_lock);
    v = s_status.shut_min;
    portEXIT_CRITICAL(&s_status_lock);
    return v ? v : CONTROL_SHUTDOWN_C_MIN;
}

static void pack_status(const remote_status_t *st, uint8_t out[BLE_LINK_STATUS_LEN])
{
    int t10 = st->temp_valid ? (int)(st->temp_c * 10 + (st->temp_c >= 0 ? 0.5f : -0.5f)) : 0;
    uint32_t left = st->timer_left_s > 0xFFFF ? 0xFFFF : st->timer_left_s;
    memset(out, 0, BLE_LINK_STATUS_LEN);
    out[0] = 1;
    out[1] = s_ble_seq;
    out[2] = s_ble_result;
    out[3] = (st->vbus_ok << 0) | (st->vbus_9v << 1) | (st->vbus_saturated << 2) | (st->temp_valid << 3) |
             (st->warn << 4) | (st->protect << 5) | (st->demo << 6) | (st->timer_active << 7);
#define PUT16(i, v) do { out[i] = (uint8_t)(v); out[(i) + 1] = (uint8_t)((uint16_t)(v) >> 8); } while (0)
    PUT16(4, st->pwm);
    PUT16(6, st->setpoint);
    PUT16(8, st->limit);
    PUT16(10, st->vbus_mv < 0 ? 0 : st->vbus_mv);
    PUT16(12, (int16_t)t10);
    PUT16(14, left);
#undef PUT16
    out[16] = st->fault;
    out[17] = st->driver;
    // 关断下限压进 [18] 的高 7 位，给不出有效值时页面会退回 61（见 ble_link.h）
    uint16_t shut = st->shut_min ? st->shut_min : CONTROL_SHUTDOWN_C_MIN;
    out[18] = (uint8_t)((st->on_run_page ? 1 : 0) | (shut << 1));
    out[19] = (uint8_t)st->shutdown_c;
}
#endif

void remote_publish(const remote_status_t *st)
{
    portENTER_CRITICAL(&s_status_lock);
    s_status = *st;
    portEXIT_CRITICAL(&s_status_lock);

#ifdef CONFIG_REMOTE_BLE_ENABLE
    // 有新命令结果时立即通知，否则每 BLE_NOTIFY_MS 一次（倒计时按秒变化）
    TickType_t now = xTaskGetTickCount();
    if (ble_link_connected() && (s_ble_dirty || now - s_ble_t_notify >= pdMS_TO_TICKS(BLE_NOTIFY_MS))) {
        uint8_t buf[BLE_LINK_STATUS_LEN];
        pack_status(st, buf);
        ble_link_publish(buf);
        s_ble_dirty = false;
        s_ble_t_notify = now;
    }
#endif
}

void remote_ble_command(uint8_t op, uint8_t seq, uint16_t value)
{
    remote_cmd_t cmd = {.source = REMOTE_SRC_BLE, .seq = seq};
    switch (op) {
    case 1: cmd.kind = REMOTE_CMD_DUTY;  cmd.permille = value > 1000 ? 1000 : value; break;
    case 2: cmd.kind = REMOTE_CMD_TIMER; cmd.minutes = value > 999 ? 999 : value;   break;
    case 3: cmd.kind = REMOTE_CMD_STOP;  break;
    case 4:
        // 关断温度：范围在 remote.c 里先把关（下限随预警阈值联动），
        // 网页/App 能拿到明确的“参数错误”
        if (value < shut_min_now() || value > CONTROL_SHUTDOWN_C_MAX) {
            s_ble_seq = seq;
            s_ble_result = BLE_RESULT_BAD_ARG;
            s_ble_dirty = true;
            return;
        }
        cmd.kind = REMOTE_CMD_TEMP;
        cmd.temp_c = value;
        break;
    default:
        s_ble_seq = seq;
        s_ble_result = BLE_RESULT_BAD_ARG;
        s_ble_dirty = true;
        return;
    }
    if (!s_cmd_q || xQueueSend(s_cmd_q, &cmd, 0) != pdTRUE) {
        s_ble_seq = seq;
        s_ble_result = REMOTE_ERR_BUSY;
        s_ble_dirty = true;
    }
}

remote_link_t remote_link(void)
{
#ifdef CONFIG_REMOTE_BLE_ENABLE
    if (ble_link_connected()) {
        return REMOTE_LINK_BLE;
    }
#endif
    return s_clients > 0 ? REMOTE_LINK_WIFI : REMOTE_LINK_NONE;
}

bool remote_wifi_on(void)
{
    return s_wifi_on;
}

bool remote_ble_running(void)
{
#ifdef CONFIG_REMOTE_BLE_ENABLE
    return ble_link_running();
#else
    return false;
#endif
}

const char *remote_ble_name(void)
{
    return s_ble_name;
}

/* ---------- HTTP 侧（httpd 任务） ---------- */

static esp_err_t send_json(httpd_req_t *req, const char *status, const char *body)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

// 投递命令并等待主循环执行结果
static esp_err_t run_cmd(httpd_req_t *req, remote_cmd_t *cmd)
{
    static const char *codes[] = {
        [REMOTE_OK] = "ok",
        [REMOTE_ERR_LOCKED] = "locked",
        [REMOTE_ERR_NOT_RUNNING] = "not_running",
        [REMOTE_ERR_BUSY] = "busy",
    };
    reply_t r;

    cmd->source = REMOTE_SRC_HTTP;
    cmd->seq = ++s_seq;
    xQueueReset(s_reply_q);     // 丢弃之前超时请求的迟到回复
    if (xQueueSend(s_cmd_q, cmd, pdMS_TO_TICKS(100)) != pdTRUE) {
        return send_json(req, "503 Service Unavailable", "{\"ok\":false,\"err\":\"busy\"}");
    }
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(CMD_TIMEOUT_MS);
    while (true) {
        TickType_t now = xTaskGetTickCount();
        if ((int32_t)(deadline - now) <= 0 ||
            xQueueReceive(s_reply_q, &r, deadline - now) != pdTRUE) {
            return send_json(req, "504 Gateway Timeout", "{\"ok\":false,\"err\":\"timeout\"}");
        }
        if (r.seq == cmd->seq) {
            break;
        }
    }

    char body[64];
    snprintf(body, sizeof(body), "{\"ok\":%s,\"err\":\"%s\"}",
             r.result == REMOTE_OK ? "true" : "false", codes[r.result]);
    return send_json(req, r.result == REMOTE_OK ? "200 OK" : "409 Conflict", body);
}

// 读取查询参数中的非负整数，失败返回 -1
static long query_uint(httpd_req_t *req, const char *key, long max)
{
    char q[64], v[16];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK ||
        httpd_query_key_value(q, key, v, sizeof(v)) != ESP_OK || v[0] == '\0') {
        return -1;
    }
    char *end;
    long n = strtol(v, &end, 10);
    return (*end != '\0' || n < 0 || n > max) ? -1 : n;
}

static esp_err_t h_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_send(req, index_html_start, index_html_end - index_html_start);
}

static esp_err_t h_status(httpd_req_t *req)
{
    remote_status_t st;
    char body[512];     // 加 ver 之后留足余量，截断了页面会解析失败

    portENTER_CRITICAL(&s_status_lock);
    st = s_status;
    portEXIT_CRITICAL(&s_status_lock);

    char temp[16] = "null";
    if (st.temp_valid) {
        int t10 = (int)(st.temp_c * 10 + (st.temp_c >= 0 ? 0.5f : -0.5f));
        snprintf(temp, sizeof(temp), "%s%d.%d", t10 < 0 ? "-" : "", abs(t10) / 10, abs(t10) % 10);
    }
    snprintf(body, sizeof(body),
             "{\"pwm\":%u,\"set\":%u,\"limit\":%u,"
             "\"vbus\":{\"ok\":%s,\"v9\":%s,\"mv\":%d,\"sat\":%s},"
             "\"temp\":%s,"
             "\"fault\":%u,\"warn\":%s,\"protect\":%s,\"demo\":%s,"
             "\"shut\":%u,\"shut_min\":%u,\"shut_max\":%u,"
             "\"timer\":{\"on\":%s,\"left\":%lu},\"run_page\":%s,\"driver\":%u,"
             "\"ver\":\"%s\",\"part\":\"%s\"}",
             st.pwm, st.setpoint, st.limit,
             st.vbus_ok ? "true" : "false", st.vbus_9v ? "true" : "false", st.vbus_mv,
             st.vbus_saturated ? "true" : "false",
             temp,
             st.fault, st.warn ? "true" : "false", st.protect ? "true" : "false",
             st.demo ? "true" : "false",
             st.shutdown_c, st.shut_min ? st.shut_min : CONTROL_SHUTDOWN_C_MIN, CONTROL_SHUTDOWN_C_MAX,
             st.timer_active ? "true" : "false", (unsigned long)st.timer_left_s,
             st.on_run_page ? "true" : "false", st.driver,
             esp_app_get_description()->version, ota_running_partition_label());
    return send_json(req, "200 OK", body);
}

static esp_err_t h_duty(httpd_req_t *req)
{
    long pct = query_uint(req, "pct", 100);
    if (pct < 0) {
        return send_json(req, "400 Bad Request", "{\"ok\":false,\"err\":\"bad_arg\"}");
    }
    remote_cmd_t cmd = {.kind = REMOTE_CMD_DUTY, .permille = (uint16_t)(pct * 10)};
    return run_cmd(req, &cmd);
}

static esp_err_t h_timer(httpd_req_t *req)
{
    long min = query_uint(req, "min", 999);
    if (min < 0) {
        return send_json(req, "400 Bad Request", "{\"ok\":false,\"err\":\"bad_arg\"}");
    }
    remote_cmd_t cmd = {.kind = REMOTE_CMD_TIMER, .minutes = (uint16_t)min};
    return run_cmd(req, &cmd);
}

static esp_err_t h_temp(httpd_req_t *req)
{
    long c = query_uint(req, "c", CONTROL_SHUTDOWN_C_MAX);
    // query_uint 失败回 -1；下限随预警阈值联动，得跟快照比
    if (c < 0 || c < shut_min_now()) {
        return send_json(req, "400 Bad Request", "{\"ok\":false,\"err\":\"bad_arg\"}");
    }
    remote_cmd_t cmd = {.kind = REMOTE_CMD_TEMP, .temp_c = (uint16_t)c};
    return run_cmd(req, &cmd);
}

static esp_err_t h_stop(httpd_req_t *req)
{
    remote_cmd_t cmd = {.kind = REMOTE_CMD_STOP};
    return run_cmd(req, &cmd);
}

// 未注册的地址一律 302 到控制页。iOS 需要响应体非空才判定为门户。
static esp_err_t h_redirect(httpd_req_t *req, httpd_err_code_t err)
{
    (void)err;
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, "Redirect to 850nm controller", HTTPD_RESP_USE_STRLEN);
}

static esp_err_t http_start(void)
{
    httpd_handle_t server = NULL;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.lru_purge_enable = true;   // 手机联网检测会开很多连接，满了就淘汰最旧的
    // 默认只有 8 个，加上 /api/ota 的 POST + OPTIONS 正好用满，再想加就没位置了
    cfg.max_uri_handlers = 12;
    // 联网检测请求很多，httpd 对无效请求的告警没有意义
    esp_log_level_set("httpd_uri", ESP_LOG_ERROR);
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
    esp_log_level_set("httpd_parse", ESP_LOG_ERROR);
    esp_err_t err = httpd_start(&server, &cfg);
    if (err != ESP_OK) {
        return err;
    }
    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = h_index},
        {.uri = "/api/status", .method = HTTP_GET, .handler = h_status},
        {.uri = "/api/duty", .method = HTTP_POST, .handler = h_duty},
        {.uri = "/api/timer", .method = HTTP_POST, .handler = h_timer},
        {.uri = "/api/temp", .method = HTTP_POST, .handler = h_temp},
        {.uri = "/api/stop", .method = HTTP_POST, .handler = h_stop},
        // 固件更新：POST 传 app.bin 原文，OPTIONS 是给独立更新页跨域用的预检
        {.uri = "/api/ota", .method = HTTP_POST, .handler = ota_http_post},
        {.uri = "/api/ota", .method = HTTP_OPTIONS, .handler = ota_http_options},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        httpd_register_uri_handler(server, &uris[i]);
    }
    // 其余地址（含手机的联网检测地址）全部重定向到控制页，触发系统弹出页面
    httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, h_redirect);
    return ESP_OK;
}

// DHCP Option 114（RFC 8910）：较新的 Android / iOS 直接从这里拿到门户地址
static void dhcp_set_portal_uri(esp_netif_t *netif)
{
    static char uri[32];
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(netif, &ip) != ESP_OK) {
        return;
    }
    snprintf(uri, sizeof(uri), "http://" IPSTR, IP2STR(&ip.ip));
    esp_netif_dhcps_stop(netif);
    if (esp_netif_dhcps_option(netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI, uri, strlen(uri)) != ESP_OK) {
        ESP_LOGW(TAG, "DHCP captive portal option not set");
    }
    esp_netif_dhcps_start(netif);
}

/* ---------- Wi-Fi ---------- */

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == WIFI_EVENT_AP_STACONNECTED) {
        const wifi_event_ap_staconnected_t *e = data;
        s_clients++;
        ESP_LOGI(TAG, "phone " MACSTR " connected (%d)", MAC2STR(e->mac), s_clients);
    } else if (id == WIFI_EVENT_AP_STADISCONNECTED) {
        const wifi_event_ap_stadisconnected_t *e = data;
        if (s_clients > 0) {
            s_clients--;
        }
        ESP_LOGI(TAG, "phone " MACSTR " left (%d)", MAC2STR(e->mac), s_clients);
    }
}

#define CHECK(x) do { esp_err_t e_ = (x); if (e_ != ESP_OK) { \
        ESP_LOGE(TAG, "%s failed: %s", #x, esp_err_to_name(e_)); return e_; } } while (0)

static esp_err_t wifi_bring_up(void)
{
    CHECK(esp_netif_init());
    CHECK(esp_event_loop_create_default());
    esp_netif_t *netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    CHECK(esp_wifi_init(&init));
    CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL, NULL));

    // SSID = 前缀 + MAC 后 2 字节，多块板子同时上电时不冲突
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    snprintf(s_ssid, sizeof(s_ssid), "%s%02X%02X", CONFIG_REMOTE_WIFI_SSID_PREFIX, mac[4], mac[5]);

    wifi_config_t wc = {
        .ap = {
            .channel = CONFIG_REMOTE_WIFI_CHANNEL,
            .max_connection = CONFIG_REMOTE_WIFI_MAX_CONN,
            .authmode = WIFI_AUTH_WPA2_PSK,
            .pmf_cfg = {.capable = true, .required = false},
        },
    };
    strlcpy((char *)wc.ap.ssid, s_ssid, sizeof(wc.ap.ssid));
    wc.ap.ssid_len = strlen(s_ssid);
    strlcpy((char *)wc.ap.password, CONFIG_REMOTE_WIFI_PASSWORD, sizeof(wc.ap.password));

    CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    CHECK(esp_wifi_set_config(WIFI_IF_AP, &wc));
    CHECK(esp_wifi_start());
    s_wifi_on = true;
    s_netif = netif;
    // 单位 0.25 dBm；与 CONFIG_ESP_PHY_MAX_WIFI_TX_POWER 一起限制发射峰值电流
    esp_wifi_set_max_tx_power(CONFIG_REMOTE_WIFI_TX_POWER_DBM * 4);
    dhcp_set_portal_uri(netif);
    CHECK(http_start());

    // 强制门户：所有域名的 A 记录都解析到本机，手机联网检测会落到本机 HTTP
    esp_log_level_set("example_dns_redirect_server", ESP_LOG_WARN);
    dns_server_config_t dns = DNS_SERVER_CONFIG_SINGLE("*", "WIFI_AP_DEF");
    if (!start_dns_server(&dns)) {
        ESP_LOGW(TAG, "DNS server failed, phone will not open the page automatically");
    }

    s_running = true;
    ESP_LOGI(TAG, "AP \"%s\" ch %d, TX %d dBm, captive portal http://192.168.4.1",
             s_ssid, CONFIG_REMOTE_WIFI_CHANNEL, CONFIG_REMOTE_WIFI_TX_POWER_DBM);
    return ESP_OK;
}

static void wifi_stop(void)
{
    if (!s_wifi_on) {
        return;
    }
    esp_wifi_stop();
    s_wifi_on = false;
    s_clients = 0;      // 停止热点时不一定逐个上报断开
    ESP_LOGI(TAG, "hotspot off (phone on Bluetooth)");
}

static void wifi_resume(void)
{
    if (s_wifi_on || !s_running) {
        return;
    }
    if (esp_wifi_start() != ESP_OK) {
        ESP_LOGE(TAG, "hotspot restart failed");
        return;
    }
    esp_wifi_set_max_tx_power(CONFIG_REMOTE_WIFI_TX_POWER_DBM * 4);
    dhcp_set_portal_uri(s_netif);
    s_wifi_on = true;
    ESP_LOGI(TAG, "hotspot back on");
}

/*
 * 连接管理：
 *   空闲        热点 + 蓝牙广播都开，两种方式都能连
 *   蓝牙已连接  关闭热点；断开 WIFI_RESUME_MS 后恢复热点
 *   有手机在热点上  停止蓝牙广播；手机全部离开后恢复广播
 */
static void link_task(void *arg)
{
    uint32_t delay_ms = (uint32_t)(uintptr_t)arg;
    ESP_LOGI(TAG, "Wi-Fi starts in %lu ms", (unsigned long)delay_ms);
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    if (wifi_bring_up() != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi remote not available");
    }

#ifdef CONFIG_REMOTE_BLE_ENABLE
    vTaskDelay(pdMS_TO_TICKS(BLE_START_GAP_MS));
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BT);
    snprintf(s_ble_name, sizeof(s_ble_name), "%s%02X%02X", CONFIG_REMOTE_WIFI_SSID_PREFIX, mac[4], mac[5]);
    if (ble_link_start(s_ble_name) != ESP_OK) {
        ESP_LOGE(TAG, "Bluetooth remote not available");
    }
#endif
    s_starting = false;

#ifdef CONFIG_REMOTE_BLE_ENABLE
    TickType_t ble_lost_at = 0;
    bool was_ble = false;
    while (ble_link_running()) {
        bool ble = ble_link_connected();
        TickType_t now = xTaskGetTickCount();
        if (ble) {
            // 固件正在上传时不能关热点：httpd 只有一个任务，热点一停这次上传就断了。
            // 刷完就重启了，重启后一切照旧。
            if (!ota_busy()) {
                wifi_stop();
            }
        } else {
            if (was_ble) {
                ble_lost_at = now;
            }
            if (!s_wifi_on && now - ble_lost_at >= pdMS_TO_TICKS(WIFI_RESUME_MS)) {
                wifi_resume();
            }
            ble_link_set_adv(s_clients == 0);
        }
        was_ble = ble;
        vTaskDelay(pdMS_TO_TICKS(LINK_PERIOD_MS));
    }
#endif
    vTaskDelete(NULL);
}

esp_err_t remote_start_delayed(uint32_t delay_ms)
{
    s_cmd_q = xQueueCreate(2, sizeof(remote_cmd_t));
    s_reply_q = xQueueCreate(2, sizeof(reply_t));
    if (!s_cmd_q || !s_reply_q) {
        return ESP_ERR_NO_MEM;
    }
    // 低优先级后台启动，射频校准期间主循环（按键、保护、界面）照常运行
    s_starting = true;
    if (xTaskCreate(link_task, "remote_link", 4096, (void *)(uintptr_t)delay_ms, 2, NULL) != pdPASS) {
        s_starting = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

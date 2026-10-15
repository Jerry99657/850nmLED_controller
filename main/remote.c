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

#define CMD_TIMEOUT_MS      1000    // 主循环 5 ms 一轮，正常几毫秒内就会回复

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
    const reply_t r = {.seq = cmd->seq, .result = result};
    xQueueSend(s_reply_q, &r, 0);
}

void remote_publish(const remote_status_t *st)
{
    portENTER_CRITICAL(&s_status_lock);
    s_status = *st;
    portEXIT_CRITICAL(&s_status_lock);
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
    char body[384];

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
             "\"timer\":{\"on\":%s,\"left\":%lu},\"run_page\":%s,\"driver\":%u}",
             st.pwm, st.setpoint, st.limit,
             st.vbus_ok ? "true" : "false", st.vbus_9v ? "true" : "false", st.vbus_mv,
             st.vbus_saturated ? "true" : "false",
             temp,
             st.fault, st.warn ? "true" : "false", st.protect ? "true" : "false",
             st.demo ? "true" : "false",
             st.timer_active ? "true" : "false", (unsigned long)st.timer_left_s,
             st.on_run_page ? "true" : "false", st.driver);
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
        {.uri = "/api/stop", .method = HTTP_POST, .handler = h_stop},
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

static void start_task(void *arg)
{
    uint32_t delay_ms = (uint32_t)(uintptr_t)arg;
    ESP_LOGI(TAG, "Wi-Fi starts in %lu ms", (unsigned long)delay_ms);
    vTaskDelay(pdMS_TO_TICKS(delay_ms));
    if (wifi_bring_up() != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi remote not available");
    }
    s_starting = false;
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
    if (xTaskCreate(start_task, "remote_start", 4096, (void *)(uintptr_t)delay_ms, 2, NULL) != pdPASS) {
        s_starting = false;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

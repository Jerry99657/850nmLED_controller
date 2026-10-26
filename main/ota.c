/*
 * 固件在线更新（HTTP OTA）：见 ota.h
 */
#include "ota.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "sdkconfig.h"
#include "led_pwm.h"

#define TAG             "ota"
#define CHUNK           4096        // 一次 recv 的字节数
#define REBOOT_DELAY_MS 1500        // 回包之后再重启，留时间让浏览器读完响应
#define LOG_EVERY       (256 * 1024)

/*
 * 接收缓冲必须是静态的：httpd 任务栈只有 4 KB（HTTPD_DEFAULT_CONFIG 的 stack_size），
 * 在 handler 里放个局部 4 KB 数组直接爆栈。httpd 是单个任务、handler 同步执行，
 * 同一时刻只可能有一个上传，静态数组不会打架。
 */
static uint8_t s_buf[CHUNK];
static volatile bool s_busy;

bool ota_busy(void)
{
    return s_busy;
}

// 与 remote.c 的 send_json 一致，另外加上跨域头：独立网页（file:// 打开时 Origin 是
// null）要能读到这个响应。不带 cookie，所以 Access-Control-Allow-Origin: * 是合法的。
static esp_err_t send_json(httpd_req_t *req, const char *status, const char *body)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

// 跨域预检。上传用的是 application/octet-stream，不在 CORS 简单请求的范围内，
// 所以从别的来源打开更新页时浏览器会先发一个 OPTIONS。
esp_err_t ota_http_options(httpd_req_t *req)
{
    httpd_resp_set_status(req, HTTPD_204);
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Methods", "POST, OPTIONS");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Headers", "Content-Type");
    return httpd_resp_send(req, NULL, 0);
}

// esp_err_t 换成页面能翻译的短标识
static const char *err_token(esp_err_t err)
{
    switch (err) {
    case ESP_ERR_OTA_VALIDATE_FAILED:   return "bad_image";     // 不是完好的 app 镜像 / 芯片型号不对
    case ESP_ERR_INVALID_SIZE:          return "bad_size";      // 比分区还大
    case ESP_ERR_OTA_ALREADY_IN_PROGRESS: return "busy";
    case ESP_ERR_INVALID_ARG:           return "not_this_board"; // 别的工程的固件
    default:                            return "write_failed";
    }
}

esp_err_t ota_http_post(httpd_req_t *req)
{
    if (s_busy) {
        return send_json(req, "503 Service Unavailable", "{\"ok\":false,\"err\":\"busy\"}");
    }
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (part == NULL) {
        ESP_LOGE(TAG, "没有可写的 OTA 分区，分区表不是双 OTA");
        return send_json(req, "500 Internal Server Error", "{\"ok\":false,\"err\":\"no_partition\"}");
    }
    if (req->content_len == 0 || req->content_len > part->size) {
        ESP_LOGW(TAG, "拒绝上传：%u 字节，\"%s\" 分区只有 %u",
                 (unsigned)req->content_len, part->label, (unsigned)part->size);
        return send_json(req, "400 Bad Request", "{\"ok\":false,\"err\":\"bad_size\"}");
    }

    ESP_LOGI(TAG, "开始更新：%u 字节 -> \"%s\"", (unsigned)req->content_len, part->label);
    s_busy = true;
    TickType_t t0 = xTaskGetTickCount();
    size_t left = req->content_len, done = 0, last_log = 0;
    unsigned timeouts = 0;
    esp_ota_handle_t h = 0;
    esp_err_t err = esp_ota_begin(part, req->content_len, &h);

    while (err == ESP_OK && left > 0) {
        int n = httpd_req_recv(req, (char *)s_buf, left < CHUNK ? left : CHUNK);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) {
            // 接收超时是按次算的（默认 5 s），手机偶尔卡一下不算断线。
            // 但手机走开时底层 socket 可能一直不报错，光靠超时就会永远卡在这里，
            // 所以连续超时到 60 s 就当断线处理。
            if (++timeouts > 12) {
                ESP_LOGW(TAG, "连续接收超时，放弃：%u/%u 字节", (unsigned)done, (unsigned)req->content_len);
                err = ESP_FAIL;
                break;
            }
            continue;
        }
        timeouts = 0;
        if (n <= 0) {
            ESP_LOGW(TAG, "上传中断在 %u/%u 字节", (unsigned)done, (unsigned)req->content_len);
            err = ESP_FAIL;
            break;
        }
        err = esp_ota_write(h, s_buf, (size_t)n);
        left -= (size_t)n;
        done += (size_t)n;
        if (done - last_log >= LOG_EVERY) {
            last_log = done;
            ESP_LOGI(TAG, "已收到 %u%%", (unsigned)(done * 100 / req->content_len));
        }
    }

    if (err == ESP_OK) {
        // 收尾并校验镜像（magic、芯片型号、校验和），传错东西在这里被挡下；
        // 无论成败 h 都作废了
        err = esp_ota_end(h);
    } else {
        esp_ota_abort(h);
    }

    // 还要确认这是"本工程"的固件：esp_ota_end 只管镜像完不完整，不管它是谁编的。
    // 刷进别的工程的 .bin 会得到一个能启动但完全不工作的板子，宁可拒掉。
    esp_app_desc_t desc = {0};
    if (err == ESP_OK) {
        err = esp_ota_get_partition_description(part, &desc);
    }
    if (err == ESP_OK) {
        const char *me = esp_app_get_description()->project_name;
        if (strncmp(desc.project_name, me, sizeof(desc.project_name)) != 0) {
            ESP_LOGE(TAG, "不是本工程的固件：\"%.*s\" != \"%.*s\"",
                     (int)sizeof(desc.project_name), desc.project_name,
                     (int)sizeof(desc.project_name), me);
            err = ESP_ERR_INVALID_ARG;
        }
    }
    if (err == ESP_OK) {
        err = esp_ota_set_boot_partition(part);
    }

    if (err != ESP_OK) {
        s_busy = false;
        ESP_LOGE(TAG, "更新失败：%s", esp_err_to_name(err));
        char body[96];
        snprintf(body, sizeof(body), "{\"ok\":false,\"err\":\"%s\",\"code\":%d}",
                 err_token(err), (int)err);
        return send_json(req, "409 Conflict", body);
    }

    ESP_LOGI(TAG, "更新完成：%u 字节，%u ms，下次从 \"%s\" 启动 v%.*s",
             (unsigned)done, (unsigned)((xTaskGetTickCount() - t0) * 1000 / configTICK_RATE_HZ),
             part->label, (int)sizeof(desc.version), desc.version);

    char body[160];
    snprintf(body, sizeof(body), "{\"ok\":true,\"size\":%u,\"part\":\"%s\",\"ver\":\"%.*s\"}",
             (unsigned)done, part->label, (int)sizeof(desc.version), desc.version);
    esp_err_t rc = send_json(req, "200 OK", body);

    // 回包之后才重启：浏览器得先读到这个响应才知道刷成功了
    vTaskDelay(pdMS_TO_TICKS(REBOOT_DELAY_MS));
    led_pwm_off();      // 重启前顺手把红外输出关掉，不停在原占空比上
    esp_restart();
    return rc;          // 走不到
}

void ota_report_boot(void)
{
    const esp_partition_t *p = esp_ota_get_running_partition();
    const esp_app_desc_t *d = esp_app_get_description();
#ifdef CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    const char *state = "?";
    esp_ota_img_states_t st = ESP_OTA_IMG_UNDEFINED;
    if (p && esp_ota_get_state_partition(p, &st) == ESP_OK) {
        switch (st) {
        case ESP_OTA_IMG_NEW:            state = "new"; break;
        case ESP_OTA_IMG_PENDING_VERIFY: state = "待确认"; break;
        case ESP_OTA_IMG_VALID:          state = "valid"; break;
        case ESP_OTA_IMG_INVALID:        state = "invalid"; break;
        case ESP_OTA_IMG_ABORTED:        state = "aborted（上次没确认，这次是从旧版本启动的）"; break;
        default: break;
        }
    }
    ESP_LOGI(TAG, "当前运行 %s v%s，状态 %s", p ? p->label : "-", d->version, state);
#else
    ESP_LOGI(TAG, "当前运行 %s v%s", p ? p->label : "-", d->version);
#endif
}

const char *ota_running_partition_label(void)
{
    const esp_partition_t *p = esp_ota_get_running_partition();
    return p ? p->label : "?";
}

void ota_confirm_boot(void)
{
#ifdef CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    // 主循环已经跑满一段时间、没崩，认下这次启动：以后再上电不会退回旧固件。
    // 启动就崩的固件走不到这里，下次上电由 bootloader 自动回退到另一个分区。
    ESP_LOGI(TAG, "确认本次启动：%s", esp_err_to_name(esp_ota_mark_app_valid_cancel_rollback()));
#endif
}

/*
 * 蓝牙 BLE 遥控
 */
#include "ble_link.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "esp_bt.h"
#include "esp_log.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "remote.h"

#define CMD_LEN             4
#define ADV_ITVL_MS         200     // 广播间隔，越大越省电，手机发现会稍慢

static const char *TAG = "ble";

// 128 位 UUID 按小端字节序书写：9e5d000X-3c1a-4b8e-9f2d-850e0a1c0001
#define UUID_850(x) BLE_UUID128_INIT(0x01, 0x00, 0x1c, 0x0a, 0x0e, 0x85, 0x2d, 0x9f, \
                                     0x8e, 0x4b, 0x1a, 0x3c, (x), 0x00, 0x5d, 0x9e)
static const ble_uuid128_t s_svc_uuid = UUID_850(0x01);
static const ble_uuid128_t s_cmd_uuid = UUID_850(0x02);
static const ble_uuid128_t s_sta_uuid = UUID_850(0x03);

static volatile bool s_running;
static volatile bool s_synced;
static volatile uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static volatile bool s_subscribed;
static volatile bool s_adv_wanted = true;
static uint8_t s_addr_type;
static uint16_t s_sta_handle;
static uint8_t s_status[BLE_LINK_STATUS_LEN];
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

static void adv_start(void);

static int chr_access(uint16_t conn, uint16_t attr, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR && attr == s_sta_handle) {
        uint8_t buf[BLE_LINK_STATUS_LEN];
        portENTER_CRITICAL(&s_lock);
        memcpy(buf, s_status, sizeof(buf));
        portEXIT_CRITICAL(&s_lock);
        return os_mbuf_append(ctxt->om, buf, sizeof(buf)) == 0 ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint8_t buf[CMD_LEN];
        uint16_t len = 0;
        if (OS_MBUF_PKTLEN(ctxt->om) != CMD_LEN ||
            ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len) != 0 || len != CMD_LEN) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        // 只入队，结果随下一次状态通知带回（seq + result）
        remote_ble_command(buf[0], buf[1], (uint16_t)(buf[2] | (buf[3] << 8)));
        return 0;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

static const struct ble_gatt_svc_def s_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &s_cmd_uuid.u,
                .access_cb = chr_access,
                .flags = BLE_GATT_CHR_F_WRITE,
            },
            {
                .uuid = &s_sta_uuid.u,
                .access_cb = chr_access,
                .flags = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &s_sta_handle,
            },
            {0},
        },
    },
    {0},
};

static int gap_event(struct ble_gap_event *ev, void *arg)
{
    switch (ev->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (ev->connect.status == 0) {
            s_conn = ev->connect.conn_handle;
            ESP_LOGI(TAG, "phone connected");
        } else {
            adv_start();
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_subscribed = false;
        ESP_LOGI(TAG, "phone disconnected (reason 0x%x)", ev->disconnect.reason);
        adv_start();
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (ev->subscribe.attr_handle == s_sta_handle) {
            s_subscribed = ev->subscribe.cur_notify;
        }
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        adv_start();
        break;
    default:
        break;
    }
    return 0;
}

// 广播包放 flags + 名称，扫描响应放 128 位服务 UUID（一起放会超过 31 字节）
static void adv_start(void)
{
    if (!s_synced || !s_adv_wanted || s_conn != BLE_HS_CONN_HANDLE_NONE || ble_gap_adv_active()) {
        return;
    }
    struct ble_hs_adv_fields f = {0};
    const char *name = ble_svc_gap_device_name();
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.name = (const uint8_t *)name;
    f.name_len = strlen(name);
    f.name_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&f);

    struct ble_hs_adv_fields r = {0};
    r.uuids128 = &s_svc_uuid;
    r.num_uuids128 = 1;
    r.uuids128_is_complete = 1;
    if (rc == 0) {
        rc = ble_gap_adv_rsp_set_fields(&r);
    }

    struct ble_gap_adv_params p = {
        .conn_mode = BLE_GAP_CONN_MODE_UND,
        .disc_mode = BLE_GAP_DISC_MODE_GEN,
        .itvl_min = BLE_GAP_ADV_ITVL_MS(ADV_ITVL_MS),
        .itvl_max = BLE_GAP_ADV_ITVL_MS(ADV_ITVL_MS),
    };
    if (rc == 0) {
        rc = ble_gap_adv_start(s_addr_type, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
    }
    if (rc != 0) {
        ESP_LOGW(TAG, "advertising failed: %d", rc);
    }
}

static void on_sync(void)
{
    if (ble_hs_util_ensure_addr(0) != 0 || ble_hs_id_infer_auto(0, &s_addr_type) != 0) {
        ESP_LOGE(TAG, "no BLE address");
        return;
    }
    s_synced = true;
    adv_start();
    ESP_LOGI(TAG, "advertising as \"%s\"", ble_svc_gap_device_name());
}

static void on_reset(int reason)
{
    s_synced = false;
    ESP_LOGW(TAG, "host reset, reason %d", reason);
}

static void host_task(void *arg)
{
    nimble_port_run();          // nimble_port_stop() 前不返回
    nimble_port_freertos_deinit();
}

esp_err_t ble_link_start(const char *name)
{
    esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble init failed: %s", esp_err_to_name(err));
        return err;
    }
    // 桌面几米内足够，降低发射峰值电流
    esp_ble_tx_power_set(ESP_BLE_PWR_TYPE_DEFAULT, ESP_PWR_LVL_N0);

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;

    ble_svc_gap_init();
    ble_svc_gatt_init();
    if (ble_gatts_count_cfg(s_svcs) != 0 || ble_gatts_add_svcs(s_svcs) != 0) {
        ESP_LOGE(TAG, "GATT service registration failed");
        return ESP_FAIL;
    }
    ble_svc_gap_device_name_set(name);

    nimble_port_freertos_init(host_task);
    s_running = true;
    return ESP_OK;
}

bool ble_link_running(void)
{
    return s_running;
}

bool ble_link_connected(void)
{
    return s_conn != BLE_HS_CONN_HANDLE_NONE;
}

void ble_link_set_adv(bool on)
{
    if (!s_running || on == s_adv_wanted) {
        return;
    }
    s_adv_wanted = on;
    if (on) {
        adv_start();
    } else if (ble_gap_adv_active()) {
        ble_gap_adv_stop();
    }
    ESP_LOGI(TAG, "advertising %s", on ? "on" : "off");
}

void ble_link_publish(const uint8_t status[BLE_LINK_STATUS_LEN])
{
    portENTER_CRITICAL(&s_lock);
    memcpy(s_status, status, BLE_LINK_STATUS_LEN);
    portEXIT_CRITICAL(&s_lock);

    uint16_t conn = s_conn;
    if (!s_running || !s_subscribed || conn == BLE_HS_CONN_HANDLE_NONE) {
        return;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(status, BLE_LINK_STATUS_LEN);
    if (om) {
        ble_gatts_notify_custom(conn, s_sta_handle, om);    // 失败时 om 由协议栈释放
    }
}

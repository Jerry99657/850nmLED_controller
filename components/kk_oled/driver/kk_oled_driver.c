/*
 * KK_OLED 驱动：ESP32-C3 + ST7735S 0.96 英寸 80x160 IPS TFT（8 引脚 SPI）
 *
 * KK_OLED 核心维护 1 bit 页式帧缓冲（80 列 x 20 页），本驱动把差异区域
 * 转成 RGB565（置位 = 前景色，清零 = 背景色）后通过 SPI 写入 ST7735S。
 *
 * 硬件：SPI2，SCLK=GPIO6，MOSI=GPIO7，CS=GPIO10，DC=GPIO3。
 * 屏幕 RESET 接系统 EN，复位由 SWRESET 完成；背光经 470 Ω 常亮，不可调。
 *
 * 驱动工作在面板原生竖屏方向，MADCTL 不交换行列，横屏由 KK_OLED 软件旋转。
 * 这样 CASET 最大只到 列偏移+79，在 132x162 与 128x160 两种控制器 RAM
 * 配置下都不会越界。
 */
#include "kk_oled_driver.h"
#include "kk_oled_internal.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "driver/spi_master.h"
#include "esp_attr.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_io.h"
#include "esp_log.h"
#include "sdkconfig.h"

#define TFT_SPI_HOST            SPI2_HOST
#define TFT_PIN_SCLK            6
#define TFT_PIN_MOSI            7
#define TFT_PIN_CS              10
#define TFT_PIN_DC              3
#define TFT_FLUSH_TIMEOUT_MS    100

// ST7735 命令
#define CMD_SWRESET     0x01
#define CMD_SLPOUT      0x11
#define CMD_NORON       0x13
#define CMD_INVOFF      0x20
#define CMD_INVON       0x21
#define CMD_DISPOFF     0x28
#define CMD_DISPON      0x29
#define CMD_CASET       0x2A
#define CMD_RASET       0x2B
#define CMD_RAMWR       0x2C
#define CMD_MADCTL      0x36
#define CMD_COLMOD      0x3A
#define CMD_FRMCTR1     0xB1
#define CMD_FRMCTR2     0xB2
#define CMD_FRMCTR3     0xB3
#define CMD_INVCTR      0xB4
#define CMD_PWCTR1      0xC0
#define CMD_PWCTR2      0xC1
#define CMD_PWCTR3      0xC2
#define CMD_PWCTR4      0xC3
#define CMD_PWCTR5      0xC4
#define CMD_VMCTR1      0xC5
#define CMD_GMCTRP1     0xE0
#define CMD_GMCTRN1     0xE1

#define MADCTL_BGR      0x08

// Kconfig 布尔项为 n 时不生成宏，这里统一成 0/1
#ifdef CONFIG_TFT_INVERT
#define TFT_INVERT      1
#else
#define TFT_INVERT      0
#endif
#ifdef CONFIG_TFT_BGR
#define TFT_BGR         1
#else
#define TFT_BGR         0
#endif

typedef struct {
    uint8_t cmd;
    uint8_t len;
    uint16_t delay_ms;
    uint8_t data[16];
} init_cmd_t;

// ST7735R/S 通用初始化序列（与 Adafruit / TFT_eSPI 的 Rcmd1 + Rcmd3 相同）
static const init_cmd_t s_init_cmds[] = {
    {CMD_SWRESET, 0, 150, {0}},
    {CMD_SLPOUT,  0, 255, {0}},
    {CMD_FRMCTR1, 3, 0, {0x01, 0x2C, 0x2D}},
    {CMD_FRMCTR2, 3, 0, {0x01, 0x2C, 0x2D}},
    {CMD_FRMCTR3, 6, 0, {0x01, 0x2C, 0x2D, 0x01, 0x2C, 0x2D}},
    {CMD_INVCTR,  1, 0, {0x07}},
    {CMD_PWCTR1,  3, 0, {0xA2, 0x02, 0x84}},
    {CMD_PWCTR2,  1, 0, {0xC5}},
    {CMD_PWCTR3,  2, 0, {0x0A, 0x00}},
    {CMD_PWCTR4,  2, 0, {0x8A, 0x2A}},
    {CMD_PWCTR5,  2, 0, {0x8A, 0xEE}},
    {CMD_VMCTR1,  1, 0, {0x0E}},
    {CMD_COLMOD,  1, 0, {0x05}},    // 16 位 RGB565
    {CMD_GMCTRP1, 16, 0, {0x02, 0x1C, 0x07, 0x12, 0x37, 0x32, 0x29, 0x2D,
                          0x29, 0x25, 0x2B, 0x39, 0x00, 0x01, 0x03, 0x10}},
    {CMD_GMCTRN1, 16, 0, {0x03, 0x1D, 0x07, 0x06, 0x2E, 0x2C, 0x29, 0x2D,
                          0x2E, 0x2E, 0x37, 0x3F, 0x00, 0x00, 0x02, 0x10}},
    {CMD_NORON,   0, 10, {0}},
};

static const char *TAG = "kk_oled_st7735";

static esp_lcd_panel_io_handle_t s_io;
static SemaphoreHandle_t s_done;
static bool s_bus_ready;
static uint16_t s_fg;   // 已转成大端
static uint16_t s_bg;

// RGB565 大端发送缓冲，最大为整屏
static DMA_ATTR uint16_t s_tx[OLED_PHYSICAL_WIDTH * OLED_PHYSICAL_HEIGHT];

static inline uint16_t swap16(uint16_t c)
{
    return (uint16_t)((c << 8) | (c >> 8));
}

static bool on_color_done(esp_lcd_panel_io_handle_t io, esp_lcd_panel_io_event_data_t *edata, void *ctx)
{
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_done, &woken);
    return woken == pdTRUE;
}

static esp_err_t tx_cmd(uint8_t cmd, const uint8_t *data, size_t len)
{
    return esp_lcd_panel_io_tx_param(s_io, cmd, len ? data : NULL, len);
}

static esp_err_t bus_init(void)
{
    const spi_bus_config_t bus = {
        .sclk_io_num = TFT_PIN_SCLK,
        .mosi_io_num = TFT_PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = sizeof(s_tx),
    };
    esp_err_t err = spi_bus_initialize(TFT_SPI_HOST, &bus, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        return err;
    }

    s_done = xSemaphoreCreateBinary();
    if (!s_done) {
        return ESP_ERR_NO_MEM;
    }

    const esp_lcd_panel_io_spi_config_t io_cfg = {
        .cs_gpio_num = TFT_PIN_CS,
        .dc_gpio_num = TFT_PIN_DC,
        .spi_mode = 0,
        .pclk_hz = CONFIG_TFT_SPI_CLOCK_MHZ * 1000 * 1000,
        .trans_queue_depth = 4,
        .on_color_trans_done = on_color_done,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
    };
    return esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)TFT_SPI_HOST, &io_cfg, &s_io);
}

// 写入面板坐标（竖屏，未加偏移）下的矩形，阻塞到 DMA 完成
static esp_err_t write_window(uint16_t x0, uint16_t y0, uint16_t x1, uint16_t y1,
                              const uint16_t *pixels)
{
    x0 += CONFIG_TFT_COL_OFFSET;
    x1 += CONFIG_TFT_COL_OFFSET;
    y0 += CONFIG_TFT_ROW_OFFSET;
    y1 += CONFIG_TFT_ROW_OFFSET;
    const uint8_t caset[4] = {x0 >> 8, x0 & 0xFF, x1 >> 8, x1 & 0xFF};
    const uint8_t raset[4] = {y0 >> 8, y0 & 0xFF, y1 >> 8, y1 & 0xFF};
    size_t bytes = (size_t)(x1 - x0 + 1) * (y1 - y0 + 1) * sizeof(uint16_t);

    esp_err_t err = tx_cmd(CMD_CASET, caset, sizeof(caset));
    if (err == ESP_OK) {
        err = tx_cmd(CMD_RASET, raset, sizeof(raset));
    }
    if (err == ESP_OK) {
        err = esp_lcd_panel_io_tx_color(s_io, CMD_RAMWR, pixels, bytes);
    }
    if (err == ESP_OK && xSemaphoreTake(s_done, pdMS_TO_TICKS(TFT_FLUSH_TIMEOUT_MS)) != pdTRUE) {
        err = ESP_ERR_TIMEOUT;
    }
    return err;
}

OLED_Status OLED_DriverInit(void)
{
    esp_err_t err = ESP_OK;

    if (!s_bus_ready) {
        err = bus_init();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "SPI init failed: %s", esp_err_to_name(err));
            return OLED_ERROR;
        }
        s_bus_ready = true;
    }
    s_fg = swap16(CONFIG_TFT_FG_COLOR);
    s_bg = swap16(CONFIG_TFT_BG_COLOR);

    for (size_t i = 0; i < sizeof(s_init_cmds) / sizeof(s_init_cmds[0]) && err == ESP_OK; i++) {
        const init_cmd_t *c = &s_init_cmds[i];
        err = tx_cmd(c->cmd, c->data, c->len);
        if (c->delay_ms) {
            vTaskDelay(pdMS_TO_TICKS(c->delay_ms));
        }
    }

    // 原生竖屏方向，不翻转、不交换行列
    const uint8_t madctl = TFT_BGR ? MADCTL_BGR : 0x00;
    if (err == ESP_OK) {
        err = tx_cmd(CMD_MADCTL, &madctl, 1);
    }
    if (err == ESP_OK) {
        err = tx_cmd(TFT_INVERT ? CMD_INVON : CMD_INVOFF, NULL, 0);
    }

    // 核心初始帧全为 0，屏幕也整屏填充背景色，保证后续差分基准一致
    if (err == ESP_OK) {
        for (size_t i = 0; i < sizeof(s_tx) / sizeof(s_tx[0]); i++) {
            s_tx[i] = s_bg;
        }
        err = write_window(0, 0, OLED_PHYSICAL_WIDTH - 1, OLED_PHYSICAL_HEIGHT - 1, s_tx);
    }
    if (err == ESP_OK) {
        err = tx_cmd(CMD_DISPON, NULL, 0);
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel init failed: %s", esp_err_to_name(err));
        return OLED_ERROR;
    }
    ESP_LOGI(TAG, "ST7735S %ux%u portrait, SPI %d MHz, offset col %d row %d, MADCTL 0x%02X, invert %d",
             OLED_PHYSICAL_WIDTH, OLED_PHYSICAL_HEIGHT, CONFIG_TFT_SPI_CLOCK_MHZ,
             CONFIG_TFT_COL_OFFSET, CONFIG_TFT_ROW_OFFSET, madctl, TFT_INVERT);
    return OLED_OK;
}

OLED_Status OLED_DriverWriteBlocking(void)
{
    const uint8_t *fb = OLED_InternalGetTransferBuffer();
    uint16_t x0 = OLED_PHYSICAL_WIDTH, x1 = 0;
    uint16_t p0 = OLED_PHYSICAL_PAGES, p1 = 0;

    // 各页差异列区间合并成一个矩形，一次窗口写入
    for (uint8_t page = 0; page < OLED_PHYSICAL_PAGES; page++) {
        uint8_t min_x = OLED_InternalGetTransferMinX(page);
        if (min_x >= OLED_PHYSICAL_WIDTH) {
            continue;
        }
        uint8_t max_x = OLED_InternalGetTransferMaxX(page);
        if (min_x < x0) {
            x0 = min_x;
        }
        if (max_x > x1) {
            x1 = max_x;
        }
        if (page < p0) {
            p0 = page;
        }
        p1 = page;
    }
    if (p0 >= OLED_PHYSICAL_PAGES) {
        return OLED_OK;
    }

    // 页式 1 bit → 行优先 RGB565
    const uint16_t y0 = p0 * 8U;
    const uint16_t y1 = p1 * 8U + 7U;
    uint16_t *out = s_tx;
    for (uint16_t y = y0; y <= y1; y++) {
        const uint8_t *row = &fb[(y >> 3) * OLED_PHYSICAL_WIDTH];
        const uint8_t mask = (uint8_t)(1U << (y & 7U));
        for (uint16_t x = x0; x <= x1; x++) {
            *out++ = (row[x] & mask) ? s_fg : s_bg;
        }
    }

    esp_err_t err = write_window(x0, y0, x1, y1, s_tx);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "flush failed: %s", esp_err_to_name(err));
        return OLED_ERROR;
    }
    return OLED_OK;
}

OLED_Status OLED_DriverWriteIT(void)
{
    return OLED_UNSUPPORTED;
}

OLED_Status OLED_DriverWriteDMA(void)
{
    return OLED_UNSUPPORTED;
}

bool OLED_DriverIsBusy(void)
{
    return false;
}

OLED_Status OLED_DriverSetContrast(uint8_t value)
{
    (void)value;
    return OLED_UNSUPPORTED;
}

OLED_Status OLED_DriverSetPowerSave(bool enable)
{
    return tx_cmd(enable ? CMD_DISPOFF : CMD_DISPON, NULL, 0) == ESP_OK ? OLED_OK : OLED_ERROR;
}

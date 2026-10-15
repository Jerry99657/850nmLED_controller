#ifndef KK_OLED_DRIVER_H
#define KK_OLED_DRIVER_H

#include "kk_oled.h"

#include <stdbool.h>
#include <stdint.h>

/*
 * ESP32-C3 + ST7735S 0.96 英寸 80x160 TFT。
 * 物理画布按面板原生竖屏 80x160 组织（20 页 x 8 行），
 * 应用通过 OLED_SetRotation(90/270) 得到 160x80 横屏逻辑画布。
 */
/** 面板原生宽度（竖屏）。 */
#define OLED_PHYSICAL_WIDTH 80U
/** 面板原生高度（竖屏）。 */
#define OLED_PHYSICAL_HEIGHT 160U
/** 每页 8 行像素，160 行共 20 页。 */
#define OLED_PHYSICAL_PAGES (OLED_PHYSICAL_HEIGHT / 8U)

/** 初始化 SPI 与 ST7735S，整屏填充背景色并点亮显示。 */
OLED_Status OLED_DriverInit(void);

/** 将核心标记的差异区域转换为 RGB565 并阻塞发送。 */
OLED_Status OLED_DriverWriteBlocking(void);

/** 未实现，返回 OLED_UNSUPPORTED。 */
OLED_Status OLED_DriverWriteIT(void);

/** 未实现，返回 OLED_UNSUPPORTED。 */
OLED_Status OLED_DriverWriteDMA(void);

/** 阻塞驱动始终返回 false。 */
bool OLED_DriverIsBusy(void);

/** TFT 背光由固定电阻供电，返回 OLED_UNSUPPORTED。 */
OLED_Status OLED_DriverSetContrast(uint8_t value);

/** 发送 DISPOFF / DISPON。 */
OLED_Status OLED_DriverSetPowerSave(bool enable);

#endif

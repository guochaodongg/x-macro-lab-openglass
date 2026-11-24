#ifndef LCD_ST7789_H
#define LCD_ST7789_H

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t lcd_st7789_init(void);
esp_err_t lcd_st7789_set_backlight(bool on);
esp_err_t lcd_st7789_sleep(void);

/* RGB565 input is big-endian (high byte first), one pixel every two bytes. */
esp_err_t lcd_st7789_fill(uint16_t rgb565);
esp_err_t lcd_st7789_draw_rgb565(int x, int y, int w, int h, const uint8_t *rgb565_be);

#ifdef __cplusplus
}
#endif

#endif

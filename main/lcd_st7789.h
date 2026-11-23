#ifndef LCD_ST7789_H
#define LCD_ST7789_H

#include <stdbool.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t lcd_st7789_init(void);
esp_err_t lcd_st7789_set_backlight(bool on);
esp_err_t lcd_st7789_sleep(void);

#ifdef __cplusplus
}
#endif

#endif

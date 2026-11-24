#ifndef PHOTO_DISPLAY_H
#define PHOTO_DISPLAY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PHOTO_LCD_WIDTH         240
#define PHOTO_LCD_HEIGHT        280
#define PHOTO_MAX_JPEG_BYTES    (2 * 1024 * 1024)
#define PHOTO_MAX_DECODE_DIM    480

esp_err_t photo_display_init(void);
esp_err_t photo_display_show_rgb565(const uint8_t *pixels, size_t len);
esp_err_t photo_display_show_jpeg(const uint8_t *jpeg, size_t len);
void photo_display_get_info(char *buf, size_t len);
esp_err_t photo_display_copy_jpeg(uint8_t **out_buf, size_t *out_len);
void photo_display_free_jpeg(uint8_t *buf);

#ifdef __cplusplus
}
#endif

#endif

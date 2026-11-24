#include "photo_display.h"

#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "jpeg_decoder.h"
#include "lcd_st7789.h"

#define PHOTO_STRIP_ROWS 32

static const char *TAG = "photo_display";

static SemaphoreHandle_t s_mutex = NULL;
static uint8_t *s_last_jpeg = NULL;
static size_t s_last_len = 0;
static uint16_t s_last_w = 0;
static uint16_t s_last_h = 0;

esp_err_t photo_display_init(void)
{
    if (s_mutex != NULL) {
        return ESP_OK;
    }
    s_mutex = xSemaphoreCreateMutex();
    return (s_mutex != NULL) ? ESP_OK : ESP_ERR_NO_MEM;
}

static esp_jpeg_image_scale_t select_scale(uint32_t width, uint32_t height)
{
    static const uint32_t divisors[4] = {1, 2, 4, 8};
    for (int i = 0; i < 4; i++) {
        if (width / divisors[i] <= PHOTO_MAX_DECODE_DIM &&
            height / divisors[i] <= PHOTO_MAX_DECODE_DIM) {
            return (esp_jpeg_image_scale_t)i;
        }
    }
    return JPEG_IMAGE_SCALE_1_8;
}

static inline uint8_t blend_channel(uint8_t a, uint8_t b, uint32_t frac)
{
    return (uint8_t)((a * (256 - frac) + b * frac + 128) >> 8);
}

/* Bilinear resampling of one destination row. RGB565 must be split into
 * channels before interpolation; interpolating packed bytes gives streaks. */
static void bilinear_row(const uint8_t *src, uint32_t src_w, uint32_t src_h,
                         uint32_t dst_y, uint32_t dst_w, uint32_t dst_h,
                         uint8_t *dst)
{
    int64_t sy_fp = (int64_t)((uint64_t)(dst_y * 2 + 1) * src_h * 256 /
                              (dst_h * 2)) - 128;
    if (sy_fp < 0) {
        sy_fp = 0;
    }
    uint32_t sy0 = (uint32_t)(sy_fp >> 8);
    if (sy0 >= src_h) {
        sy0 = src_h - 1;
    }
    uint32_t sy1 = (sy0 + 1 < src_h) ? sy0 + 1 : sy0;
    uint32_t fy = (uint32_t)(sy_fp & 0xFF);

    for (uint32_t x = 0; x < dst_w; x++) {
        int64_t sx_fp = (int64_t)((uint64_t)(x * 2 + 1) * src_w * 256 /
                                  (dst_w * 2)) - 128;
        if (sx_fp < 0) {
            sx_fp = 0;
        }
        uint32_t sx0 = (uint32_t)(sx_fp >> 8);
        if (sx0 >= src_w) {
            sx0 = src_w - 1;
        }
        uint32_t sx1 = (sx0 + 1 < src_w) ? sx0 + 1 : sx0;
        uint32_t fx = (uint32_t)(sx_fp & 0xFF);

        const uint8_t *p00 = src + ((size_t)sy0 * src_w + sx0) * 2;
        const uint8_t *p10 = src + ((size_t)sy0 * src_w + sx1) * 2;
        const uint8_t *p01 = src + ((size_t)sy1 * src_w + sx0) * 2;
        const uint8_t *p11 = src + ((size_t)sy1 * src_w + sx1) * 2;

        uint16_t c00 = (uint16_t)(((uint16_t)p00[0] << 8) | p00[1]);
        uint16_t c10 = (uint16_t)(((uint16_t)p10[0] << 8) | p10[1]);
        uint16_t c01 = (uint16_t)(((uint16_t)p01[0] << 8) | p01[1]);
        uint16_t c11 = (uint16_t)(((uint16_t)p11[0] << 8) | p11[1]);

        uint8_t r = blend_channel(
            blend_channel((uint8_t)(c00 >> 11), (uint8_t)(c10 >> 11), fx),
            blend_channel((uint8_t)(c01 >> 11), (uint8_t)(c11 >> 11), fx), fy);
        uint8_t g = blend_channel(
            blend_channel((uint8_t)((c00 >> 5) & 0x3F), (uint8_t)((c10 >> 5) & 0x3F), fx),
            blend_channel((uint8_t)((c01 >> 5) & 0x3F), (uint8_t)((c11 >> 5) & 0x3F), fx), fy);
        uint8_t b = blend_channel(
            blend_channel((uint8_t)(c00 & 0x1F), (uint8_t)(c10 & 0x1F), fx),
            blend_channel((uint8_t)(c01 & 0x1F), (uint8_t)(c11 & 0x1F), fx), fy);

        uint16_t out = (uint16_t)(((uint16_t)r << 11) | ((uint16_t)g << 5) | b);
        dst[x * 2] = (uint8_t)(out >> 8);
        dst[x * 2 + 1] = (uint8_t)(out & 0xFF);
    }
}

esp_err_t photo_display_show_rgb565(const uint8_t *pixels, size_t len)
{
    const size_t expected = (size_t)PHOTO_LCD_WIDTH * PHOTO_LCD_HEIGHT * 2;
    if (pixels == NULL || len != expected || s_mutex == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    esp_err_t err = lcd_st7789_draw_rgb565(0, 0, PHOTO_LCD_WIDTH, PHOTO_LCD_HEIGHT, pixels);
    if (err == ESP_OK) {
        uint8_t *saved = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
        if (saved != NULL) {
            memcpy(saved, pixels, len);
            if (s_last_jpeg != NULL) {
                heap_caps_free(s_last_jpeg);
            }
            s_last_jpeg = saved;
            s_last_len = len;
            s_last_w = PHOTO_LCD_WIDTH;
            s_last_h = PHOTO_LCD_HEIGHT;
        }
        ESP_LOGI(TAG, "showing RGB565 %dx%d (%u bytes)",
                 PHOTO_LCD_WIDTH, PHOTO_LCD_HEIGHT, (unsigned)len);
    }

    xSemaphoreGive(s_mutex);
    return err;
}

esp_err_t photo_display_show_jpeg(const uint8_t *jpeg, size_t len)
{
    if (jpeg == NULL || len < 4 || jpeg[0] != 0xFF || jpeg[1] != 0xD8) {
        return ESP_ERR_INVALID_ARG;
    }
    if (len > PHOTO_MAX_JPEG_BYTES) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_mutex == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    esp_err_t err = ESP_OK;
    uint8_t *decoded = NULL;
    uint8_t *chunk = NULL;

    esp_jpeg_image_cfg_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.indata = (uint8_t *)jpeg;
    cfg.indata_size = (uint32_t)len;
    cfg.out_format = JPEG_IMAGE_FORMAT_RGB565;
    cfg.out_scale = JPEG_IMAGE_SCALE_0;

    esp_jpeg_image_output_t info;
    err = esp_jpeg_get_image_info(&cfg, &info);
    if (err != ESP_OK || info.width == 0 || info.height == 0) {
        ESP_LOGE(TAG, "invalid JPEG header: %s", esp_err_to_name(err));
        goto done;
    }

    cfg.out_scale = select_scale(info.width, info.height);
    err = esp_jpeg_get_image_info(&cfg, &info);
    if (err != ESP_OK || info.output_len == 0) {
        err = ESP_FAIL;
        goto done;
    }

    decoded = heap_caps_malloc(info.output_len, MALLOC_CAP_SPIRAM);
    if (decoded == NULL) {
        decoded = heap_caps_malloc(info.output_len, MALLOC_CAP_8BIT);
    }
    if (decoded == NULL) {
        ESP_LOGE(TAG, "no memory for decoded image (%u bytes)", (unsigned)info.output_len);
        err = ESP_ERR_NO_MEM;
        goto done;
    }

    cfg.outbuf = decoded;
    cfg.outbuf_size = info.output_len;
    cfg.flags.swap_color_bytes = 1;  /* big-endian RGB565 for the LCD helper */

    esp_jpeg_image_output_t output;
    err = esp_jpeg_decode(&cfg, &output);
    if (err != ESP_OK || output.width == 0 || output.height == 0) {
        ESP_LOGE(TAG, "JPEG decode failed: %s", esp_err_to_name(err));
        err = (err == ESP_OK) ? ESP_FAIL : err;
        goto done;
    }

    /* Fit the decoded image into the panel, keep aspect ratio, black letterbox. */
    uint32_t scaled_w, scaled_h;
    if ((uint64_t)output.width * PHOTO_LCD_HEIGHT >
        (uint64_t)output.height * PHOTO_LCD_WIDTH) {
        scaled_w = PHOTO_LCD_WIDTH;
        scaled_h = (uint32_t)output.height * PHOTO_LCD_WIDTH / output.width;
    } else {
        scaled_h = PHOTO_LCD_HEIGHT;
        scaled_w = (uint32_t)output.width * PHOTO_LCD_HEIGHT / output.height;
    }
    if (scaled_w < 1) {
        scaled_w = 1;
    }
    if (scaled_h < 1) {
        scaled_h = 1;
    }
    int offset_x = (PHOTO_LCD_WIDTH - (int)scaled_w) / 2;
    int offset_y = (PHOTO_LCD_HEIGHT - (int)scaled_h) / 2;

    err = lcd_st7789_fill(0x0000);
    if (err != ESP_OK) {
        goto done;
    }

    size_t chunk_bytes = (size_t)scaled_w * PHOTO_STRIP_ROWS * 2;
    chunk = heap_caps_malloc(chunk_bytes, MALLOC_CAP_8BIT);
    if (chunk == NULL) {
        err = ESP_ERR_NO_MEM;
        goto done;
    }

    bool identity = (output.width == scaled_w && output.height == scaled_h);
    for (uint32_t y0 = 0; y0 < scaled_h && err == ESP_OK; y0 += PHOTO_STRIP_ROWS) {
        uint32_t rows = (y0 + PHOTO_STRIP_ROWS <= scaled_h) ? PHOTO_STRIP_ROWS
                                                            : scaled_h - y0;
        for (uint32_t row = 0; row < rows; row++) {
            uint8_t *dst = chunk + (size_t)row * scaled_w * 2;
            uint32_t y = y0 + row;
            if (identity) {
                const uint8_t *src = decoded + (size_t)y * output.width * 2;
                memcpy(dst, src, (size_t)scaled_w * 2);
            } else {
                bilinear_row(decoded, output.width, output.height,
                             y, scaled_w, scaled_h, dst);
            }
        }
        err = lcd_st7789_draw_rgb565(offset_x, offset_y + (int)y0,
                                     (int)scaled_w, (int)rows, chunk);
    }

    uint8_t *saved = heap_caps_malloc(len, MALLOC_CAP_SPIRAM);
    if (saved != NULL) {
        memcpy(saved, jpeg, len);
        if (s_last_jpeg != NULL) {
            heap_caps_free(s_last_jpeg);
        }
        s_last_jpeg = saved;
        s_last_len = len;
        s_last_w = output.width;
        s_last_h = output.height;
    } else {
        ESP_LOGW(TAG, "displayed image but could not keep a copy for /photo");
    }

    ESP_LOGI(TAG, "showing %ux%u (decoded %ux%u, %u bytes)",
             (unsigned)scaled_w, (unsigned)scaled_h,
             (unsigned)output.width, (unsigned)output.height, (unsigned)len);

done:
    if (chunk != NULL) {
        heap_caps_free(chunk);
    }
    if (decoded != NULL) {
        heap_caps_free(decoded);
    }
    xSemaphoreGive(s_mutex);
    return err;
}

void photo_display_get_info(char *buf, size_t len)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    if (s_last_len == 0) {
        snprintf(buf, len, "尚无照片");
    } else {
        snprintf(buf, len, "%ux%u, %u B", (unsigned)s_last_w,
                 (unsigned)s_last_h, (unsigned)s_last_len);
    }
    xSemaphoreGive(s_mutex);
}

esp_err_t photo_display_copy_jpeg(uint8_t **out_buf, size_t *out_len)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    uint8_t *copy = NULL;

    if (s_last_jpeg == NULL || s_last_len == 0) {
        err = ESP_ERR_NOT_FOUND;
    } else {
        copy = heap_caps_malloc(s_last_len, MALLOC_CAP_SPIRAM);
        if (copy == NULL) {
            err = ESP_ERR_NO_MEM;
        } else {
            memcpy(copy, s_last_jpeg, s_last_len);
            *out_len = s_last_len;
        }
    }

    xSemaphoreGive(s_mutex);
    if (err == ESP_OK) {
        *out_buf = copy;
    }
    return err;
}

void photo_display_free_jpeg(uint8_t *buf)
{
    if (buf != NULL) {
        heap_caps_free(buf);
    }
}

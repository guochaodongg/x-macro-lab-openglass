#include "lcd_st7789.h"

#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "spider_image.h"

/* XIAO ESP32S3 D8/D9/D10 are the default SPI pins; D6/D7/D0/D1 avoid camera pins. */
#define LCD_SPI_HOST        SPI2_HOST
#define LCD_PIN_SCLK        GPIO_NUM_7    /* D8  */
#define LCD_PIN_MOSI        GPIO_NUM_9    /* D10 */
#define LCD_PIN_CS          GPIO_NUM_43   /* D6  */
#define LCD_PIN_DC          GPIO_NUM_44   /* D7  */
#define LCD_PIN_RST         GPIO_NUM_1    /* D0  */
#define LCD_PIN_BLK         GPIO_NUM_2    /* D1  */

#define LCD_WIDTH           240
#define LCD_HEIGHT          280
#define LCD_X_OFFSET        0
#define LCD_Y_OFFSET        20
#define LCD_PIXEL_CLOCK_HZ  20000000
#define LCD_SPI_MODE        3
#define LCD_INVERT_COLOR    true
#define LCD_RGB_ORDER       LCD_RGB_ELEMENT_ORDER_BGR

#define LCD_BLK_TIMER       LEDC_TIMER_1
#define LCD_BLK_CHANNEL     LEDC_CHANNEL_1
#define LCD_BLK_DUTY_FULL   1023

#define LCD_BAR_HEIGHT      32

#define COLOR_BLACK         0x0000
#define COLOR_WHITE         0xFFFF

static const char *TAG = "lcd_st7789";

static esp_lcd_panel_io_handle_t s_lcd_io = NULL;
static esp_lcd_panel_handle_t s_lcd_panel = NULL;
static bool s_lcd_initialized = false;

static esp_err_t draw_spider_image(void)
{
    const size_t row_bytes = SPIDER_IMAGE_WIDTH / 8;
    const size_t chunk_pixels = (size_t)SPIDER_IMAGE_WIDTH * LCD_BAR_HEIGHT;
    uint8_t *line = heap_caps_malloc(chunk_pixels * 2, MALLOC_CAP_DMA);
    if (line == NULL) {
        return ESP_ERR_NO_MEM;
    }

    esp_err_t err = ESP_OK;
    for (int y0 = 0; y0 < LCD_HEIGHT && err == ESP_OK; y0 += LCD_BAR_HEIGHT) {
        int h = LCD_BAR_HEIGHT;
        if (y0 + h > LCD_HEIGHT) {
            h = LCD_HEIGHT - y0;
        }

        memset(line, 0xFF, chunk_pixels * 2);
        for (int y = y0; y < y0 + h; y++) {
            for (int x = 0; x < SPIDER_IMAGE_WIDTH; x++) {
                uint8_t byte = spider_image_bits[(size_t)y * row_bytes + (x >> 3)];
                uint16_t color = (byte & (0x80 >> (x & 7))) ? COLOR_BLACK : COLOR_WHITE;
                uint8_t *p = &line[((y - y0) * SPIDER_IMAGE_WIDTH + x) * 2];
                p[0] = (uint8_t)(color >> 8);
                p[1] = (uint8_t)(color & 0xFF);
            }
        }

        err = esp_lcd_panel_draw_bitmap(s_lcd_panel, 0, y0, SPIDER_IMAGE_WIDTH, y0 + h, line);
    }

    heap_caps_free(line);
    return err;
}

esp_err_t lcd_st7789_set_backlight(bool on)
{
    if (!s_lcd_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    ledc_set_duty(LEDC_LOW_SPEED_MODE, LCD_BLK_CHANNEL, on ? LCD_BLK_DUTY_FULL : 0);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, LCD_BLK_CHANNEL);
    return ESP_OK;
}

esp_err_t lcd_st7789_sleep(void)
{
    if (!s_lcd_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = esp_lcd_panel_disp_on_off(s_lcd_panel, false);
    if (err != ESP_OK) {
        return err;
    }
    return lcd_st7789_set_backlight(false);
}

esp_err_t lcd_st7789_init(void)
{
    esp_err_t err;

    spi_bus_config_t bus_config = {
        .sclk_io_num = LCD_PIN_SCLK,
        .mosi_io_num = LCD_PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_WIDTH * LCD_BAR_HEIGHT * 2,
    };
    err = spi_bus_initialize(LCD_SPI_HOST, &bus_config, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return err;
    }

    ledc_timer_config_t blk_timer = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .timer_num = LCD_BLK_TIMER,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    err = ledc_timer_config(&blk_timer);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc_timer_config failed: %s", esp_err_to_name(err));
        return err;
    }

    ledc_channel_config_t blk_channel = {
        .gpio_num = LCD_PIN_BLK,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = LCD_BLK_CHANNEL,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = LCD_BLK_TIMER,
        .duty = 0,
        .hpoint = 0,
    };
    err = ledc_channel_config(&blk_channel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ledc_channel_config failed: %s", esp_err_to_name(err));
        return err;
    }
    s_lcd_initialized = true;

    esp_lcd_panel_io_spi_config_t io_config = {
        .dc_gpio_num = LCD_PIN_DC,
        .cs_gpio_num = LCD_PIN_CS,
        .spi_mode = LCD_SPI_MODE,
        .pclk_hz = LCD_PIXEL_CLOCK_HZ,
        .lcd_cmd_bits = 8,
        .lcd_param_bits = 8,
        .trans_queue_depth = 8,
    };
    err = esp_lcd_new_panel_io_spi(LCD_SPI_HOST, &io_config, &s_lcd_io);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_io_spi failed: %s", esp_err_to_name(err));
        return err;
    }

    esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = LCD_PIN_RST,
        .rgb_ele_order = LCD_RGB_ORDER,
        .bits_per_pixel = 16,
    };
    err = esp_lcd_new_panel_st7789(s_lcd_io, &panel_config, &s_lcd_panel);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_lcd_new_panel_st7789 failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_lcd_panel_reset(s_lcd_panel);
    if (err == ESP_OK) {
        err = esp_lcd_panel_init(s_lcd_panel);
    }
    if (err == ESP_OK) {
        err = esp_lcd_panel_set_gap(s_lcd_panel, LCD_X_OFFSET, LCD_Y_OFFSET);
    }
    if (err == ESP_OK) {
        err = esp_lcd_panel_invert_color(s_lcd_panel, LCD_INVERT_COLOR);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "panel init failed: %s", esp_err_to_name(err));
        return err;
    }

    err = draw_spider_image();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "draw_spider_image failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_lcd_panel_disp_on_off(s_lcd_panel, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "disp_on failed: %s", esp_err_to_name(err));
        return err;
    }

    err = lcd_st7789_set_backlight(true);
    if (err != ESP_OK) {
        return err;
    }

    ESP_LOGI(TAG, "ST7789 ready: %dx%d, SPI%d, %u Hz, y_offset=%d",
             LCD_WIDTH, LCD_HEIGHT, LCD_SPI_MODE, LCD_PIXEL_CLOCK_HZ, LCD_Y_OFFSET);
    return ESP_OK;
}

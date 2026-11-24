#include "lcd_st7789.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_st7789.h"
#include "halloween_images.h"

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
#define LCD_IMAGE_Y_OFFSET  20
#define LCD_PIXEL_CLOCK_HZ  20000000
#define LCD_SPI_MODE        3
#define LCD_INVERT_COLOR    true
#define LCD_RGB_ORDER       LCD_RGB_ELEMENT_ORDER_BGR
#define LCD_CAROUSEL_PERIOD_MS 2000

#define LCD_BLK_TIMER       LEDC_TIMER_1
#define LCD_BLK_CHANNEL     LEDC_CHANNEL_1
#define LCD_BLK_DUTY_FULL   1023

#define LCD_DMA_CHUNK_HEIGHT 32

static const char *TAG = "lcd_st7789";

static esp_lcd_panel_io_handle_t s_lcd_io = NULL;
static esp_lcd_panel_handle_t s_lcd_panel = NULL;
static uint8_t *s_frame_buffer = NULL;
static int s_current_frame = 0;
static bool s_lcd_initialized = false;

static inline void put_color(uint8_t *buf, int index, uint16_t color)
{
    buf[index * 2 + 0] = (uint8_t)(color >> 8);
    buf[index * 2 + 1] = (uint8_t)(color & 0xFF);
}

static esp_err_t draw_frame(int frame)
{
    if (s_lcd_panel == NULL || s_frame_buffer == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (frame < 0 || frame >= HALLOWEEN_FRAME_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }

    const uint16_t bg_color = halloween_palette[0];
    const uint8_t *image = halloween_frames[frame];

    for (int y = 0; y < LCD_HEIGHT; y++) {
        int img_y = y - LCD_IMAGE_Y_OFFSET;
        for (int x = 0; x < HALLOWEEN_FRAME_WIDTH; x++) {
            uint16_t color = bg_color;
            if (img_y >= 0 && img_y < HALLOWEEN_FRAME_HEIGHT) {
                uint8_t packed = image[img_y * (HALLOWEEN_FRAME_WIDTH / 2) + (x >> 1)];
                int idx = (x & 1) ? (packed & 0x0F) : (packed >> 4);
                color = halloween_palette[idx];
            }
            put_color(s_frame_buffer, y * HALLOWEEN_FRAME_WIDTH + x, color);
        }
    }

    return esp_lcd_panel_draw_bitmap(s_lcd_panel, 0, 0,
                                     HALLOWEEN_FRAME_WIDTH, LCD_HEIGHT,
                                     s_frame_buffer);
}

static void carousel_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(LCD_CAROUSEL_PERIOD_MS));
        s_current_frame = (s_current_frame + 1) % HALLOWEEN_FRAME_COUNT;
        draw_frame(s_current_frame);
    }
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

    const size_t frame_size = (size_t)LCD_WIDTH * LCD_HEIGHT * 2;
    s_frame_buffer = heap_caps_malloc(frame_size, MALLOC_CAP_DMA);
    if (s_frame_buffer == NULL) {
        s_frame_buffer = heap_caps_malloc(frame_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (s_frame_buffer == NULL) {
        ESP_LOGE(TAG, "alloc frame buffer failed");
        return ESP_ERR_NO_MEM;
    }

    spi_bus_config_t bus_config = {
        .sclk_io_num = LCD_PIN_SCLK,
        .mosi_io_num = LCD_PIN_MOSI,
        .miso_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = LCD_WIDTH * LCD_DMA_CHUNK_HEIGHT * 2,
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

    err = draw_frame(s_current_frame);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "draw_frame failed: %s", esp_err_to_name(err));
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

    xTaskCreate(carousel_task, "lcd_carousel", 2048, NULL, 5, NULL);

    ESP_LOGI(TAG, "ST7789 ready: %dx%d, %d frames, %u ms interval",
             LCD_WIDTH, LCD_HEIGHT, HALLOWEEN_FRAME_COUNT, LCD_CAROUSEL_PERIOD_MS);
    return ESP_OK;
}

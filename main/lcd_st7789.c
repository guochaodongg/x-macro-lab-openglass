#include "lcd_st7789.h"

#include <string.h>

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "driver/ledc.h"
#include "esp_lcd_io_spi.h"
#include "esp_lcd_panel_io.h"
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
#define LCD_PIXEL_CLOCK_HZ  5000000
#define LCD_SPI_MODE        0
#define LCD_INVERT_COLOR    true
#define LCD_RGB_ORDER       LCD_RGB_ELEMENT_ORDER_BGR

#define LCD_BLK_TIMER       LEDC_TIMER_1
#define LCD_BLK_CHANNEL     LEDC_CHANNEL_1
#define LCD_BLK_DUTY_FULL   1023

#define LCD_BAR_HEIGHT      32

#define COLOR_BLACK         0x0000
#define COLOR_WHITE         0xFFFF

/* The panel is configured as BGR; standard RGB565 must be converted before sending. */
#define LCD_SWAP_RB         1

static const char *TAG = "lcd_st7789";

static esp_lcd_panel_io_handle_t s_lcd_io = NULL;
static esp_lcd_panel_handle_t s_lcd_panel = NULL;
static bool s_lcd_initialized = false;

/* One statically allocated DMA strip, reused by all draws. No heap spikes. */
static DRAM_ATTR uint8_t s_dma_strip[LCD_WIDTH * LCD_BAR_HEIGHT * 2]
    __attribute__((aligned(4)));

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

static inline uint16_t lcd_color_to_panel(uint16_t rgb565)
{
#if LCD_SWAP_RB
    return (uint16_t)(((rgb565 & 0x001F) << 11) |
                      (rgb565 & 0x07E0) |
                      ((rgb565 >> 11) & 0x001F));
#else
    return rgb565;
#endif
}

esp_err_t lcd_st7789_fill(uint16_t rgb565)
{
    if (!s_lcd_initialized) {
        return ESP_ERR_INVALID_STATE;
    }

    const size_t chunk_pixels = (size_t)LCD_WIDTH * LCD_BAR_HEIGHT;
    uint16_t panel_color = lcd_color_to_panel(rgb565);
    for (size_t i = 0; i < chunk_pixels; i++) {
        s_dma_strip[i * 2] = (uint8_t)(panel_color >> 8);
        s_dma_strip[i * 2 + 1] = (uint8_t)(panel_color & 0xFF);
    }

    esp_err_t err = ESP_OK;
    for (int y0 = 0; y0 < LCD_HEIGHT && err == ESP_OK; y0 += LCD_BAR_HEIGHT) {
        int h = (y0 + LCD_BAR_HEIGHT <= LCD_HEIGHT) ? LCD_BAR_HEIGHT : LCD_HEIGHT - y0;
        err = esp_lcd_panel_draw_bitmap(s_lcd_panel, 0, y0, LCD_WIDTH, y0 + h, s_dma_strip);
    }

    return err;
}

esp_err_t lcd_st7789_draw_rgb565(int x, int y, int w, int h, const uint8_t *rgb565_be)
{
    if (!s_lcd_initialized || rgb565_be == NULL || w <= 0 || h <= 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (x < 0 || y < 0 || x + w > LCD_WIDTH || y + h > LCD_HEIGHT) {
        return ESP_ERR_INVALID_ARG;
    }

    esp_err_t err = ESP_OK;
    for (int y0 = 0; y0 < h && err == ESP_OK; y0 += LCD_BAR_HEIGHT) {
        int rows = (y0 + LCD_BAR_HEIGHT <= h) ? LCD_BAR_HEIGHT : h - y0;
        for (int row = 0; row < rows; row++) {
            const uint8_t *src = rgb565_be + (size_t)(y0 + row) * w * 2;
            uint8_t *dst = s_dma_strip + (size_t)row * w * 2;
            for (int col = 0; col < w; col++, src += 2, dst += 2) {
                uint16_t color = (uint16_t)(((uint16_t)src[0] << 8) | src[1]);
                color = lcd_color_to_panel(color);
                dst[0] = (uint8_t)(color >> 8);
                dst[1] = (uint8_t)(color & 0xFF);
            }
        }
        err = esp_lcd_panel_draw_bitmap(s_lcd_panel, x, y + y0, x + w, y + y0 + rows,
                                        s_dma_strip);
    }

    return err;
}

/* The ST7789 drives a 240x320 panel but we only use a 280-row window shifted
 * by LCD_Y_OFFSET. Clear the whole physical panel before applying the gap so
 * the unused top/bottom rows cannot show stale or overlapping pixels. */
static esp_err_t clear_whole_panel(void)
{
    const size_t strip_pixels = (size_t)LCD_WIDTH * LCD_BAR_HEIGHT;
    uint16_t black = lcd_color_to_panel(COLOR_BLACK);
    for (size_t i = 0; i < strip_pixels; i++) {
        s_dma_strip[i * 2] = (uint8_t)(black >> 8);
        s_dma_strip[i * 2 + 1] = (uint8_t)(black & 0xFF);
    }

    const int panel_height = 320;
    esp_err_t err = ESP_OK;
    for (int y0 = 0; y0 < panel_height && err == ESP_OK; y0 += LCD_BAR_HEIGHT) {
        int h = (y0 + LCD_BAR_HEIGHT <= panel_height) ? LCD_BAR_HEIGHT : panel_height - y0;
        err = esp_lcd_panel_draw_bitmap(s_lcd_panel, 0, y0, LCD_WIDTH, y0 + h, s_dma_strip);
    }
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
    /* CPU-driven transfers: on long flex cables the DMA engine can latch
     * corrupted bytes halfway through a strip, which shows up as regular
     * horizontal noise lines on one side of the panel. Polling mode is slower
     * but reliable; a full frame still takes well under a second at 5 MHz. */
    err = spi_bus_initialize(LCD_SPI_HOST, &bus_config, SPI_DMA_DISABLED);
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
        err = clear_whole_panel();
    }
    if (err == ESP_OK) {
        /* ST7789 RAM is 240x320 but the glass is only 280 rows. VSCRDEF tells
         * the controller to scan exactly 280 rows (TFA=20, VSA=280, BFA=20) and
         * VSCSAD stays 0, so the visible area shows RAM rows 0..279. Therefore
         * no extra y gap is applied when writing pixels; adding one here would
         * shift the image by 20 rows and duplicate the bottom. */
        const uint8_t vscrdef[] = {
            0x00, 0x14,   /* TFA = 20  */
            0x01, 0x18,   /* VSA = 280 */
            0x00, 0x14,   /* BFA = 20  */
        };
        err = esp_lcd_panel_io_tx_param(s_lcd_io, 0x33, vscrdef, sizeof(vscrdef));
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

    ESP_LOGI(TAG, "ST7789 ready: %dx%d, SPI mode %d, %u Hz, VSCRDEF 20/280/20",
             LCD_WIDTH, LCD_HEIGHT, LCD_SPI_MODE, LCD_PIXEL_CLOCK_HZ);
    return ESP_OK;
}

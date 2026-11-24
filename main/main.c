/*
 * Seeed Studio XIAO ESP32S3 - LED / BOOT key / ST7789 LCD demo
 * ------------------------------------------------------------
 * Hardware:
 *   - User LED : GPIO21, ORANGE, ACTIVE LOW (0 = on, 1 = off)
 *   - BOOT key : GPIO0,  ACTIVE LOW, RTC GPIO, EXT0 wake source
 *   - LCD      : ST7789 1.69" 240x280 SPI, shows a spider image on boot
 *
 * Behaviour:
 *   - Power on / reset          -> LED blinks at 1 Hz, LCD shows image
 *   - Short press of BOOT       -> enter deep sleep (LED/LCD off)
 *   - Short press of BOOT again -> wake from deep sleep and resume
 *   - Web UI                    -> upload a photo over Wi-Fi, display it on LCD
 */
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_sleep.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "lcd_st7789.h"
#include "photo_display.h"
#include "photo_httpd.h"
#include "photo_wifi.h"

#define USER_LED_PIN       GPIO_NUM_21
#define BOOT_KEY_PIN       GPIO_NUM_0
#define KEY_ACTIVE_LEVEL   0
#define KEY_DEBOUNCE_MS    50
#define KEY_POLL_MS        10

static const char *TAG = "app";
RTC_DATA_ATTR static uint32_t s_boot_count = 0;

static void boot_key_init(void)
{
    gpio_reset_pin(BOOT_KEY_PIN);
    gpio_set_direction(BOOT_KEY_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BOOT_KEY_PIN, GPIO_PULLUP_ONLY);
    gpio_pulldown_dis(BOOT_KEY_PIN);
    gpio_pullup_en(BOOT_KEY_PIN);
}

static void wait_for_key_release(void)
{
    while (gpio_get_level(BOOT_KEY_PIN) == KEY_ACTIVE_LEVEL) {
        vTaskDelay(pdMS_TO_TICKS(KEY_POLL_MS));
    }
    vTaskDelay(pdMS_TO_TICKS(KEY_DEBOUNCE_MS));
}

static void power_off(void)
{
    ESP_LOGI(TAG, "BOOT pressed -> entering deep sleep");

    gpio_set_level(USER_LED_PIN, 1);
    lcd_st7789_sleep();

    ESP_ERROR_CHECK(esp_sleep_enable_ext0_wakeup(BOOT_KEY_PIN, KEY_ACTIVE_LEVEL));
    vTaskDelay(pdMS_TO_TICKS(100));
    esp_deep_sleep_start();
}

static void led_task(void *arg)
{
    while (1) {
        gpio_set_level(USER_LED_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(500));
        gpio_set_level(USER_LED_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

static void power_key_task(void *arg)
{
    int last_level = 1;

    wait_for_key_release();

    while (1) {
        int level = gpio_get_level(BOOT_KEY_PIN);
        if (level != last_level) {
            vTaskDelay(pdMS_TO_TICKS(KEY_DEBOUNCE_MS));
            int stable = gpio_get_level(BOOT_KEY_PIN);
            if (stable == level) {
                last_level = stable;
                if (stable == KEY_ACTIVE_LEVEL) {
                    wait_for_key_release();
                    power_off();
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(KEY_POLL_MS));
    }
}

void app_main(void)
{
    s_boot_count++;
    ESP_LOGI(TAG, "boot #%lu", (unsigned long)s_boot_count);

    gpio_reset_pin(USER_LED_PIN);
    gpio_set_direction(USER_LED_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(USER_LED_PIN, 1);

    boot_key_init();

    esp_err_t lcd_err = lcd_st7789_init();
    if (lcd_err == ESP_OK) {
        ESP_LOGI(TAG, "LCD ready");
    } else {
        ESP_LOGE(TAG, "LCD init failed: %s", esp_err_to_name(lcd_err));
    }

    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES ||
        nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_ERROR_CHECK(photo_display_init());
    ESP_ERROR_CHECK(photo_wifi_init());
    ESP_ERROR_CHECK(photo_httpd_start());

    xTaskCreate(led_task, "led", 2048, NULL, 5, NULL);
    xTaskCreate(power_key_task, "power_key", 2048, NULL, 5, NULL);
}

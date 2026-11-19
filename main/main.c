/*
 * Seeed Studio XIAO ESP32S3 - BOOT key power on/off demo (ESP-IDF v6.1)
 * ---------------------------------------------------------------------
 * Hardware:
 *   - User LED : GPIO21, ORANGE, ACTIVE LOW (0 = on, 1 = off)
 *   - BOOT key : GPIO0,  ACTIVE LOW (internal pull-up enabled)
 *
 * Behaviour:
 *   - Power on / reset           -> normal operation (LED blinks at 1 Hz)
 *   - Short press of BOOT        -> enter deep sleep (LED off, ~uA current)
 *   - Short press of BOOT again  -> wake from deep sleep, resume operation
 *
 * GPIO0 is an RTC GPIO, so it can be used as an EXT0 wake-up source while
 * the chip is in deep sleep.
 *
 * Build / flash:
 *   idf.py set-target esp32s3
 *   idf.py build
 *   idf.py -p COMx flash monitor
 */
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_sleep.h"

#define USER_LED_PIN       GPIO_NUM_21
#define BOOT_KEY_PIN       GPIO_NUM_0
#define KEY_ACTIVE_LEVEL   0
#define KEY_DEBOUNCE_MS    50
#define KEY_POLL_MS        10

/* Set to e.g. 1000 to require a 1 s hold before powering off (0 = any tap). */
#define POWER_KEY_HOLD_MS  0

static void boot_key_init(void)
{
    gpio_reset_pin(BOOT_KEY_PIN);
    gpio_set_direction(BOOT_KEY_PIN, GPIO_MODE_INPUT);
    gpio_set_pull_mode(BOOT_KEY_PIN, GPIO_PULLUP_ONLY);

    /* EXT0 wake-up uses the RTC GPIO pad: keep the pull-up enabled there too. */
    gpio_pulldown_dis(BOOT_KEY_PIN);
    gpio_pullup_en(BOOT_KEY_PIN);
}

static void wait_for_key_release(void)
{
    /* Ignore a key that is still held (e.g. the very press that woke us). */
    while (gpio_get_level(BOOT_KEY_PIN) == KEY_ACTIVE_LEVEL) {
        vTaskDelay(pdMS_TO_TICKS(KEY_POLL_MS));
    }
    vTaskDelay(pdMS_TO_TICKS(KEY_DEBOUNCE_MS));
}

static void power_off(void)
{
    printf("[power] BOOT pressed -> entering deep sleep\n");

    /* Turn the LED off before sleeping. */
    gpio_set_level(USER_LED_PIN, 1);

    /* Wake up when BOOT is pressed (line goes low). */
    ESP_ERROR_CHECK(esp_sleep_enable_ext0_wakeup(BOOT_KEY_PIN, KEY_ACTIVE_LEVEL));

    /* Give the UART a moment to flush the message above. */
    vTaskDelay(pdMS_TO_TICKS(100));

    esp_deep_sleep_start();   /* does not return */
}

static void led_task(void *arg)
{
    while (1) {
        gpio_set_level(USER_LED_PIN, 0);   // LED on
        vTaskDelay(pdMS_TO_TICKS(500));

        gpio_set_level(USER_LED_PIN, 1);   // LED off
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

static void power_key_task(void *arg)
{
    int last_level = 1;   // released

    /* After wake-up the key may still be held down; wait until released. */
    wait_for_key_release();

    while (1) {
        int level = gpio_get_level(BOOT_KEY_PIN);
        if (level != last_level) {
            vTaskDelay(pdMS_TO_TICKS(KEY_DEBOUNCE_MS));
            int stable = gpio_get_level(BOOT_KEY_PIN);
            if (stable == level) {
                last_level = stable;
                if (stable == KEY_ACTIVE_LEVEL) {
#if POWER_KEY_HOLD_MS > 0
                    TickType_t pressed_at = xTaskGetTickCount();
                    while (gpio_get_level(BOOT_KEY_PIN) == KEY_ACTIVE_LEVEL) {
                        vTaskDelay(pdMS_TO_TICKS(KEY_POLL_MS));
                    }
                    if ((xTaskGetTickCount() - pressed_at) >= pdMS_TO_TICKS(POWER_KEY_HOLD_MS)) {
                        power_off();
                    }
#else
                    wait_for_key_release();
                    power_off();
#endif
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(KEY_POLL_MS));
    }
}

void app_main(void)
{
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    if (cause == ESP_SLEEP_WAKEUP_EXT0) {
        printf("[power] woke from deep sleep by BOOT key\n");
    } else {
        printf("[power] cold boot / reset\n");
    }

    gpio_reset_pin(USER_LED_PIN);
    gpio_set_direction(USER_LED_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(USER_LED_PIN, 1);   // start with LED off

    boot_key_init();

    xTaskCreate(led_task, "led", 2048, NULL, 5, NULL);
    xTaskCreate(power_key_task, "power_key", 2048, NULL, 5, NULL);
}

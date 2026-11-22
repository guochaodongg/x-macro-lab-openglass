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
 *   - Wi-Fi AP + HTTP server     -> first-time provisioning via browser
 *   - Wi-Fi STA + NVS memory     -> auto reconnect to saved AP after power cycle
 *   - OV2640 camera              -> MJPEG stream / JPEG snapshot in browser
 *
 * GPIO0 is an RTC GPIO, so it can be used as an EXT0 wake-up source while
 * the chip is in deep sleep.
 *
 * Web status page:
 *   SSID: OpenGlass-Status   Password: openglass123
 *   URL:  http://192.168.4.1/
 *   Live: http://192.168.4.1:81/stream
 *   Snap: http://192.168.4.1/snapshot
 *
 * Build / flash:
 *   idf.py set-target esp32s3
 *   idf.py build
 *   idf.py -p COMx flash monitor
 */
#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/event_groups.h"
#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_camera.h"
#include "esp_sleep.h"
#include "esp_attr.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "dns_server.h"

#define USER_LED_PIN       GPIO_NUM_21
#define BOOT_KEY_PIN       GPIO_NUM_0
#define KEY_ACTIVE_LEVEL   0
#define KEY_DEBOUNCE_MS    50
#define KEY_POLL_MS        10

/* Set to e.g. 1000 to require a 1 s hold before powering off (0 = any tap). */
#define POWER_KEY_HOLD_MS  0

/* Wi-Fi AP settings for the local status page. */
#define WIFI_AP_SSID        "OpenGlass-Status"
#define WIFI_AP_PASSWORD    "openglass123"
#define WIFI_AP_CHANNEL     1
#define WIFI_AP_MAX_CONNS   4
#define WIFI_NVS_NAMESPACE  "openglass"
#define WIFI_NVS_KEY_SSID   "wifi_ssid"
#define WIFI_NVS_KEY_PASS   "wifi_pass"
#define DEFAULT_WIFI_SSID   "pura80"
#define DEFAULT_WIFI_PASSWORD "88888888"
#define WIFI_SCAN_MAX_AP    16
#define WIFI_FORM_MAX_LEN   1024
#define WIFI_RETRY_DELAY_MS 5000
#define WEB_SERVER_PORT     80
#define CAMERA_STREAM_PORT  81
#define CAMERA_STREAM_CTRL_PORT 32769
#define WEB_AP_IP           "192.168.4.1"

/* APSTA channel-switch handling: allow a couple of attempts with a delay,
 * matching the approach used by 78/esp-wifi-connect. */
#define PROV_CONNECT_TIMEOUT_MS  10000
#define PROV_CONNECT_ATTEMPTS    2
#define PROV_CONNECT_RETRY_MS    3000
#define WIFI_SCAN_DONE_BIT       BIT0
#define WIFI_SCAN_TIMEOUT_MS     5000

/* OV2640 camera wiring on XIAO ESP32S3 Sense / OmiGlass. */
#define CAMERA_PIN_PWDN     -1
#define CAMERA_PIN_RESET    -1
#define CAMERA_PIN_XCLK     10
#define CAMERA_PIN_SIOD     40
#define CAMERA_PIN_SIOC     39
#define CAMERA_PIN_Y9       48
#define CAMERA_PIN_Y8       11
#define CAMERA_PIN_Y7       12
#define CAMERA_PIN_Y6       14
#define CAMERA_PIN_Y5       16
#define CAMERA_PIN_Y4       18
#define CAMERA_PIN_Y3       17
#define CAMERA_PIN_Y2       15
#define CAMERA_PIN_VSYNC    38
#define CAMERA_PIN_HREF     47
#define CAMERA_PIN_PCLK     13
#define CAMERA_XCLK_HZ      20000000
#define CAMERA_FRAME_SIZE   FRAMESIZE_VGA
#define CAMERA_JPEG_QUALITY 12
#define CAMERA_FB_COUNT     2
#define CAMERA_STREAM_DELAY_MS 100
#define CAMERA_VFLIP        1
#define CAMERA_HMIRROR      1

#define LOG_BUFFER_SIZE     2048
#define STATUS_BODY_SIZE    2048
#define LOG_LINE_SIZE       256
#define STATUS_PERIOD_MS    5000

typedef struct {
    const char *wake_reason;
    uint32_t led_toggles;
    uint32_t key_presses;
    uint32_t sta_count;
    uint32_t camera_frames;
    uint32_t camera_errors;
    uint32_t camera_last_len;
    uint32_t camera_width;
    uint32_t camera_height;
    char sta_ssid[32];
    char sta_ip[16];
    bool led_on;
    bool camera_ready;
    bool wifi_configured;
    bool sta_connected;
    int prov_state;
    char prov_message[96];
} runtime_status_t;

#define PROV_STATE_IDLE       0
#define PROV_STATE_CONNECTING 1
#define PROV_STATE_SUCCESS    2
#define PROV_STATE_FAILED     3

RTC_DATA_ATTR static uint32_t s_boot_count = 0;

static runtime_status_t s_status;
static SemaphoreHandle_t s_status_mutex;
static SemaphoreHandle_t s_log_mutex;
static SemaphoreHandle_t s_camera_mutex;
static EventGroupHandle_t s_wifi_event_group;
static httpd_handle_t s_httpd = NULL;
static httpd_handle_t s_stream_httpd = NULL;
static volatile bool s_wifi_connecting = false;
static volatile bool s_retry_scheduled = false;
static volatile bool s_prov_armed = false;
static volatile bool s_prov_got_ip = false;
static volatile bool s_prov_conn_failed = false;
static char s_pending_ssid[32];
static char s_pending_password[64];

static void prov_state_set(int state, const char *message)
{
    if (s_status_mutex != NULL && xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_status.prov_state = state;
        snprintf(s_status.prov_message, sizeof(s_status.prov_message), "%s", message);
        xSemaphoreGive(s_status_mutex);
    } else {
        s_status.prov_state = state;
        snprintf(s_status.prov_message, sizeof(s_status.prov_message), "%s", message);
    }
}

static char s_log_buffer[LOG_BUFFER_SIZE];
static size_t s_log_write_pos = 0;
static bool s_log_wrapped = false;

static void status_log(const char *fmt, ...)
{
    char line[LOG_LINE_SIZE];
    va_list args;

    va_start(args, fmt);
    int len = vsnprintf(line, sizeof(line), fmt, args);
    va_end(args);

    if (len < 0) {
        return;
    }
    if (len >= (int)sizeof(line)) {
        len = (int)sizeof(line) - 1;
    }

    printf("%.*s", len, line);

    if (s_log_mutex == NULL || xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }

    for (int i = 0; i < len; i++) {
        s_log_buffer[s_log_write_pos] = line[i];
        s_log_write_pos = (s_log_write_pos + 1) % LOG_BUFFER_SIZE;
        if (s_log_write_pos == 0) {
            s_log_wrapped = true;
        }
    }

    xSemaphoreGive(s_log_mutex);
}

static void status_snapshot(runtime_status_t *snapshot)
{
    memset(snapshot, 0, sizeof(*snapshot));

    if (s_status_mutex == NULL || xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }

    *snapshot = s_status;
    xSemaphoreGive(s_status_mutex);
}

static void status_update_led(bool on)
{
    if (s_status_mutex == NULL || xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }

    s_status.led_on = on;
    s_status.led_toggles++;
    xSemaphoreGive(s_status_mutex);
}

static void status_update_key(void)
{
    if (s_status_mutex == NULL || xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(50)) != pdTRUE) {
        return;
    }

    s_status.key_presses++;
    xSemaphoreGive(s_status_mutex);
}

static uint32_t uptime_seconds(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000000ULL);
}

static const char *wakeup_cause_string(esp_sleep_wakeup_cause_t cause)
{
    switch (cause) {
    case ESP_SLEEP_WAKEUP_EXT0:
        return "BOOT 键唤醒（EXT0）";
    case ESP_SLEEP_WAKEUP_TIMER:
        return "定时器唤醒";
    case ESP_SLEEP_WAKEUP_UNDEFINED:
        return "冷启动 / 复位";
    default:
        return "其他唤醒源";
    }
}

static bool camera_is_ready(void)
{
    runtime_status_t status;
    status_snapshot(&status);
    return status.camera_ready;
}

static void camera_init(void)
{
    camera_config_t config = {
        .pin_pwdn = CAMERA_PIN_PWDN,
        .pin_reset = CAMERA_PIN_RESET,
        .pin_xclk = CAMERA_PIN_XCLK,
        .pin_sccb_sda = CAMERA_PIN_SIOD,
        .pin_sccb_scl = CAMERA_PIN_SIOC,
        .pin_d7 = CAMERA_PIN_Y9,
        .pin_d6 = CAMERA_PIN_Y8,
        .pin_d5 = CAMERA_PIN_Y7,
        .pin_d4 = CAMERA_PIN_Y6,
        .pin_d3 = CAMERA_PIN_Y5,
        .pin_d2 = CAMERA_PIN_Y4,
        .pin_d1 = CAMERA_PIN_Y3,
        .pin_d0 = CAMERA_PIN_Y2,
        .pin_vsync = CAMERA_PIN_VSYNC,
        .pin_href = CAMERA_PIN_HREF,
        .pin_pclk = CAMERA_PIN_PCLK,
        .xclk_freq_hz = CAMERA_XCLK_HZ,
        .ledc_timer = LEDC_TIMER_0,
        .ledc_channel = LEDC_CHANNEL_0,
        .pixel_format = PIXFORMAT_JPEG,
        .frame_size = CAMERA_FRAME_SIZE,
        .jpeg_quality = CAMERA_JPEG_QUALITY,
        .fb_count = CAMERA_FB_COUNT,
        .fb_location = CAMERA_FB_IN_PSRAM,
        .grab_mode = CAMERA_GRAB_LATEST,
    };

    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        status_log("[camera] 初始化失败: %s\n", esp_err_to_name(err));
        return;
    }

    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor != NULL) {
        if (sensor->set_vflip != NULL) {
            sensor->set_vflip(sensor, CAMERA_VFLIP);
        }
        if (sensor->set_hmirror != NULL) {
            sensor->set_hmirror(sensor, CAMERA_HMIRROR);
        }
    }

    if (s_status_mutex != NULL && xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_status.camera_ready = true;
        xSemaphoreGive(s_status_mutex);
    } else {
        s_status.camera_ready = true;
    }

    status_log("[camera] 初始化成功: JPEG, frame_size=%d, XCLK=%u Hz, fb_count=%u\n",
               (int)CAMERA_FRAME_SIZE, (unsigned int)CAMERA_XCLK_HZ, (unsigned int)CAMERA_FB_COUNT);
}

static camera_fb_t *camera_lock_frame(void)
{
    if (!camera_is_ready() || s_camera_mutex == NULL) {
        return NULL;
    }
    if (xSemaphoreTake(s_camera_mutex, pdMS_TO_TICKS(2000)) != pdTRUE) {
        return NULL;
    }

    camera_fb_t *fb = esp_camera_fb_get();

    if (s_status_mutex != NULL && xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (fb != NULL) {
            s_status.camera_frames++;
            s_status.camera_last_len = fb->len;
            s_status.camera_width = fb->width;
            s_status.camera_height = fb->height;
        } else {
            s_status.camera_errors++;
        }
        xSemaphoreGive(s_status_mutex);
    }

    if (fb == NULL) {
        xSemaphoreGive(s_camera_mutex);
    }
    return fb;
}

static void camera_unlock_frame(camera_fb_t *fb)
{
    if (fb == NULL) {
        return;
    }
    esp_camera_fb_return(fb);
    xSemaphoreGive(s_camera_mutex);
}

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
    status_log("[power] BOOT pressed -> entering deep sleep\n");

    /* Turn the LED off before sleeping. */
    gpio_set_level(USER_LED_PIN, 1);

    /* Wake up when BOOT is pressed (line goes low). */
    ESP_ERROR_CHECK(esp_sleep_enable_ext0_wakeup(BOOT_KEY_PIN, KEY_ACTIVE_LEVEL));

    /* Give the console and HTTP client a moment to flush the message above. */
    vTaskDelay(pdMS_TO_TICKS(100));

    esp_deep_sleep_start();   /* does not return */
}

static void led_task(void *arg)
{
    while (1) {
        gpio_set_level(USER_LED_PIN, 0);   // LED on
        status_update_led(true);
        vTaskDelay(pdMS_TO_TICKS(500));

        gpio_set_level(USER_LED_PIN, 1);   // LED off
        status_update_led(false);
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
                    status_update_key();
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

static bool wifi_load_credentials(char *ssid, size_t ssid_size,
                                  char *password, size_t password_size)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) {
        return false;
    }

    size_t len = ssid_size;
    err = nvs_get_str(handle, WIFI_NVS_KEY_SSID, ssid, &len);
    if (err != ESP_OK) {
        nvs_close(handle);
        return false;
    }

    len = password_size;
    err = nvs_get_str(handle, WIFI_NVS_KEY_PASS, password, &len);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        password[0] = '\0';
    } else if (err != ESP_OK) {
        nvs_close(handle);
        return false;
    }

    nvs_close(handle);
    return ssid[0] != '\0';
}

static esp_err_t wifi_save_credentials(const char *ssid, const char *password)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(WIFI_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_str(handle, WIFI_NVS_KEY_SSID, ssid);
    if (err == ESP_OK) {
        err = nvs_set_str(handle, WIFI_NVS_KEY_PASS, password);
    }
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static esp_err_t wifi_configure_sta(const char *ssid, const char *password)
{
    wifi_config_t config = {0};
    strncpy((char *)config.sta.ssid, ssid, sizeof(config.sta.ssid) - 1);
    strncpy((char *)config.sta.password, password, sizeof(config.sta.password) - 1);
    config.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    config.sta.failure_retry_cnt = 3;
    config.sta.threshold.authmode = (password[0] == '\0') ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    return esp_wifi_set_config(WIFI_IF_STA, &config);
}

static void wifi_reconnect_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(WIFI_RETRY_DELAY_MS));
    s_retry_scheduled = false;

    if (s_status.wifi_configured && !s_status.sta_connected && !s_wifi_connecting) {
        status_log("[wifi] 尝试重连已保存热点: %s\n", s_status.sta_ssid);
        esp_err_t err = esp_wifi_connect();
        if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
            status_log("[wifi] 重连请求失败: %s\n", esp_err_to_name(err));
        }
    }
    vTaskDelete(NULL);
}

static void schedule_wifi_reconnect(void)
{
    if (!s_status.wifi_configured || s_retry_scheduled || s_wifi_connecting) {
        return;
    }
    s_retry_scheduled = true;
    if (xTaskCreate(wifi_reconnect_task, "wifi_retry", 2048, NULL, 5, NULL) != pdPASS) {
        s_retry_scheduled = false;
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    uint32_t count = 0;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        if (s_status_mutex != NULL && xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            s_status.sta_count++;
            count = s_status.sta_count;
            xSemaphoreGive(s_status_mutex);
        }
        status_log("[wifi] 客户端已连接，当前 %u 个\n", (unsigned int)count);
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        if (s_status_mutex != NULL && xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            if (s_status.sta_count > 0) {
                s_status.sta_count--;
            }
            count = s_status.sta_count;
            xSemaphoreGive(s_status_mutex);
        }
        status_log("[wifi] 客户端已断开，当前 %u 个\n", (unsigned int)count);
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (s_status.wifi_configured) {
            status_log("[wifi] 正在连接已保存热点: %s\n", s_status.sta_ssid);
            esp_wifi_connect();
        }
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED) {
        wifi_ap_record_t ap_info;
        if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
            if (s_status_mutex != NULL && xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                strncpy(s_status.sta_ssid, (const char *)ap_info.ssid, sizeof(s_status.sta_ssid) - 1);
                xSemaphoreGive(s_status_mutex);
            }
        }
        status_log("[wifi] 已连接到热点: %s\n", s_status.sta_ssid);
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *info = (wifi_event_sta_disconnected_t *)event_data;
        if (s_status_mutex != NULL && xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            s_status.sta_connected = false;
            s_status.sta_ip[0] = '\0';
            xSemaphoreGive(s_status_mutex);
        }
        status_log("[wifi] 热点连接断开，reason=%d\n", info != NULL ? (int)info->reason : -1);
        if (s_prov_armed) {
            s_prov_conn_failed = true;
        }
        schedule_wifi_reconnect();
        return;
    }

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_SCAN_DONE) {
        if (s_wifi_event_group != NULL) {
            xEventGroupSetBits(s_wifi_event_group, WIFI_SCAN_DONE_BIT);
        }
        return;
    }
}

static void ip_event_handler(void *arg, esp_event_base_t event_base,
                             int32_t event_id, void *event_data)
{
    if (event_base != IP_EVENT) {
        return;
    }

    if (event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        char ip[16] = {0};
        esp_ip4addr_ntoa(&event->ip_info.ip, ip, sizeof(ip));

        if (s_status_mutex != NULL && xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            strncpy(s_status.sta_ip, ip, sizeof(s_status.sta_ip) - 1);
            s_status.sta_connected = true;
            xSemaphoreGive(s_status_mutex);
        }
        status_log("[wifi] STA 已获取 IP: %s，网页可通过该地址访问\n", ip);
        if (s_prov_armed) {
            s_prov_got_ip = true;
        }
        return;
    }

    if (event_id == IP_EVENT_STA_LOST_IP) {
        if (s_status_mutex != NULL && xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            s_status.sta_connected = false;
            s_status.sta_ip[0] = '\0';
            xSemaphoreGive(s_status_mutex);
        }
        status_log("[wifi] STA IP 已丢失\n");
    }
}

static void wifi_init(void)
{
    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t wifi_init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init_config));

    static esp_event_handler_instance_t s_wifi_event_instance;
    static esp_event_handler_instance_t s_ip_event_instance;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                       ESP_EVENT_ANY_ID,
                                                       &wifi_event_handler,
                                                       NULL,
                                                       &s_wifi_event_instance));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT,
                                                       ESP_EVENT_ANY_ID,
                                                       &ip_event_handler,
                                                       NULL,
                                                       &s_ip_event_instance));

    wifi_config_t ap_config = {0};
    strncpy((char *)ap_config.ap.ssid, WIFI_AP_SSID, sizeof(ap_config.ap.ssid) - 1);
    ap_config.ap.ssid_len = strlen(WIFI_AP_SSID);
    strncpy((char *)ap_config.ap.password, WIFI_AP_PASSWORD, sizeof(ap_config.ap.password) - 1);
    ap_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    ap_config.ap.channel = WIFI_AP_CHANNEL;
    ap_config.ap.max_connection = WIFI_AP_MAX_CONNS;
    ap_config.ap.beacon_interval = 100;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));

    char ssid[32] = {0};
    char password[64] = {0};
    bool loaded = wifi_load_credentials(ssid, sizeof(ssid), password, sizeof(password));
    if (!loaded) {
        strncpy(ssid, DEFAULT_WIFI_SSID, sizeof(ssid) - 1);
        strncpy(password, DEFAULT_WIFI_PASSWORD, sizeof(password) - 1);
        esp_err_t err = wifi_save_credentials(ssid, password);
        if (err == ESP_OK) {
            status_log("[wifi] 未保存热点，已写入默认配置: %s\n", ssid);
        } else {
            status_log("[wifi] 写入默认热点失败: %s，本次仍会尝试连接\n", esp_err_to_name(err));
        }
    }

    ESP_ERROR_CHECK(wifi_configure_sta(ssid, password));
    if (s_status_mutex != NULL && xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        strncpy(s_status.sta_ssid, ssid, sizeof(s_status.sta_ssid) - 1);
        s_status.wifi_configured = true;
        xSemaphoreGive(s_status_mutex);
    } else {
        strncpy(s_status.sta_ssid, ssid, sizeof(s_status.sta_ssid) - 1);
        s_status.wifi_configured = true;
    }
    status_log("[wifi] %s热点: %s\n", loaded ? "已读取保存的" : "使用默认", ssid);

    ESP_ERROR_CHECK(esp_wifi_start());
    /* Keep the AP beacon stable while STA scans/connects; otherwise the SSID
     * can appear intermittently or not at all on some phones. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    /* Read back the effective AP config so a mismatch is visible in logs. */
    wifi_config_t effective = {0};
    if (esp_wifi_get_config(WIFI_IF_AP, &effective) == ESP_OK) {
        status_log("[wifi] AP 生效配置: SSID=\"%s\"(len=%u), auth=%d, channel=%u, hidden=%u, max_conn=%u\n",
                   (const char *)effective.ap.ssid, (unsigned)effective.ap.ssid_len,
                   (int)effective.ap.authmode, (unsigned)effective.ap.channel,
                   (unsigned)effective.ap.ssid_hidden, (unsigned)effective.ap.max_connection);
    }

    /* Captive portal: resolve every DNS query to the AP gateway so that a
     * phone/PC joining the config AP is taken straight to the web UI. */
    esp_ip4_addr_t gateway;
    IP4_ADDR(&gateway, 192, 168, 4, 1);
    dns_server_start(gateway);

    status_log("[wifi] 配网热点已启动: SSID=%s, 密码=%s\n", WIFI_AP_SSID, WIFI_AP_PASSWORD);
    status_log("[wifi] 连接热点后访问: http://%s/（多数设备会自动弹出配网页面）\n", WEB_AP_IP);
}

static int hex_value(char c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static void url_decode(const char *src, size_t len, char *dst, size_t dst_size)
{
    size_t out = 0;
    for (size_t i = 0; i < len && (out + 1) < dst_size; i++) {
        if (src[i] == '+') {
            dst[out++] = ' ';
        } else if (src[i] == '%' && (i + 2) < len) {
            int high = hex_value(src[i + 1]);
            int low = hex_value(src[i + 2]);
            if (high >= 0 && low >= 0) {
                dst[out++] = (char)((high << 4) | low);
                i += 2;
            } else {
                dst[out++] = src[i];
            }
        } else {
            dst[out++] = src[i];
        }
    }
    dst[out] = '\0';
}

static bool form_get_value(const char *body, const char *key, char *out, size_t out_size)
{
    char prefix[32];
    snprintf(prefix, sizeof(prefix), "%s=", key);

    const char *value = NULL;
    const char *search = body;
    while ((search = strstr(search, prefix)) != NULL) {
        if (search == body || search[-1] == '&') {
            value = search + strlen(prefix);
            break;
        }
        search++;
    }

    if (value == NULL) {
        out[0] = '\0';
        return false;
    }

    const char *end = strchr(value, '&');
    size_t len = end != NULL ? (size_t)(end - value) : strlen(value);
    url_decode(value, len, out, out_size);
    return true;
}

static size_t json_escape(const char *src, char *dst, size_t dst_size)
{
    size_t out = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p != '\0'; p++) {
        if (*p == '"' || *p == '\\') {
            if (out + 2 >= dst_size) {
                break;
            }
            dst[out++] = '\\';
            dst[out++] = (char)*p;
        } else if (*p < 0x20) {
            if (out + 6 >= dst_size) {
                break;
            }
            snprintf(dst + out, 7, "\\u%04x", (unsigned int)*p);
            out += 6;
        } else {
            if (out + 1 >= dst_size) {
                break;
            }
            dst[out++] = (char)*p;
        }
    }
    dst[out] = '\0';
    return out;
}

static esp_err_t wifi_scan_get_handler(httpd_req_t *req)
{
    wifi_ap_record_t ap_records[WIFI_SCAN_MAX_AP];
    uint16_t ap_count = WIFI_SCAN_MAX_AP;
    memset(ap_records, 0, sizeof(ap_records));

    esp_err_t err;
    if (s_wifi_event_group == NULL) {
        err = esp_wifi_scan_start(NULL, true);
    } else {
        xEventGroupClearBits(s_wifi_event_group, WIFI_SCAN_DONE_BIT);
        err = esp_wifi_scan_start(NULL, false);
        if (err == ESP_OK) {
            EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                                   WIFI_SCAN_DONE_BIT,
                                                   pdTRUE,
                                                   pdFALSE,
                                                   pdMS_TO_TICKS(WIFI_SCAN_TIMEOUT_MS));
            if ((bits & WIFI_SCAN_DONE_BIT) == 0) {
                err = ESP_ERR_TIMEOUT;
            }
        }
    }
    if (err != ESP_OK) {
        status_log("[wifi] 扫描热点失败: %s\n", esp_err_to_name(err));
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"aps\":[],\"error\":\"scan failed\"}");
    }

    err = esp_wifi_scan_get_ap_records(&ap_count, ap_records);
    if (err != ESP_OK) {
        ap_count = 0;
    }

    char body[2048];
    int written = snprintf(body, sizeof(body), "{\"aps\":[");
    if (written < 0) {
        return ESP_FAIL;
    }
    size_t used = (size_t)written;

    for (uint16_t i = 0; i < ap_count; i++) {
        char ssid[64];
        json_escape((const char *)ap_records[i].ssid, ssid, sizeof(ssid));
        int n = snprintf(body + used, sizeof(body) - used,
                         "%s{\"ssid\":\"%s\",\"rssi\":%d,\"open\":%s}",
                         i == 0 ? "" : ",",
                         ssid,
                         (int)ap_records[i].rssi,
                         ap_records[i].authmode == WIFI_AUTH_OPEN ? "true" : "false");
        if (n < 0 || (size_t)n >= sizeof(body) - used) {
            break;
        }
        used += (size_t)n;
    }

    snprintf(body + used, sizeof(body) - used, "]}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, strlen(body));
}

static void wifi_apply_task(void *arg)
{
    (void)arg;
    /* Delay so the HTTP response can be delivered before the STA is disconnected. */
    vTaskDelay(pdMS_TO_TICKS(300));

    bool connected = false;

    for (int attempt = 1; attempt <= PROV_CONNECT_ATTEMPTS && !connected; attempt++) {
        /* APSTA channel switch can break the first association; retry with a gap. */
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(200));

        esp_err_t err = wifi_configure_sta(s_pending_ssid, s_pending_password);
        if (err != ESP_OK) {
            status_log("[prov] 配置 STA 失败: %s\n", esp_err_to_name(err));
            break;
        }

        s_prov_got_ip = false;
        s_prov_conn_failed = false;
        s_prov_armed = true;
        err = esp_wifi_connect();
        if (err != ESP_OK && err != ESP_ERR_WIFI_CONN) {
            s_prov_armed = false;
            status_log("[prov] 连接请求失败: %s\n", esp_err_to_name(err));
            break;
        }

        status_log("[prov] 正在验证热点 `%s`（第 %d/%d 次）\n",
                   s_pending_ssid, attempt, PROV_CONNECT_ATTEMPTS);

        int waited_ms = 0;
        while (waited_ms < PROV_CONNECT_TIMEOUT_MS && !s_prov_got_ip && !s_prov_conn_failed) {
            vTaskDelay(pdMS_TO_TICKS(200));
            waited_ms += 200;
        }
        s_prov_armed = false;
        connected = s_prov_got_ip;

        if (!connected && attempt < PROV_CONNECT_ATTEMPTS) {
            status_log("[prov] 本次未连接成功，%d ms 后重试\n", PROV_CONNECT_RETRY_MS);
            vTaskDelay(pdMS_TO_TICKS(PROV_CONNECT_RETRY_MS));
        }
    }

    if (connected) {
        /* Persist credentials only after the target AP was actually reached. */
        esp_err_t err = wifi_save_credentials(s_pending_ssid, s_pending_password);
        if (err == ESP_OK) {
            if (s_status_mutex != NULL && xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
                s_status.wifi_configured = true;
                xSemaphoreGive(s_status_mutex);
            } else {
                s_status.wifi_configured = true;
            }
            status_log("[prov] 配网成功，凭据已保存: %s\n", s_pending_ssid);
            prov_state_set(PROV_STATE_SUCCESS, "连接成功，凭据已保存");
        } else {
            status_log("[prov] 已连接但保存凭据失败: %s\n", esp_err_to_name(err));
            prov_state_set(PROV_STATE_FAILED, "已连接但保存失败");
        }
    } else {
        status_log("[prov] 配网失败，未保存凭据\n");
        prov_state_set(PROV_STATE_FAILED, "连接失败，请检查密码或热点");
    }

    s_wifi_connecting = false;
    vTaskDelete(NULL);
}

static esp_err_t wifi_connect_post_handler(httpd_req_t *req)
{
    size_t content_len = httpd_req_get_hdr_value_len(req, "Content-Length");
    if (content_len == 0 || content_len > WIFI_FORM_MAX_LEN) {
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        return httpd_resp_sendstr(req, "请求内容无效\n");
    }

    char *body = malloc(content_len + 1);
    if (body == NULL) {
        httpd_resp_set_status(req, "500 Internal Server Error");
        return httpd_resp_sendstr(req, "内存不足\n");
    }

    size_t received = 0;
    while (received < content_len) {
        int n = httpd_req_recv(req, body + received, content_len - received);
        if (n <= 0) {
            free(body);
            httpd_resp_set_status(req, "400 Bad Request");
            return httpd_resp_sendstr(req, "读取请求失败\n");
        }
        received += (size_t)n;
    }
    body[received] = '\0';

    status_log("[prov] 收到配网请求 len=%u has_ssid=%d has_pass=%d\n",
               (unsigned int)content_len,
               strstr(body, "ssid=") != NULL ? 1 : 0,
               strstr(body, "pass=") != NULL ? 1 : 0);

    char ssid[32] = {0};
    char password[64] = {0};
    if (!form_get_value(body, "ssid", ssid, sizeof(ssid)) || ssid[0] == '\0') {
        free(body);
        httpd_resp_set_status(req, "400 Bad Request");
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        return httpd_resp_sendstr(req, "服务器未解析到 Wi-Fi 名称，请重新输入后提交\n");
    }
    form_get_value(body, "pass", password, sizeof(password));
    status_log("[prov] 解析结果 ssid_len=%u pass_len=%u\n",
               (unsigned int)strlen(ssid), (unsigned int)strlen(password));
    free(body);

    if (s_wifi_connecting) {
        httpd_resp_set_status(req, "429 Too Many Requests");
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        return httpd_resp_sendstr(req, "正在连接其他热点，请稍后再试\n");
    }

    /* Credentials are persisted only after the connection is verified,
     * matching 78/esp-wifi-connect's "connect first, then save" flow. */
    strncpy(s_pending_ssid, ssid, sizeof(s_pending_ssid) - 1);
    strncpy(s_pending_password, password, sizeof(s_pending_password) - 1);
    s_wifi_connecting = true;
    prov_state_set(PROV_STATE_CONNECTING, "正在验证热点连接...");

    if (s_status_mutex != NULL && xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_status.sta_connected = false;
        s_status.sta_ip[0] = '\0';
        xSemaphoreGive(s_status_mutex);
    } else {
        s_status.sta_connected = false;
        s_status.sta_ip[0] = '\0';
    }

    if (xTaskCreate(wifi_apply_task, "wifi_apply", 3072, NULL, 5, NULL) != pdPASS) {
        s_wifi_connecting = false;
        prov_state_set(PROV_STATE_FAILED, "创建连接任务失败");
        httpd_resp_set_status(req, "500 Internal Server Error");
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        return httpd_resp_sendstr(req, "创建连接任务失败\n");
    }

    status_log("[prov] 已提交热点 `%s`，正在验证连接\n", ssid);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_sendstr(req, "正在验证连接，请查看配网状态区域获取结果\n");
    return ESP_OK;
}

static const char *const PROVISION_HTML =
    "<!doctype html>\n"
    "<html lang=\"zh-CN\">\n"
    "<head>\n"
    "<meta charset=\"utf-8\">\n"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
    "<title>Wi-Fi 配网</title>\n"
    "<style>\n"
    "body{margin:0;background:#0f172a;color:#e2e8f0;font-family:Consolas,Monaco,monospace;}\n"
    ".wrap{max-width:720px;margin:0 auto;padding:28px 20px;}\n"
    "h1{font-size:24px;margin:0 0 6px;color:#f8fafc;}\n"
    ".hint{color:#94a3b8;font-size:13px;margin:6px 0 0;}\n"
    ".card{background:#1e293b;border:1px solid #334155;border-radius:10px;padding:18px;margin-bottom:16px;}\n"
    ".title{font-size:14px;color:#93c5fd;margin-bottom:12px;}\n"
    "pre{margin:0;white-space:pre-wrap;word-break:break-all;line-height:1.55;}\n"
    ".dot{display:inline-block;width:8px;height:8px;border-radius:50%;background:#22c55e;margin-right:8px;}\n"
    "input,button{font-family:inherit;font-size:13px;padding:8px 10px;border-radius:6px;border:1px solid #475569;background:#0f172a;color:#e2e8f0;}\n"
    "button{background:#2563eb;border-color:#2563eb;cursor:pointer;}\n"
    ".row{display:flex;gap:8px;flex-wrap:wrap;align-items:center;}\n"
    "#wifi-msg,#prov-status{color:#fbbf24;}\n"
    "</style>\n"
    "</head>\n"
    "<body>\n"
    "<div class=\"wrap\">\n"
    "<h1><span class=\"dot\"></span>Wi-Fi 配网</h1>\n"
    "<div class=\"hint\">选择公共 Wi-Fi 或手机热点并输入密码；验证成功后会自动进入功能页面。</div>\n"
    "<div class=\"card\">\n"
    "<div class=\"title\">热点配置</div>\n"
    "<div class=\"hint\">Wi-Fi 名称（可点击扫描后选择）</div>\n"
    "<div class=\"row\">\n"
    "<input id=\"ssid\" name=\"ssid\" list=\"wifi-list\" placeholder=\"Wi-Fi 名称\" maxlength=\"31\">\n"
    "<datalist id=\"wifi-list\"></datalist>\n"
    "<button type=\"button\" onclick=\"scanWifi()\">扫描热点</button>\n"
    "</div>\n"
    "<div class=\"hint\" style=\"margin-top:10px;\">密码（开放网络留空）</div>\n"
    "<div class=\"row\" style=\"margin-top:4px;\">\n"
    "<input id=\"pass\" name=\"pass\" type=\"password\" placeholder=\"密码（开放网络留空）\" maxlength=\"63\">\n"
    "<button type=\"button\" onclick=\"connectWifi()\">连接并保存</button>\n"
    "</div>\n"
    "<div class=\"hint\" id=\"wifi-msg\">尚未提交配置</div>\n"
    "</div>\n"
    "<div class=\"card\"><pre id=\"prov-status\">正在检查配网状态...</pre></div>\n"
    "</div>\n"
    "<script>\n"
    "let redirecting=false;\n"
    "function field(text,name){const m=text.match(new RegExp(name+'\\\\s*: (.+)'));return m?m[1]:'';}\n"
    "async function scanWifi(){\n"
    "  try{\n"
    "    const data=await (await fetch('/api/wifi/scan',{cache:'no-store'})).json();\n"
    "    const list=document.getElementById('wifi-list');\n"
    "    list.innerHTML='';\n"
    "    (data.aps||[]).forEach(function(ap){\n"
    "      const o=document.createElement('option');\n"
    "      o.value=ap.ssid;\n"
    "      o.textContent=ap.ssid+'  '+ap.rssi+' dBm'+(ap.open?'  [开放]':'');\n"
    "      list.appendChild(o);\n"
    "    });\n"
    "    document.getElementById('wifi-msg').textContent='已扫描到 '+((data.aps||[]).length)+' 个热点';\n"
    "  }catch(e){document.getElementById('wifi-msg').textContent='扫描失败: '+e;}\n"
    "}\n"
    "async function connectWifi(){\n"
    "  const ssid=document.getElementById('ssid').value.trim();\n"
    "  const pass=document.getElementById('pass').value;\n"
    "  if(!ssid){document.getElementById('wifi-msg').textContent='请先在上方输入框填写 Wi-Fi 名称';return;}\n"
    "  const body='ssid='+encodeURIComponent(ssid)+'&pass='+encodeURIComponent(pass);\n"
    "  try{\n"
    "    const r=await fetch('/api/wifi/connect',{method:'POST',cache:'no-store',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:body});\n"
    "    document.getElementById('wifi-msg').textContent=await r.text();\n"
    "    checkStatus();\n"
    "  }catch(e){document.getElementById('wifi-msg').textContent='提交失败: '+e;}\n"
    "}\n"
    "async function checkStatus(){\n"
    "  try{\n"
    "    const text=await (await fetch('/api/status',{cache:'no-store'})).text();\n"
    "    const st=field(text,'配网状态');\n"
    "    const msg=field(text,'配网信息');\n"
    "    document.getElementById('prov-status').textContent='配网状态: '+st+'\\n配网信息: '+msg;\n"
    "    if(st==='成功'){\n"
    "      document.getElementById('wifi-msg').textContent='配网成功，即将进入功能页面...';\n"
    "      if(!redirecting){redirecting=true;setTimeout(function(){location.href='/app';},1000);}\n"
    "    } else if(st==='失败'){\n"
    "      document.getElementById('wifi-msg').textContent='配网失败：'+msg;\n"
    "    }\n"
    "  }catch(e){\n"
    "    document.getElementById('prov-status').textContent='状态查询暂不可用，正在重试...';\n"
    "  }\n"
    "}\n"
    "setInterval(checkStatus,1000);\n"
    "checkStatus();\n"
    "</script>\n"
    "</body>\n"
    "</html>\n";

static const char *const APP_HTML =
    "<!doctype html>\n"
    "<html lang=\"zh-CN\">\n"
    "<head>\n"
    "<meta charset=\"utf-8\">\n"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
    "<title>XIAO ESP32S3 状态与摄像头</title>\n"
    "<style>\n"
    "body{margin:0;background:#0f172a;color:#e2e8f0;font-family:Consolas,Monaco,monospace;}\n"
    ".wrap{max-width:960px;margin:0 auto;padding:24px 20px;}\n"
    "h1{font-size:22px;margin:0 0 4px;color:#f8fafc;}\n"
    ".hint{color:#94a3b8;font-size:13px;margin:6px 0 0;}\n"
    ".card{background:#1e293b;border:1px solid #334155;border-radius:10px;padding:16px;margin-bottom:16px;}\n"
    ".title{font-size:14px;color:#93c5fd;margin-bottom:10px;}\n"
    "#cam{width:100%;max-width:640px;display:block;border-radius:8px;background:#000;}\n"
    "pre{margin:0;white-space:pre-wrap;word-break:break-all;line-height:1.55;}\n"
    "#logs{max-height:320px;overflow:auto;color:#a7f3d0;font-size:13px;}\n"
    ".dot{display:inline-block;width:8px;height:8px;border-radius:50%;background:#22c55e;margin-right:8px;}\n"
    "a{color:#38bdf8;}\n"
    "</style>\n"
    "</head>\n"
    "<body>\n"
    "<div class=\"wrap\">\n"
    "<h1><span class=\"dot\"></span>XIAO ESP32S3 状态与摄像头</h1>\n"
    "<div class=\"hint\">功能页面 · 每秒刷新状态和日志 · <a href=\"/provision\">重新配网</a></div>\n"
    "<div class=\"card\">\n"
    "<div class=\"title\">摄像头实时画面（MJPEG，独立端口）</div>\n"
    "<img id=\"cam\" src=\"\" alt=\"正在连接摄像头...\">\n"
    "<div class=\"hint\"><a href=\"/snapshot\" target=\"_blank\">打开单帧 JPEG 抓拍</a></div>\n"
    "</div>\n"
    "<div class=\"card\"><pre id=\"status\">正在加载状态...</pre></div>\n"
    "<div class=\"card\"><pre id=\"logs\">正在加载日志...</pre></div>\n"
    "</div>\n"
    "<script>\n"
    "const cam=document.getElementById('cam');\n"
    "cam.onerror=function(){this.alt='摄像头画面暂不可用';};\n"
    "cam.src=location.protocol+'//'+location.hostname+':81/stream';\n"
    "async function refresh(){\n"
    "  try{\n"
    "    const s=await fetch('/api/status');\n"
    "    const l=await fetch('/api/logs');\n"
    "    document.getElementById('status').textContent=await s.text();\n"
    "    const pre=document.getElementById('logs');\n"
    "    pre.textContent=await l.text();\n"
    "    pre.scrollTop=pre.scrollHeight;\n"
    "  }catch(e){\n"
    "    document.getElementById('status').textContent='状态获取失败: '+e;\n"
    "  }\n"
    "}\n"
    "setInterval(refresh,1000);\n"
    "refresh();\n"
    "</script>\n"
    "</body>\n"
    "</html>\n";

static esp_err_t send_html(httpd_req_t *req, const char *html)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, html, strlen(html));
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    runtime_status_t status;
    status_snapshot(&status);
    return send_html(req, status.wifi_configured ? APP_HTML : PROVISION_HTML);
}

static esp_err_t app_get_handler(httpd_req_t *req)
{
    return send_html(req, APP_HTML);
}

static esp_err_t provision_get_handler(httpd_req_t *req)
{
    return send_html(req, PROVISION_HTML);
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    char body[STATUS_BODY_SIZE];
    runtime_status_t status;
    status_snapshot(&status);

    int len = snprintf(body, sizeof(body),
                       "运行时间      : %lu s\n"
                       "唤醒原因      : %s\n"
                       "启动次数      : %lu\n"
                       "LED 状态      : %s\n"
                       "LED 翻转次数  : %lu\n"
                       "BOOT 按键次数 : %lu\n"
                       "Wi-Fi 客户端  : %u\n"
                       "已保存热点    : %s\n"
                       "STA 连接      : %s\n"
                       "STA 热点      : %s\n"
                       "STA IP        : %s\n"
                       "摄像头        : %s\n"
                       "画面尺寸      : %ux%u\n"
                       "已采集帧数    : %lu\n"
                       "抓拍失败次数  : %lu\n"
                       "最近 JPEG     : %lu bytes\n"
                       "空闲堆内存    : %lu bytes\n"
                       "配网热点      : %s\n"
                       "配网地址      : http://%s/\n"
                       "局域网地址    : http://%s/\n"
                       "配网状态      : %s\n"
                       "配网信息      : %s\n",
                       (unsigned long)uptime_seconds(),
                       status.wake_reason != NULL ? status.wake_reason : "未知",
                       (unsigned long)s_boot_count,
                       status.led_on ? "亮" : "灭",
                       (unsigned long)status.led_toggles,
                       (unsigned long)status.key_presses,
                       (unsigned int)status.sta_count,
                       status.wifi_configured ? status.sta_ssid : "未配置",
                       status.sta_connected ? "已连接" : (status.wifi_configured ? "连接中/未连接" : "未配置"),
                       status.sta_ssid[0] != '\0' ? status.sta_ssid : "未配置",
                       status.sta_ip[0] != '\0' ? status.sta_ip : "等待分配",
                       status.camera_ready ? "就绪" : "不可用",
                       (unsigned int)status.camera_width,
                       (unsigned int)status.camera_height,
                       (unsigned long)status.camera_frames,
                       (unsigned long)status.camera_errors,
                       (unsigned long)status.camera_last_len,
                       (unsigned long)esp_get_free_heap_size(),
                       WIFI_AP_SSID,
                       WEB_AP_IP,
                       status.sta_ip[0] != '\0' ? status.sta_ip : "未连接",
                       status.prov_state == PROV_STATE_CONNECTING ? "验证中" :
                       (status.prov_state == PROV_STATE_SUCCESS ? "成功" :
                       (status.prov_state == PROV_STATE_FAILED ? "失败" : "空闲")),
                       status.prov_message[0] != '\0' ? status.prov_message : "无");
    if (len < 0) {
        return ESP_FAIL;
    }
    if (len >= (int)sizeof(body)) {
        len = (int)sizeof(body) - 1;
    }

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, len);
}

static esp_err_t logs_get_handler(httpd_req_t *req)
{
    char body[LOG_BUFFER_SIZE + 1];
    size_t out = 0;

    if (s_log_mutex != NULL && xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        if (s_log_wrapped) {
            if (s_log_write_pos == 0) {
                for (size_t i = 0; i < LOG_BUFFER_SIZE; i++) {
                    body[out++] = s_log_buffer[i];
                }
            } else {
                for (size_t i = s_log_write_pos; i < LOG_BUFFER_SIZE; i++) {
                    body[out++] = s_log_buffer[i];
                }
                for (size_t i = 0; i < s_log_write_pos; i++) {
                    body[out++] = s_log_buffer[i];
                }
            }
        } else {
            for (size_t i = 0; i < s_log_write_pos; i++) {
                body[out++] = s_log_buffer[i];
            }
        }
        xSemaphoreGive(s_log_mutex);
    } else {
        const char *msg = "日志暂不可用\n";
        size_t msg_len = strlen(msg);
        memcpy(body, msg, msg_len);
        out = msg_len;
    }

    body[out] = '\0';

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, out);
}

static esp_err_t snapshot_get_handler(httpd_req_t *req)
{
    camera_fb_t *fb = camera_lock_frame();
    if (fb == NULL) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        return httpd_resp_sendstr(req, "摄像头未就绪\n");
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Content-Disposition", "inline; filename=snapshot.jpg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t ret = httpd_resp_send(req, (const char *)fb->buf, fb->len);
    camera_unlock_frame(fb);
    return ret;
}

static esp_err_t stream_get_handler(httpd_req_t *req)
{
    if (!camera_is_ready()) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        return httpd_resp_sendstr(req, "摄像头未就绪\n");
    }

    httpd_resp_set_type(req, "multipart/x-mixed-replace; boundary=frame");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    while (1) {
        camera_fb_t *fb = camera_lock_frame();
        if (fb == NULL) {
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        char header[128];
        int header_len = snprintf(header, sizeof(header),
                                  "--frame\r\n"
                                  "Content-Type: image/jpeg\r\n"
                                  "Content-Length: %u\r\n\r\n",
                                  (unsigned int)fb->len);

        esp_err_t ret = httpd_resp_send_chunk(req, header, header_len);
        if (ret == ESP_OK) {
            ret = httpd_resp_send_chunk(req, (const char *)fb->buf, fb->len);
        }
        if (ret == ESP_OK) {
            ret = httpd_resp_send_chunk(req, "\r\n", 2);
        }

        camera_unlock_frame(fb);

        if (ret != ESP_OK) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(CAMERA_STREAM_DELAY_MS));
    }

    return ESP_OK;
}

static void stream_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = CAMERA_STREAM_PORT;
    config.ctrl_port = CAMERA_STREAM_CTRL_PORT;
    config.stack_size = 6144;
    config.max_uri_handlers = 2;
    config.max_open_sockets = 2;

    ESP_ERROR_CHECK(httpd_start(&s_stream_httpd, &config));

    const httpd_uri_t stream_uri = {
        .uri = "/stream",
        .method = HTTP_GET,
        .handler = stream_get_handler,
        .user_ctx = NULL,
    };
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_stream_httpd, &stream_uri));

    status_log("[http] 摄像头视频流已启动: http://%s:%u/stream\n", WEB_AP_IP, CAMERA_STREAM_PORT);
}

/* Captive portal: OS/phone connectivity probes are redirected to the config page. */
static esp_err_t captive_portal_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, NULL, 0);
}

static void web_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = WEB_SERVER_PORT;
    config.stack_size = 6144;
    config.max_uri_handlers = 22;
    config.max_open_sockets = 7;
    config.uri_match_fn = httpd_uri_match_wildcard;

    ESP_ERROR_CHECK(httpd_start(&s_httpd, &config));

    const httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t app_uri = {
        .uri = "/app",
        .method = HTTP_GET,
        .handler = app_get_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t provision_uri = {
        .uri = "/provision",
        .method = HTTP_GET,
        .handler = provision_get_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t status_uri = {
        .uri = "/api/status",
        .method = HTTP_GET,
        .handler = status_get_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t logs_uri = {
        .uri = "/api/logs",
        .method = HTTP_GET,
        .handler = logs_get_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t snapshot_uri = {
        .uri = "/snapshot",
        .method = HTTP_GET,
        .handler = snapshot_get_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t stream_uri = {
        .uri = "/stream",
        .method = HTTP_GET,
        .handler = snapshot_get_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t wifi_scan_uri = {
        .uri = "/api/wifi/scan",
        .method = HTTP_GET,
        .handler = wifi_scan_get_handler,
        .user_ctx = NULL,
    };
    const httpd_uri_t wifi_connect_uri = {
        .uri = "/api/wifi/connect",
        .method = HTTP_POST,
        .handler = wifi_connect_post_handler,
        .user_ctx = NULL,
    };

    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd, &root_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd, &app_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd, &provision_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd, &status_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd, &logs_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd, &snapshot_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd, &stream_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd, &wifi_scan_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd, &wifi_connect_uri));

    /* Connectivity-check endpoints used by iOS / Android / Windows / Firefox.
     * A 302 to "/" makes the OS pop the captive-portal login automatically. */
    static const char *const probe_uris[] = {
        "/hotspot-detect.html", "/library/test/success.html",
        "/generate_204", "/gen_204", "/api/v1/generate_204", "/mobile/status.php",
        "/ncsi.txt", "/connectivity-check", "/connectivity-check.html",
        "/success.txt", "/portal.html", "/fwlink/",
    };
    for (size_t i = 0; i < sizeof(probe_uris) / sizeof(probe_uris[0]); i++) {
        httpd_uri_t probe = {
            .uri = probe_uris[i],
            .method = HTTP_GET,
            .handler = captive_portal_handler,
            .user_ctx = NULL,
        };
        httpd_register_uri_handler(s_httpd, &probe);
    }

    status_log("[http] 网页服务已启动: http://%s/（未配置时配网，已配置后进功能页），功能页: http://%s/app，重配: http://%s/provision\n",
               WEB_AP_IP, WEB_AP_IP, WEB_AP_IP);
    status_log("[http] 视频流服务已启动: http://%s:%u/stream\n", WEB_AP_IP, CAMERA_STREAM_PORT);
}

static void monitor_task(void *arg)
{
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(STATUS_PERIOD_MS));

        runtime_status_t status;
        status_snapshot(&status);
        status_log("[status] uptime=%lus led=%s blink=%lu key=%lu clients=%u cam_frames=%lu heap=%lu\n",
                   (unsigned long)uptime_seconds(),
                   status.led_on ? "on" : "off",
                   (unsigned long)status.led_toggles,
                   (unsigned long)status.key_presses,
                   (unsigned int)status.sta_count,
                   (unsigned long)status.camera_frames,
                   (unsigned long)esp_get_free_heap_size());
    }
}

void app_main(void)
{
    s_status_mutex = xSemaphoreCreateMutex();
    s_log_mutex = xSemaphoreCreateMutex();
    s_camera_mutex = xSemaphoreCreateMutex();
    s_wifi_event_group = xEventGroupCreate();

    esp_err_t nvs_ret = nvs_flash_init();
    if (nvs_ret == ESP_ERR_NVS_NO_FREE_PAGES || nvs_ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_ret);

    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    const char *reason = wakeup_cause_string(cause);
    s_boot_count++;

    if (s_status_mutex != NULL && xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_status.wake_reason = reason;
        xSemaphoreGive(s_status_mutex);
    } else {
        s_status.wake_reason = reason;
    }

    status_log("[system] 启动 #%lu，原因: %s\n", (unsigned long)s_boot_count, reason);

    gpio_reset_pin(USER_LED_PIN);
    gpio_set_direction(USER_LED_PIN, GPIO_MODE_OUTPUT);
    gpio_set_level(USER_LED_PIN, 1);   // start with LED off

    boot_key_init();
    camera_init();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    wifi_init();
    web_server_start();
    stream_server_start();

    xTaskCreate(led_task, "led", 2048, NULL, 5, NULL);
    xTaskCreate(power_key_task, "power_key", 2048, NULL, 5, NULL);
    xTaskCreate(monitor_task, "monitor", 2048, NULL, 5, NULL);
}

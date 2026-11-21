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
 *   - Wi-Fi AP + HTTP server     -> view runtime status/logs in a browser
 *   - OV2640 camera              -> MJPEG stream / JPEG snapshot in browser
 *
 * GPIO0 is an RTC GPIO, so it can be used as an EXT0 wake-up source while
 * the chip is in deep sleep.
 *
 * Web status page:
 *   SSID: OpenGlass-Status   Password: openglass123
 *   URL:  http://192.168.4.1/
 *   Live: http://192.168.4.1/stream
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
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
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
#include "nvs_flash.h"

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
#define WEB_SERVER_PORT     80
#define WEB_AP_IP           "192.168.4.1"

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
#define STATUS_BODY_SIZE    1536
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
    bool led_on;
    bool camera_ready;
} runtime_status_t;

RTC_DATA_ATTR static uint32_t s_boot_count = 0;

static runtime_status_t s_status;
static SemaphoreHandle_t s_status_mutex;
static SemaphoreHandle_t s_log_mutex;
static SemaphoreHandle_t s_camera_mutex;
static httpd_handle_t s_httpd = NULL;

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
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        if (s_status_mutex != NULL && xSemaphoreTake(s_status_mutex, pdMS_TO_TICKS(50)) == pdTRUE) {
            if (s_status.sta_count > 0) {
                s_status.sta_count--;
            }
            count = s_status.sta_count;
            xSemaphoreGive(s_status_mutex);
        }
        status_log("[wifi] 客户端已断开，当前 %u 个\n", (unsigned int)count);
    }
}

static void wifi_init_ap(void)
{
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t wifi_init_config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init_config));

    static esp_event_handler_instance_t s_wifi_event_instance;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT,
                                                       ESP_EVENT_ANY_ID,
                                                       &wifi_event_handler,
                                                       NULL,
                                                       &s_wifi_event_instance));

    wifi_config_t wifi_config = {0};
    strncpy((char *)wifi_config.ap.ssid, WIFI_AP_SSID, sizeof(wifi_config.ap.ssid) - 1);
    wifi_config.ap.ssid_len = strlen(WIFI_AP_SSID);
    strncpy((char *)wifi_config.ap.password, WIFI_AP_PASSWORD, sizeof(wifi_config.ap.password) - 1);
    wifi_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    wifi_config.ap.channel = WIFI_AP_CHANNEL;
    wifi_config.ap.max_connection = WIFI_AP_MAX_CONNS;
    wifi_config.ap.beacon_interval = 100;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    status_log("[wifi] 热点已启动: SSID=%s, 密码=%s\n", WIFI_AP_SSID, WIFI_AP_PASSWORD);
    status_log("[wifi] 连接热点后访问: http://%s/\n", WEB_AP_IP);
}

static const char *const INDEX_HTML =
    "<!doctype html>\n"
    "<html lang=\"zh-CN\">\n"
    "<head>\n"
    "<meta charset=\"utf-8\">\n"
    "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
    "<title>XIAO ESP32S3 状态与摄像头</title>\n"
    "<style>\n"
    "body{margin:0;background:#0f172a;color:#e2e8f0;font-family:Consolas,Monaco,monospace;}\n"
    ".wrap{max-width:960px;margin:0 auto;padding:24px;}\n"
    "h1{font-size:22px;margin:0 0 4px;color:#f8fafc;}\n"
    ".hint{color:#94a3b8;font-size:13px;margin:6px 0 0;}\n"
    ".card{background:#1e293b;border:1px solid #334155;border-radius:10px;padding:16px;margin-bottom:16px;}\n"
    ".camtitle{font-size:14px;color:#93c5fd;margin-bottom:10px;}\n"
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
    "<div class=\"hint\">页面每秒自动刷新 · 按 BOOT 进入深度睡眠后网页和摄像头会停止</div>\n"
    "<div class=\"card\">\n"
    "<div class=\"camtitle\">摄像头实时画面（MJPEG）</div>\n"
    "<img id=\"cam\" src=\"/stream\" alt=\"正在连接摄像头...\">\n"
    "<div class=\"hint\"><a href=\"/snapshot\" target=\"_blank\">打开单帧 JPEG 抓拍</a></div>\n"
    "</div>\n"
    "<div class=\"card\"><pre id=\"status\">正在加载状态...</pre></div>\n"
    "<div class=\"card\"><pre id=\"logs\">正在加载日志...</pre></div>\n"
    "</div>\n"
    "<script>\n"
    "const cam=document.getElementById('cam');\n"
    "cam.onerror=function(){this.alt='摄像头画面暂不可用';};\n"
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

static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, INDEX_HTML, strlen(INDEX_HTML));
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
                       "摄像头        : %s\n"
                       "画面尺寸      : %ux%u\n"
                       "已采集帧数    : %lu\n"
                       "抓拍失败次数  : %lu\n"
                       "最近 JPEG     : %lu bytes\n"
                       "空闲堆内存    : %lu bytes\n"
                       "AP 名称       : %s\n"
                       "访问地址      : http://%s/\n",
                       (unsigned long)uptime_seconds(),
                       status.wake_reason != NULL ? status.wake_reason : "未知",
                       (unsigned long)s_boot_count,
                       status.led_on ? "亮" : "灭",
                       (unsigned long)status.led_toggles,
                       (unsigned long)status.key_presses,
                       (unsigned int)status.sta_count,
                       status.camera_ready ? "就绪" : "不可用",
                       (unsigned int)status.camera_width,
                       (unsigned int)status.camera_height,
                       (unsigned long)status.camera_frames,
                       (unsigned long)status.camera_errors,
                       (unsigned long)status.camera_last_len,
                       (unsigned long)esp_get_free_heap_size(),
                       WIFI_AP_SSID,
                       WEB_AP_IP);
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

static void web_server_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = WEB_SERVER_PORT;
    config.stack_size = 6144;
    config.max_uri_handlers = 5;
    config.max_open_sockets = 7;

    ESP_ERROR_CHECK(httpd_start(&s_httpd, &config));

    const httpd_uri_t root_uri = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
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
        .handler = stream_get_handler,
        .user_ctx = NULL,
    };

    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd, &root_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd, &status_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd, &logs_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd, &snapshot_uri));
    ESP_ERROR_CHECK(httpd_register_uri_handler(s_httpd, &stream_uri));

    status_log("[http] 网页服务已启动: http://%s/，视频流: http://%s/stream\n", WEB_AP_IP, WEB_AP_IP);
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
    wifi_init_ap();
    web_server_start();

    xTaskCreate(led_task, "led", 2048, NULL, 5, NULL);
    xTaskCreate(power_key_task, "power_key", 2048, NULL, 5, NULL);
    xTaskCreate(monitor_task, "monitor", 2048, NULL, 5, NULL);
}

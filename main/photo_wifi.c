#include "photo_wifi.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"
#include "dns_server.h"

#define WIFI_NVS_NAMESPACE  "openglass"
#define WIFI_NVS_KEY_SSID   "wifi_ssid"
#define WIFI_NVS_KEY_PASS   "wifi_pass"
#define WIFI_RETRY_MAX      5

static const char *TAG = "photo_wifi";

static volatile bool s_sta_connected = false;
static char s_sta_ip[16] = "0.0.0.0";
static int s_retry_count = 0;
static volatile bool s_retry_task_running = false;

static bool load_credentials(char *ssid, size_t ssid_size, char *password, size_t password_size)
{
    nvs_handle_t handle;
    if (nvs_open(WIFI_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }

    bool ok = false;
    size_t len = ssid_size;
    if (nvs_get_str(handle, WIFI_NVS_KEY_SSID, ssid, &len) == ESP_OK && ssid[0] != '\0') {
        len = password_size;
        if (nvs_get_str(handle, WIFI_NVS_KEY_PASS, password, &len) != ESP_OK) {
            password[0] = '\0';
        }
        ok = true;
    }

    nvs_close(handle);
    return ok;
}

bool photo_wifi_has_credentials(void)
{
    char ssid[32] = {0};
    char password[64] = {0};
    return load_credentials(ssid, sizeof(ssid), password, sizeof(password));
}

static void apply_sta_credentials(const char *ssid, const char *password)
{
    wifi_config_t sta_config;
    memset(&sta_config, 0, sizeof(sta_config));
    strncpy((char *)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid) - 1);
    strncpy((char *)sta_config.sta.password, password, sizeof(sta_config.sta.password) - 1);
    sta_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    esp_wifi_disconnect();
    esp_err_t err = esp_wifi_set_config(WIFI_IF_STA, &sta_config);
    if (err == ESP_OK) {
        s_retry_count = 0;
        err = esp_wifi_connect();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "STA setup failed: %s", esp_err_to_name(err));
    }
}

esp_err_t photo_wifi_set_credentials(const char *ssid, const char *password)
{
    if (ssid == NULL || password == NULL || ssid[0] == '\0' || strlen(ssid) >= 32 ||
        strlen(password) >= 64) {
        return ESP_ERR_INVALID_ARG;
    }

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
    if (err != ESP_OK) {
        return err;
    }

    apply_sta_credentials(ssid, password);
    return ESP_OK;
}

static void retry_connect_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(3000));
    s_retry_task_running = false;
    if (!s_sta_connected && s_retry_count < WIFI_RETRY_MAX) {
        s_retry_count++;
        esp_wifi_connect();
    }
    vTaskDelete(NULL);
}

static void wifi_event_handler(void *arg, esp_event_base_t base, int32_t event_id, void *event_data)
{
    if (base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "client joined AP");
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        if (photo_wifi_has_credentials()) {
            esp_wifi_connect();
        }
    } else if (base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_sta_connected = false;
        strcpy(s_sta_ip, "0.0.0.0");
        if (!s_retry_task_running && photo_wifi_has_credentials()) {
            s_retry_task_running = true;
            xTaskCreate(retry_connect_task, "wifi_retry", 2048, NULL, 5, NULL);
        }
    } else if (base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        snprintf(s_sta_ip, sizeof(s_sta_ip), IPSTR, IP2STR(&event->ip_info.ip));
        s_sta_connected = true;
        s_retry_count = 0;
        ESP_LOGI(TAG, "STA connected: %s", s_sta_ip);
    }
}

esp_err_t photo_wifi_init(void)
{
    esp_netif_create_default_wifi_ap();
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&init_config);
    if (err != ESP_OK) {
        return err;
    }

    esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                        &wifi_event_handler, NULL, NULL);
    esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                        &wifi_event_handler, NULL, NULL);

    esp_netif_ip_info_t ap_ip;
    ap_ip.ip.addr = ESP_IP4TOADDR(192, 168, 4, 1);
    ap_ip.netmask.addr = ESP_IP4TOADDR(255, 255, 255, 0);
    ap_ip.gw.addr = ESP_IP4TOADDR(192, 168, 4, 1);
    esp_netif_t *ap_netif = esp_netif_get_handle_from_ifkey("WIFI_AP_DEF");
    if (ap_netif != NULL) {
        esp_netif_set_ip_info(ap_netif, &ap_ip);
    }

    wifi_config_t ap_config;
    memset(&ap_config, 0, sizeof(ap_config));
    strncpy((char *)ap_config.ap.ssid, PHOTO_AP_SSID, sizeof(ap_config.ap.ssid) - 1);
    strncpy((char *)ap_config.ap.password, PHOTO_AP_PASSWORD, sizeof(ap_config.ap.password) - 1);
    ap_config.ap.ssid_len = strlen(PHOTO_AP_SSID);
    ap_config.ap.channel = 1;
    ap_config.ap.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    ap_config.ap.max_connection = 4;
    ap_config.ap.beacon_interval = 100;
    err = esp_wifi_set_config(WIFI_IF_AP, &ap_config);
    if (err != ESP_OK) {
        return err;
    }

    char ssid[32] = {0};
    char password[64] = {0};
    if (load_credentials(ssid, sizeof(ssid), password, sizeof(password))) {
        wifi_config_t sta_config;
        memset(&sta_config, 0, sizeof(sta_config));
        strncpy((char *)sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid) - 1);
        strncpy((char *)sta_config.sta.password, password, sizeof(sta_config.sta.password) - 1);
        sta_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
        esp_wifi_set_config(WIFI_IF_STA, &sta_config);
    }

    err = esp_wifi_set_mode(WIFI_MODE_APSTA);
    if (err != ESP_OK) {
        return err;
    }
    err = esp_wifi_start();
    if (err != ESP_OK) {
        return err;
    }

    esp_ip4_addr_t gateway;
    gateway.addr = ap_ip.ip.addr;
    dns_server_start(gateway);

    ESP_LOGI(TAG, "AP: %s / %s, web: http://%s/", PHOTO_AP_SSID, PHOTO_AP_PASSWORD, PHOTO_AP_IP);
    return ESP_OK;
}

bool photo_wifi_sta_connected(void)
{
    return s_sta_connected;
}

const char *photo_wifi_mode(void)
{
    return s_sta_connected ? "AP+STA" : "AP";
}

void photo_wifi_get_ip(char *buf, size_t len)
{
    strncpy(buf, s_sta_connected ? s_sta_ip : PHOTO_AP_IP, len - 1);
    buf[len - 1] = '\0';
}

void photo_wifi_get_sta_ip(char *buf, size_t len)
{
    strncpy(buf, s_sta_ip, len - 1);
    buf[len - 1] = '\0';
}

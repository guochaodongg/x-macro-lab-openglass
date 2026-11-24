#ifndef PHOTO_WIFI_H
#define PHOTO_WIFI_H

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define PHOTO_AP_SSID       "OpenGlass-Photo"
#define PHOTO_AP_PASSWORD   "openglass123"
#define PHOTO_AP_IP         "192.168.4.1"

esp_err_t photo_wifi_init(void);
bool photo_wifi_sta_connected(void);
const char *photo_wifi_mode(void);
void photo_wifi_get_ip(char *buf, size_t len);
void photo_wifi_get_sta_ip(char *buf, size_t len);
esp_err_t photo_wifi_set_credentials(const char *ssid, const char *password);
bool photo_wifi_has_credentials(void);

#ifdef __cplusplus
}
#endif

#endif

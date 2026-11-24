#include "photo_httpd.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_http_server.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include "photo_display.h"
#include "photo_wifi.h"

extern const char upload_html[];

static const char *TAG = "photo_httpd";
static httpd_handle_t s_httpd = NULL;

static esp_err_t send_text(httpd_req_t *req, const char *text)
{
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, text, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t read_body(httpd_req_t *req, size_t max_len, uint8_t **out_buf, size_t *out_len)
{
    size_t content_len = req->content_len;
    if (content_len == 0 || content_len > max_len) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t *buf = heap_caps_malloc(content_len, MALLOC_CAP_SPIRAM);
    if (buf == NULL) {
        buf = heap_caps_malloc(content_len, MALLOC_CAP_8BIT);
    }
    if (buf == NULL) {
        return ESP_ERR_NO_MEM;
    }

    size_t received = 0;
    while (received < content_len) {
        int ret = httpd_req_recv(req, (char *)buf + received, content_len - received);
        if (ret <= 0) {
            heap_caps_free(buf);
            return ESP_FAIL;
        }
        received += (size_t)ret;
    }

    *out_buf = buf;
    *out_len = received;
    return ESP_OK;
}

static void url_decode(const char *in, char *out, size_t out_size)
{
    size_t o = 0;
    while (*in != '\0' && o + 1 < out_size) {
        if (*in == '+') {
            out[o++] = ' ';
            in++;
        } else if (*in == '%' && in[1] != '\0' && in[2] != '\0') {
            char hex[3] = {in[1], in[2], '\0'};
            out[o++] = (char)strtol(hex, NULL, 16);
            in += 3;
        } else {
            out[o++] = *in++;
        }
    }
    out[o] = '\0';
}

static esp_err_t root_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, upload_html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t upload_post_handler(httpd_req_t *req)
{
    uint8_t *jpeg = NULL;
    size_t len = 0;
    esp_err_t err = read_body(req, PHOTO_MAX_JPEG_BYTES, &jpeg, &len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "reading upload failed: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "400 Bad Request");
        return send_text(req, "图片过大或上传不完整");
    }

    char content_type[64] = {0};
    httpd_req_get_hdr_value_str(req, "Content-Type", content_type, sizeof(content_type));

    const size_t rgb565_len = (size_t)PHOTO_LCD_WIDTH * PHOTO_LCD_HEIGHT * 2;
    if (len == rgb565_len && strstr(content_type, "application/octet-stream") != NULL) {
        err = photo_display_show_rgb565(jpeg, len);
    } else {
        err = photo_display_show_jpeg(jpeg, len);
    }
    heap_caps_free(jpeg);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "showing image failed: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "400 Bad Request");
        return send_text(req, "图片格式无效或内存不足");
    }

    char reply[96];
    snprintf(reply, sizeof(reply), "OK: %u bytes displayed", (unsigned)len);
    return send_text(req, reply);
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    char ip[16] = {0};
    char sta_ip[16] = {0};
    char last[96] = {0};
    photo_wifi_get_ip(ip, sizeof(ip));
    photo_wifi_get_sta_ip(sta_ip, sizeof(sta_ip));
    photo_display_get_info(last, sizeof(last));

    char body[320];
    snprintf(body, sizeof(body),
             "{\"mode\":\"%s\",\"ip\":\"%s\",\"sta\":\"%s\",\"connected\":%s,"
             "\"last\":\"%s\",\"heap\":%u}",
             photo_wifi_mode(), ip, sta_ip,
             photo_wifi_sta_connected() ? "true" : "false",
             last, (unsigned)esp_get_free_heap_size());

    httpd_resp_set_type(req, "application/json; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, body, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t photo_get_handler(httpd_req_t *req)
{
    uint8_t *jpeg = NULL;
    size_t len = 0;
    esp_err_t err = photo_display_copy_jpeg(&jpeg, &len);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "404 Not Found");
        return send_text(req, "尚无照片");
    }

    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    err = httpd_resp_send(req, (const char *)jpeg, len);
    photo_display_free_jpeg(jpeg);
    return err;
}

static esp_err_t wifi_post_handler(httpd_req_t *req)
{
    uint8_t *body = NULL;
    size_t len = 0;
    esp_err_t err = read_body(req, 512, &body, &len);
    if (err != ESP_OK) {
        httpd_resp_set_status(req, "400 Bad Request");
        return send_text(req, "请求无效");
    }

    char raw[513];
    memcpy(raw, body, len);
    raw[len] = '\0';
    heap_caps_free(body);

    char ssid[64] = {0};
    char password[64] = {0};
    const char *ssid_start = strstr(raw, "ssid=");
    const char *pass_start = strstr(raw, "password=");
    if (ssid_start != NULL) {
        ssid_start += 5;
        char tmp[128] = {0};
        size_t n = 0;
        while (ssid_start[n] != '\0' && ssid_start[n] != '&' && n < sizeof(tmp) - 1) {
            tmp[n] = ssid_start[n];
            n++;
        }
        url_decode(tmp, ssid, sizeof(ssid));
    }
    if (pass_start != NULL) {
        pass_start += 9;
        char tmp[128] = {0};
        size_t n = 0;
        while (pass_start[n] != '\0' && pass_start[n] != '&' && n < sizeof(tmp) - 1) {
            tmp[n] = pass_start[n];
            n++;
        }
        url_decode(tmp, password, sizeof(password));
    }

    if (ssid[0] == '\0' || strlen(ssid) >= 32 || strlen(password) >= 64) {
        httpd_resp_set_status(req, "400 Bad Request");
        return send_text(req, "SSID 或密码无效");
    }

    err = photo_wifi_set_credentials(ssid, password);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "saving Wi-Fi failed: %s", esp_err_to_name(err));
        httpd_resp_set_status(req, "500 Internal Server Error");
        return send_text(req, "保存失败，请重试");
    }

    ESP_LOGI(TAG, "Wi-Fi credentials updated for SSID: %s", ssid);
    return send_text(req, "Wi-Fi 配置已保存，正在连接");
}

static esp_err_t captive_handler(httpd_req_t *req)
{
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "/");
    return httpd_resp_send(req, NULL, 0);
}

esp_err_t photo_httpd_start(void)
{
    if (s_httpd != NULL) {
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 16;
    config.stack_size = 8192;
    config.lru_purge_enable = true;

    esp_err_t err = httpd_start(&s_httpd, &config);
    if (err != ESP_OK) {
        return err;
    }

    const httpd_uri_t root_uri = {
        .uri = "/", .method = HTTP_GET, .handler = root_get_handler, .user_ctx = NULL,
    };
    const httpd_uri_t upload_uri = {
        .uri = "/upload", .method = HTTP_POST, .handler = upload_post_handler, .user_ctx = NULL,
    };
    const httpd_uri_t status_uri = {
        .uri = "/status", .method = HTTP_GET, .handler = status_get_handler, .user_ctx = NULL,
    };
    const httpd_uri_t photo_uri = {
        .uri = "/photo", .method = HTTP_GET, .handler = photo_get_handler, .user_ctx = NULL,
    };
    const httpd_uri_t wifi_uri = {
        .uri = "/wifi", .method = HTTP_POST, .handler = wifi_post_handler, .user_ctx = NULL,
    };

    httpd_register_uri_handler(s_httpd, &root_uri);
    httpd_register_uri_handler(s_httpd, &upload_uri);
    httpd_register_uri_handler(s_httpd, &status_uri);
    httpd_register_uri_handler(s_httpd, &photo_uri);
    httpd_register_uri_handler(s_httpd, &wifi_uri);

    static const char *const probes[] = {
        "/hotspot-detect.html", "/library/test/success.html",
        "/generate_204", "/gen_204", "/api/v1/generate_204",
    };
    for (size_t i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        httpd_uri_t probe = {
            .uri = probes[i], .method = HTTP_GET,
            .handler = captive_handler, .user_ctx = NULL,
        };
        httpd_register_uri_handler(s_httpd, &probe);
    }

    char ip[16] = {0};
    photo_wifi_get_ip(ip, sizeof(ip));
    ESP_LOGI(TAG, "web server ready: http://%s/", ip);
    return ESP_OK;
}

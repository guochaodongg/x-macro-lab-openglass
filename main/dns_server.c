/*
 * Minimal DNS server for the SoftAP captive portal.
 * Answers every A query with the gateway address (192.168.4.1), so a phone
 * joining the AP is guided to the upload page.
 */
#include "dns_server.h"

#include <errno.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/netdb.h"
#include "lwip/sockets.h"

static const char *TAG = "dns_server";

static int s_fd = -1;
static volatile bool s_running = false;
static TaskHandle_t s_task_handle = NULL;
static esp_ip4_addr_t s_gateway;

static void dns_task(void *arg)
{
    char buffer[1024];

    while (s_running) {
        struct sockaddr_in client_addr;
        socklen_t client_addr_len = sizeof(client_addr);
        int len = recvfrom(s_fd, buffer, sizeof(buffer), 0,
                           (struct sockaddr *)&client_addr, &client_addr_len);
        if (len < 0) {
            if (!s_running) {
                break;
            }
            ESP_LOGE(TAG, "recvfrom failed, errno=%d", errno);
            continue;
        }
        if (!s_running || len < 12 || len + 16 > (int)sizeof(buffer)) {
            continue;
        }

        buffer[2] |= 0x80;  /* QR: response */
        buffer[3] |= 0x80;  /* RA: recursion available */
        buffer[7] = 1;      /* ANCOUNT = 1 */

        memcpy(&buffer[len], "\xc0\x0c", 2);
        len += 2;
        memcpy(&buffer[len], "\x00\x01\x00\x01\x00\x00\x00\x1c\x00\x04", 10);
        len += 10;
        memcpy(&buffer[len], &s_gateway.addr, 4);
        len += 4;

        sendto(s_fd, buffer, len, 0, (struct sockaddr *)&client_addr, client_addr_len);
    }

    s_task_handle = NULL;
}

void dns_server_start(esp_ip4_addr_t gateway)
{
    if (s_running) {
        return;
    }

    s_gateway = gateway;

    s_fd = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_fd < 0) {
        ESP_LOGE(TAG, "failed to create socket");
        return;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    server_addr.sin_port = htons(53);

    if (bind(s_fd, (struct sockaddr *)&server_addr, sizeof(server_addr)) < 0) {
        ESP_LOGE(TAG, "failed to bind port 53");
        close(s_fd);
        s_fd = -1;
        return;
    }

    s_running = true;
    if (xTaskCreate(dns_task, "dns_server", 3072, NULL, 5, &s_task_handle) != pdPASS) {
        ESP_LOGE(TAG, "failed to create DNS task");
        s_running = false;
        shutdown(s_fd, SHUT_RDWR);
        close(s_fd);
        s_fd = -1;
        return;
    }

    ESP_LOGI(TAG, "started, all queries -> " IPSTR, IP2STR(&s_gateway));
}

void dns_server_stop(void)
{
    if (!s_running) {
        return;
    }

    s_running = false;
    if (s_fd >= 0) {
        shutdown(s_fd, SHUT_RDWR);
        close(s_fd);
        s_fd = -1;
    }
    vTaskDelay(pdMS_TO_TICKS(100));
}

bool dns_server_is_running(void)
{
    return s_running;
}

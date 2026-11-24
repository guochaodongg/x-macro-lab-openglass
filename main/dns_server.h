#ifndef OG_DNS_SERVER_H
#define OG_DNS_SERVER_H

#include <stdbool.h>
#include "esp_netif_ip_addr.h"

#ifdef __cplusplus
extern "C" {
#endif

void dns_server_start(esp_ip4_addr_t gateway);
void dns_server_stop(void);
bool dns_server_is_running(void);

#ifdef __cplusplus
}
#endif

#endif

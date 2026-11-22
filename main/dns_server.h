#ifndef OG_DNS_SERVER_H
#define OG_DNS_SERVER_H

#include <esp_netif_ip_addr.h>
#include <stdbool.h>

void dns_server_start(esp_ip4_addr_t gateway);
void dns_server_stop(void);
bool dns_server_is_running(void);

#endif

#include "user_ext.h"
#include "wifi_ip.h"
#include "esp_netif.h"

unsigned int wifi_sta_ip_get(char *buf, unsigned int len)
{
    esp_netif_t *netif;
    esp_netif_ip_info_t ip_info;

    if((buf == NULL) || (len == 0)) return c_ret_nk;
    netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if(netif == NULL) return c_ret_nk;
    if(esp_netif_get_ip_info(netif, &ip_info) != ESP_OK) return c_ret_nk;
    if(ip_info.ip.addr == 0) return c_ret_nk;
    snprintf(buf, len, IPSTR, IP2STR(&ip_info.ip));
    return c_ret_ok;
}

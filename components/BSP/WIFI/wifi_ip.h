#ifndef __WIFI_IP_H
#define __WIFI_IP_H

#define c_wifi_ip_max   15      /* IPv4字符串最大长度 */

unsigned int wifi_sta_ip_get(char *buf, unsigned int len);  /* 读STA当前IP, 无地址返回nk */

#endif

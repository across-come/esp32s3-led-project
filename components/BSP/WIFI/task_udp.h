#ifndef __TASK_UDP_H
#define __TASK_UDP_H

#include "wifi_ip.h"

#define c_udp_ip              "120.76.240.167"    /* 星空物联网UDP服务器 */
#define c_udp_port            53254
#define c_udp_tmo_ms          5000                /* sendto重试超时(ms) */
#define c_udp_hb_ms           10000               /* keep态心跳间隔, 维持NAT/云端源端口 */
#define c_udp_reconn_min_ms   500                 /* 重连退避起点(ms) */
#define c_udp_reconn_max_ms   8000                /* 重连退避上限(ms) */
#define c_udp_rx_max          64                  /* 单次recv最大字节 */

struct st_udp_arg
{
    char         ip[c_wifi_ip_max + 1];
    unsigned int port;
};

struct st_task_udp_run
{
    struct st_udp_arg     run;
    char                  local_ip[16];
    unsigned int          local_port;
    unsigned int          ok;
    unsigned int          fail;
    unsigned int          busy;
    unsigned int          rx_len;
    char                  rx_hex[29];   /* 收包ASCII显示, 非HEX */
    unsigned int          reconn_n;     /* 连续重连次数, 成功后清零 */
    unsigned int          reconn_ms;    /* 本次退避毫秒 */
};

struct st_task_udp_set
{
    struct st_udp_arg    set;
    unsigned int         setup_req_seq;
    unsigned int         setup_req_seq_bk;
    unsigned int         stop_req_seq;
    unsigned int         stop_req_seq_bk;
};

extern struct st_task_udp_set task_udp_set;

unsigned int task_udp_msg_parse(char *buf);
void task_udp_proc(void);
void task_udp_init(void);

#endif

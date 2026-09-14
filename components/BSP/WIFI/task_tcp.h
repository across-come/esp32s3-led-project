#ifndef __TASK_TCP_H
#define __TASK_TCP_H

#include "wifi_ip.h"

#define c_tcp_ip              "115.120.239.161"
#define c_tcp_port            23157

#define c_tcp_tmo_ms          10000               /* 握手/发送超时, 对齐3x8c WAITCONNECT 10s */
#define c_tcp_hb_ms           10000               /* 应用层心跳间隔 */
#define c_tcp_keepidle_s      10                  /* SO_KEEPALIVE空闲秒, 对齐3x8c SOF_KEEPALIVE */
#define c_tcp_keepintvl_s     5                   /* 探测间隔秒 */
#define c_tcp_keepcnt         3                   /* 探测次数, 无应答则断连 */
#define c_tcp_reconn_min_ms   500                 /* 重连退避起点(ms) */
#define c_tcp_reconn_max_ms   8000                /* 重连退避上限(ms) */
#define c_tcp_rx_max          64                  /* 单次recv最大字节 */
#define c_tcp_fifo_num        128                 /* 在线发送FIFO条数 */
#define c_tcp_fifo_len        64                  /* 单条数据最大长度 */
#define c_tcp_send_tmo_ms     10000               /* 单包发送EAGAIN超时(对齐3x8c send超时) */
#define c_tcp_hs_tmo_ms       10000               /* get/del/WAIT_UPOK 握手超时, 对齐3x8c 10s */
#define c_tcp_rx_loop_max     8                   /* 每tick最多recv次数, 避免占死调度 */
#define c_tcp_cache_max       2100                /* 离线文件一次发送上限, 与SD文件大小独立定义 */

struct st_tcp_arg
{
    char         ip[c_wifi_ip_max + 1];
    unsigned int port;
};

struct st_task_tcp_run
{
    struct st_tcp_arg     run;
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

struct st_task_tcp_set
{
    struct st_tcp_arg    set;
    unsigned int         setup_req_seq;
    unsigned int         setup_req_seq_bk;
    unsigned int         stop_req_seq;
    unsigned int         stop_req_seq_bk;
    unsigned int         send_req_seq;      /* 对齐例程KEY0: 运行中再发一包 */
    unsigned int         send_req_seq_bk;
};

extern struct st_task_tcp_set task_tcp_set;

struct st_tcp_view
{
    unsigned int connecting;
    unsigned int ok;
    unsigned int fail;
    unsigned int hello_sent;
    unsigned int cache_wait;
    char         dst[40];
    char         local[28];
    char         rx[32];
};
const struct st_tcp_view *tcp_view_get(void);

unsigned int task_tcp_msg_parse(char *buf);
void task_tcp_proc(void);
void task_tcp_init(void);

#endif

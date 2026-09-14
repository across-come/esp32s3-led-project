#ifndef __TASK_TCPS_H
#define __TASK_TCPS_H

#include "wifi_ip.h"

#define c_tcps_port     8080                /* 正点原子07_WiFi_TCPServer本地监听端口 */
#define c_tcps_tmo_ms   5000                /* send重试超时(ms); accept一直等直到stop */
#define c_tcps_rx_max   64                  /* 单次recv最大字节 */
#define c_tcps_backlog  4                   /* listen队列, 对齐例程 */

struct st_tcps_arg
{
    unsigned int port;                      /* 本地监听端口, 无远端IP */
};

struct st_task_tcps_run
{
    struct st_tcps_arg    run;
    char                  local_ip[16];
    unsigned int          local_port;
    char                  peer_ip[16];
    unsigned int          peer_port;
    unsigned int          ok;
    unsigned int          fail;
    unsigned int          busy;
    unsigned int          rx_len;
    char                  rx_hex[29];   /* 收包ASCII显示, 非HEX */
};

struct st_task_tcps_set
{
    struct st_tcps_arg   set;
    unsigned int         setup_req_seq;
    unsigned int         setup_req_seq_bk;
    unsigned int         stop_req_seq;
    unsigned int         stop_req_seq_bk;
    unsigned int         send_req_seq;      /* 对齐例程KEY0: 有客户端时再发一包 */
    unsigned int         send_req_seq_bk;
};

extern struct st_task_tcps_set task_tcps_set;

unsigned int task_tcps_msg_parse(char *buf);
void task_tcps_proc(void);
void task_tcps_init(void);

#endif

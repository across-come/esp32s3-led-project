#ifndef __TASK_WIFI_H
#define __TASK_WIFI_H

#include "esp_wifi.h"       /* wifi_ap_record_t */

/* ============ WiFi扫描参数 ============ */
#define c_wifi_ap_max           12      /* 最多显示/保存的AP数 */
#define c_wifi_ssid_max         32      /* SSID最大长度(不含结束符) */
#define c_wifi_pwd_max          64      /* 密码最大长度(不含结束符) */
#define c_wifi_sta_retry_max       20      /* 断开重试上限; STA总等待仍受 c_wifi_sta_tmo_ms */
#define c_wifi_sta_backoff_min_ms  500     /* 第1次重连等待(ms), 之后倍增 */
#define c_wifi_sta_backoff_max_ms  8000    /* 退避上限(ms), 避免对消失的AP狂连 */
#define c_wifi_sta_tmo_ms          30000   /* 连接总超时(ms), 不阻塞等事件组 */
#define c_wifi_scan_tmo_ms      10000   /* 非阻塞扫描等待SCAN_DONE超时(ms) */
#define c_wifi_sc_tmo_ms        90000   /* SmartConfig等手机EspTouch超时(ms) */

/* ============ 命令定义 ============ */
#define c_wifi_cmd_none 0
#define c_wifi_cmd_scan 1               /* 扫描周围WiFi */
#define c_wifi_cmd_sta  2               /* STA连接路由器 */
#define c_wifi_cmd_sc   3               /* SmartConfig一键配网 */

struct st_wifi_arg
{
    unsigned int cmd;
    char         ssid[c_wifi_ssid_max + 1];
    char         pwd[c_wifi_pwd_max + 1];
};

struct st_task_wifi_run
{
    struct st_wifi_arg    run;
    wifi_ap_record_t      ap_info[c_wifi_ap_max];   /* 扫描结果缓存(任务私有) */
    uint16_t              ap_count;                 /* 扫描到的AP数 */
    unsigned int          scan_done;                /* 1=收到SCAN_DONE */
    unsigned int          scan_status;              /* SCAN_DONE的status, 0成功 */
    unsigned int          scan_busy;                /* 1=非阻塞扫描进行中 */

    unsigned int          sta_got_ip;               /* 1=收到GOT_IP */
    unsigned int          sta_fail;                 /* 1=重试耗尽或超时失败 */
    unsigned int          sta_busy;                 /* 1=正在主动连接, 断开才重试 */
    unsigned int          sta_retry;                /* 断开重试次数 */
    unsigned int          sta_reconn;               /* 1=已计划重连, 等退避到期再connect */
    unsigned int          sta_disc_ignore;          /* 1=忽略下一次DISCONNECT(自己调用的disconnect) */
    unsigned int          sta_backoff_tmr;          /* 退避计时起点 */
    unsigned int          sta_backoff_ms;           /* 本次退避毫秒 */
    char                  sta_ip[16];               /* 拿到的IP字符串 */

    unsigned int          sc_busy;                  /* 1=SmartConfig已start, 未stop */
    unsigned int          sc_scan_done;             /* 1=SC_EVENT_SCAN_DONE */
    unsigned int          sc_got_ssid;              /* 1=已拿到SSID/密码 */
    unsigned int          sc_ack_done;              /* 1=SC_EVENT_SEND_ACK_DONE */
    unsigned int          sc_fail;                  /* 1=start/set_config/connect失败 */
};

struct st_task_wifi_set
{
    struct st_wifi_arg   set;
    unsigned int         setup_req_seq;
    unsigned int         setup_req_seq_bk;
    unsigned int         stop_req_seq;
    unsigned int         stop_req_seq_bk;
};

extern struct st_task_wifi_set task_wifi_set;

/* LVGL 只读快照, 任务退出后仍保留扫描/STA结果 */
struct st_wifi_view
{
    unsigned int scanning;
    unsigned int connecting;
    unsigned int got_ip;
    unsigned int fail;
    char         ssid[c_wifi_ssid_max + 1];
    char         ip[16];
    unsigned int ap_count;
    char         ap_ssid[c_wifi_ap_max][c_wifi_ssid_max + 1];
    int          ap_rssi[c_wifi_ap_max];
};
const struct st_wifi_view *wifi_view_get(void);

/* 函数声明 */
unsigned int task_wifi_msg_parse(char *buf);     /* WiFi串口命令解析 */
void task_wifi_proc(void);                       /* WiFi任务: 由task_proc周期调用 */
void wifi_init(void);                            /* WiFi初始化注册 */

#endif

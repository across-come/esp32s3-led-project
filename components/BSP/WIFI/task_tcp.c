#include "user_ext.h"
#include "task_tcp.h"
#include "esp_netif.h"
#include "esp_log.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include "task_lvgl.h"
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>

struct st_task_tcp_set task_tcp_set = {0};
static struct st_tcp_view s_tcp_view = {0};

const struct st_tcp_view *tcp_view_get(void)
{
    return &s_tcp_view;
}

static const char *TAG = "tcp";

static unsigned int s_send_tmr = 0;              /* 发送超时计时(EAGAIN保护) */

static int s_tcp_sock = -1;                      /* -1未打开; >=0 为lwIP socket fd */
static unsigned int s_tcp_ready = 0;             /* 1=HELLO已发出, 可上传 */
static struct sockaddr_in s_tcp_dest;            /* connect目标: TCPServer IP/端口, 网络字节序 */
static const char c_tcp_txt[] = "ALIENTEK DATA \r\n";   /* 网页ASCII, 对齐正点原子 */
static unsigned int s_tcp_lcd_pend = 0;          /* 1=有新数据待刷LCD, 等task_lcd空闲 */

/* ============ 发送FIFO (仅在线走RAM; 断网写穿CACHE) ============ */
static char s_tcp_fifo[c_tcp_fifo_num][c_tcp_fifo_len];
static unsigned int s_fifo_head = 0;
static unsigned int s_fifo_tail = 0;
static unsigned int s_fifo_cnt = 0;
static unsigned int s_fifo_off = 0;              /* 当前FIFO头部分发送偏移 */
static unsigned int s_tx_off = 0;                /* HELLO/心跳/KEY0 部分发送偏移 */

static char s_cache_tx[c_tcp_cache_max];         /* SD cache:ready 消息里拷来的整文件 */
static unsigned int s_cache_len = 0;
static unsigned int s_cache_off = 0;
static unsigned int s_cache_busy = 0;
static unsigned int s_cache_wait = 0;            /* 已发 get, 等 cache:ready */
static unsigned int s_await_del = 0;             /* 已发 del, 等 pending/empty */
static unsigned int s_sd_pending = 0;            /* SD 通知有积压文件 */
static unsigned int s_hs_tmr = 0;                /* get/del/ack 握手计时 */

/* 对齐3x8c WAIT_UPOK: 本地send成功不算完成, 等对端回声后再推进队列 */
#define c_ack_none    0
#define c_ack_cache   1
#define c_ack_fifo    2
static unsigned int s_ack_kind = 0;
static const char *s_ack_ptr = NULL;
static unsigned int s_ack_len = 0;
static unsigned int s_ack_got = 0;

static unsigned int tcp_data_push(const char *data, unsigned int len)
{
    if((data == NULL) || (len == 0) || (len >= c_tcp_fifo_len))
        return c_ret_nk;
    if(s_fifo_cnt >= c_tcp_fifo_num)
        return c_ret_nk;                        /* 满: 调用方落盘 */
    memcpy(s_tcp_fifo[s_fifo_tail], data, len);
    s_tcp_fifo[s_fifo_tail][len] = 0;
    s_fifo_tail = (s_fifo_tail + 1) % c_tcp_fifo_num;
    s_fifo_cnt++;
    return c_ret_ok;
}

/* FIFO全部交给SD任务落盘 (msg排队, 不在TCP里直接写卡) */
static void tcp_sd_ensure(void)
{
    if(task_get("task_sd") != NULL)
        return;
    dbgtx("tcp sd setup\r\n");
    msg_send("task_sd_msg", "setup");
}

static void tcp_sd_append(const char *line)
{
    char msg[c_tcp_fifo_len + 8];

    if((line == NULL) || (line[0] == 0))
        return;
    tcp_sd_ensure();
    snprintf(msg, sizeof(msg), "append:%s", line);
    msg_send("task_sd_msg", msg);
}

static void tcp_fifo_flush_cache(void)
{
    while(s_fifo_cnt > 0)
    {
        tcp_sd_append(s_tcp_fifo[s_fifo_head]);
        s_fifo_head = (s_fifo_head + 1) % c_tcp_fifo_num;
        s_fifo_cnt--;
    }
    s_fifo_off = 0;
}

static void tcp_ack_clear(void)
{
    s_ack_kind = c_ack_none;
    s_ack_ptr = NULL;
    s_ack_len = 0;
    s_ack_got = 0;
}

static void tcp_ack_begin(unsigned int kind, const char *buf, unsigned int len)
{
    s_ack_kind = kind;
    s_ack_ptr = buf;
    s_ack_len = len;
    s_ack_got = 0;
    s_hs_tmr = tick_get();
}

static void tcp_ack_feed(const unsigned char *rx, int n)
{
    int i;

    if((s_ack_kind == c_ack_none) || (s_ack_ptr == NULL) || (n <= 0))
        return;
    for(i = 0; i < n; i++)
    {
        if(s_ack_got >= s_ack_len)
            break;
        if((char)rx[i] == s_ack_ptr[s_ack_got])
            s_ack_got++;
        else if((char)rx[i] == s_ack_ptr[0])
            s_ack_got = 1;
        else
            s_ack_got = 0;
    }
}

static unsigned int tcp_ack_done(void)
{
    return ((s_ack_kind != c_ack_none) && (s_ack_len > 0) && (s_ack_got >= s_ack_len))
           ? c_ret_ok : c_ret_nk;
}

static void tcp_tx_reset(void)
{
    /* 未确认的CACHE仍在卡上, 重连后必须再 get; 未确认的FIFO由 flush 落盘 */
    if((s_cache_busy) || (s_ack_kind == c_ack_cache) ||
       (s_await_del) || (s_cache_wait))
        s_sd_pending = 1;
    s_tx_off = 0;
    s_fifo_off = 0;
    s_cache_busy = 0;
    s_cache_len = 0;
    s_cache_off = 0;
    s_cache_wait = 0;
    s_await_del = 0;
    s_tcp_ready = 0;
    tcp_ack_clear();
}

static void tcp_lcd_show(struct st_task_tcp_run *p_task_arg, unsigned int flag);
static void tcp_lcd_show_rx(struct st_task_tcp_run *p_task_arg);

static void tcp_ascii_put(char *dst, unsigned int dst_sz, const unsigned char *src, int len)
{
    unsigned int i;
    unsigned int n;

    if((dst == NULL) || (dst_sz == 0)) return;
    n = (len < 0) ? 0 : (unsigned int)len;
    if(n >= dst_sz) n = dst_sz - 1;
    for(i = 0; i < n; i++)
    {
        unsigned char c = src[i];
        if((c == '\r') || (c == '\n') || (c == ',')) dst[i] = ' ';
        else if((c >= 0x20) && (c < 0x7F)) dst[i] = (char)c;
        else dst[i] = '.';
    }
    dst[n] = 0;
}

/* ======================================================================
 * TCP任务 串口指令解析
 * 对齐正点原子06_WiFi_TCPClient: 板子是TCPClient, 对端是TCPServer
 * send 对应例程 KEY0 置发送标志 (不另开FreeRTOS发送线程)
 * ======================================================================
 * exe=dbg_msg_send(msg:task_tcp_msg,dat:help)
 * exe=dbg_msg_send(msg:task_tcp_msg,dat:setup)
 * exe=dbg_msg_send(msg:task_tcp_msg,dat:stop)
 * exe=dbg_msg_send(msg:task_tcp_msg,dat:send)
 * exe=dbg_msg_send(msg:task_tcp_msg,dat:tcp?)
 * exe=dbg_msg_send(msg:task_tcp_msg,dat:tcp:ip=115.120.239.161,port=27362)
 */
unsigned int task_tcp_msg_parse(char *buf)
{
    char ip[c_wifi_ip_max + 1];
    unsigned int port;

    if(buf == NULL) return c_ret_nk;

    if(strcmp(buf, "help") == 0)
    {
        dbgtx("setup  (keep+hb until stop)\r\n");
        dbgtx("stop\r\n");
        dbgtx("send\r\n");
        dbgtx("tcp?\r\n");
        dbgtx("tcp:ip=ddd.ddd.ddd.ddd,port=ddddd\r\n");
        dbgtx("push:<line>\r\n");
        return c_ret_ok;
    }
    else if(strncmp(buf, "push:", 5) == 0)
    {
        const char *pay = buf + 5;
        unsigned int len = (unsigned int)strlen(pay);

        if((pay[0] == 0) || (len >= c_tcp_fifo_len))
            return c_ret_nk;
        if(s_tcp_ready)
        {
            if(tcp_data_push(pay, len) != c_ret_ok)
                tcp_sd_append(pay);
        }
        else
            tcp_sd_append(pay);
        return c_ret_ok;
    }
    else if(strcmp(buf, "cache:pending") == 0)
    {
        s_sd_pending = 1;
        s_await_del = 0;
        return c_ret_ok;
    }
    else if(strcmp(buf, "cache:empty") == 0)
    {
        s_sd_pending = 0;
        s_cache_wait = 0;
        s_await_del = 0;
        return c_ret_ok;
    }
    else if(strcmp(buf, "cache:fail") == 0)
    {
        s_sd_pending = 0;
        s_cache_wait = 0;
        s_await_del = 0;
        dbgtx("tcp sd fail\r\n");
        ESP_LOGE(TAG, "sd fail");
        return c_ret_ok;
    }
    else if(strncmp(buf, "cache:ready", 11) == 0)
    {
        const char *pay = buf + 11;
        unsigned int len;

        if(*pay == ':')
            pay++;
        len = (unsigned int)strlen(pay);
        if((len == 0) || (len >= sizeof(s_cache_tx)))
        {
            s_cache_wait = 0;
            msg_send("task_sd_msg", "del");
            s_await_del = 1;
            return c_ret_ok;
        }
        memcpy(s_cache_tx, pay, len);
        s_cache_tx[len] = 0;
        s_cache_len = len;
        s_cache_off = 0;
        s_cache_busy = 1;
        s_cache_wait = 0;
        s_sd_pending = 1;
        ESP_LOGI(TAG, "cache ready len=%u", s_cache_len);
        return c_ret_ok;
    }
    else if(strcmp(buf, "tcp?") == 0)
    {
        memset(task_tcp_set.set.ip, 0, sizeof(task_tcp_set.set.ip));
        snprintf(task_tcp_set.set.ip, sizeof(task_tcp_set.set.ip), "%s", c_tcp_ip);
        task_tcp_set.set.port = c_tcp_port;
        dbgtx("tcp ip=%s port=%u\r\n", task_tcp_set.set.ip, task_tcp_set.set.port);
        return c_ret_ok;
    }
    else if(sscanf(buf, "tcp:ip=%15[^,],port=%u", ip, &port) == 2)
    {
        memset(task_tcp_set.set.ip, 0, sizeof(task_tcp_set.set.ip));
        strncpy(task_tcp_set.set.ip, ip, c_wifi_ip_max);
        task_tcp_set.set.port = port;
        dbgtx("tcp ip=%s port=%u\r\n", task_tcp_set.set.ip, task_tcp_set.set.port);
        return c_ret_ok;
    }
    else if(strcmp(buf, "setup") == 0)
    {
        if(task_get("task_tcp") != NULL)
        {
            dbgtx("task_tcp is running, cmd NK\r\n");
            return c_ret_ok;
        }
        if(task_tcp_set.set.ip[0] == 0)
        {
            snprintf(task_tcp_set.set.ip, sizeof(task_tcp_set.set.ip), "%s", c_tcp_ip);
            task_tcp_set.set.port = c_tcp_port;
        }
        task_tcp_set.setup_req_seq++;
        task_add("task_tcp", task_tcp_proc, c_auto_quit, sizeof(struct st_task_tcp_run));
        return c_ret_ok;
    }
    else if(strcmp(buf, "send") == 0)
    {
        if(task_get("task_tcp") == NULL) return c_ret_nk;
        task_tcp_set.send_req_seq++;            /* keep里消费, 对齐KEY0 */
        return c_ret_ok;
    }
    else if(strcmp(buf, "stop") == 0)
    {
        if(task_get("task_tcp") == NULL) return c_ret_nk;
        task_tcp_set.stop_req_seq++;
        return c_ret_ok;
    }
    return c_ret_nk;
}

static void tcp_abort(void)
{
    struct linger lg;

    if(s_tcp_sock >= 0)
    {
        /* 对齐3x8c tcp_abort: linger0发RST, 避免TIME_WAIT占源端口 */
        lg.l_onoff = 1;
        lg.l_linger = 0;
        setsockopt(s_tcp_sock, SOL_SOCKET, SO_LINGER, &lg, sizeof(lg));
        close(s_tcp_sock);
        s_tcp_sock = -1;
    }
    s_tcp_lcd_pend = 0;
    tcp_tx_reset();
}

/* 对齐3x8c: SOF_KEEPALIVE + tcp_nagle_disable; 半开连接由内核探测 */
static void tcp_sock_protect(int fd)
{
    int on = 1;
    int idle = c_tcp_keepidle_s;
    int intvl = c_tcp_keepintvl_s;
    int cnt = c_tcp_keepcnt;

    if(fd < 0) return;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &on, sizeof(on));
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
}

/* 对齐3x8c uploadTask里 s_eth_link_up: 没链路立刻放弃本次发送 */
static unsigned int tcp_sta_alive(void)
{
    char ip[16];
    return wifi_sta_ip_get(ip, sizeof(ip));
}

static unsigned int tcp_backoff_ms(unsigned int n)
{
    unsigned int ms = c_tcp_reconn_min_ms;
    unsigned int i;

    for(i = 1; i < n; i++)
    {
        if(ms >= (c_tcp_reconn_max_ms / 2))
            return c_tcp_reconn_max_ms;
        ms *= 2;
    }
    return ms;
}

/* 关socket, 算退避; step由proc切到c_step_reconn */
static void tcp_reconn_begin(struct st_task_tcp_run *p_task_arg, const char *msg)
{
    if(p_task_arg == NULL) return;
    tcp_fifo_flush_cache();                     /* 断网保护: FIFO全部落盘, 不丢数据 */
    tcp_abort();
    p_task_arg->ok = 0;
    p_task_arg->fail = 0;
    p_task_arg->busy = 0;
    if(p_task_arg->reconn_n < 16)
        p_task_arg->reconn_n++;
    p_task_arg->reconn_ms = tcp_backoff_ms(p_task_arg->reconn_n);
    sys.curr_task->tmr = tick_get();
    if(msg != NULL)
        dbgtx("%s, wait=%ums n=%u\r\n", msg, p_task_arg->reconn_ms, p_task_arg->reconn_n);
    else
        dbgtx("tcp reconn wait=%ums n=%u\r\n", p_task_arg->reconn_ms, p_task_arg->reconn_n);
}

static void tcp_fail(struct st_task_tcp_run *p_task_arg, const char *msg)
{
    if(p_task_arg == NULL) return;
    p_task_arg->busy = 0;
    p_task_arg->ok = 0;
    p_task_arg->fail = 1;
    tcp_fifo_flush_cache();
    tcp_abort();
    if(msg != NULL) dbgtx("%s\r\n", msg);
}

/* 非阻塞SOCK_STREAM, 发起connect(可能EINPROGRESS), 失败置fail */
static unsigned int tcp_open(struct st_task_tcp_run *p_task_arg)
{
    int flags;
    int ret;
    int err;

    if(p_task_arg == NULL) return c_ret_nk;

    p_task_arg->ok = 0;
    p_task_arg->fail = 0;
    p_task_arg->busy = 0;
    p_task_arg->local_port = 0;
    p_task_arg->local_ip[0] = 0;
    p_task_arg->rx_len = 0;
    p_task_arg->rx_hex[0] = 0;
    tcp_abort();

    /* 无STA则不置fail, 由proc进reconn */
    if(wifi_sta_ip_get(p_task_arg->local_ip, sizeof(p_task_arg->local_ip)) != c_ret_ok)
    {
        dbgtx("tcp no sta ip, connect wifi first\r\n");
        tcp_abort();
        return c_ret_nk;
    }
    if((p_task_arg->run.ip[0] == 0) || (p_task_arg->run.port == 0))
    {
        tcp_fail(p_task_arg, "tcp ip/port empty");
        return c_ret_nk;
    }

    memset(&s_tcp_dest, 0, sizeof(s_tcp_dest));
    s_tcp_dest.sin_family = AF_INET;
    s_tcp_dest.sin_port = htons((uint16_t)p_task_arg->run.port);
    s_tcp_dest.sin_addr.s_addr = inet_addr(p_task_arg->run.ip);
    if(s_tcp_dest.sin_addr.s_addr == INADDR_NONE)
    {
        dbgtx("tcp inet_addr err ip=%s\r\n", p_task_arg->run.ip);
        tcp_fail(p_task_arg, NULL);
        return c_ret_nk;
    }

    /* SOCK_STREAM=TCP可靠字节流; 协议0=由类型推断. 例程用阻塞connect, 这里非阻塞 */
    s_tcp_sock = socket(AF_INET, SOCK_STREAM, 0);
    if(s_tcp_sock < 0)
    {
        dbgtx("tcp socket err\r\n");
        tcp_abort();
        return c_ret_nk;
    }
    tcp_sock_protect(s_tcp_sock);
    flags = fcntl(s_tcp_sock, F_GETFL, 0);
    if(flags < 0) flags = 0;
    fcntl(s_tcp_sock, F_SETFL, flags | O_NONBLOCK);

    /* 非阻塞connect立刻返回: 0=已连上; EINPROGRESS=握手中, 到conn步poll */
    ret = connect(s_tcp_sock, (struct sockaddr *)&s_tcp_dest, sizeof(s_tcp_dest));
    if(ret != 0)
    {
        err = errno;
        if((err != EINPROGRESS) && (err != EALREADY) &&
           (err != EAGAIN) && (err != EWOULDBLOCK))
        {
            dbgtx("tcp connect err errno=%d\r\n", err);
            tcp_abort();
            return c_ret_nk;
        }
    }

    dbgtx("tcp connecting %s:%u local=%s\r\n",
          p_task_arg->run.ip, p_task_arg->run.port, p_task_arg->local_ip);
    return c_ret_ok;
}

/* 等三次握手完成: select看可写, SO_ERROR看结果. ok已连上 wt继续等 nk失败 */
static unsigned int tcp_conn_poll(struct st_task_tcp_run *p_task_arg)
{
    fd_set wset;
    struct timeval tv;
    struct sockaddr_in local_addr;
    socklen_t slen;
    int err;
    socklen_t elen;
    int ret;

    if((p_task_arg == NULL) || (s_tcp_sock < 0)) return c_ret_nk;

    FD_ZERO(&wset);
    FD_SET(s_tcp_sock, &wset);
    tv.tv_sec = 0;
    tv.tv_usec = 0;                                 /* 0超时: 不阻塞本tick */
    ret = select(s_tcp_sock + 1, NULL, &wset, NULL, &tv);
    if(ret < 0) return c_ret_nk;
    if(ret == 0) return c_ret_wt;                   /* 还不可写, 握手未完 */
    if(!FD_ISSET(s_tcp_sock, &wset)) return c_ret_wt;

    /* 可写后必须读SO_ERROR: 拒连/超时也表现为可写 */
    elen = sizeof(err);
    err = 0;
    if(getsockopt(s_tcp_sock, SOL_SOCKET, SO_ERROR, &err, &elen) != 0)
        return c_ret_nk;
    if((err == EINPROGRESS) || (err == EALREADY)) return c_ret_wt;
    if(err != 0)
    {
        dbgtx("tcp connect err errno=%d\r\n", err);
        return c_ret_nk;
    }

    slen = sizeof(local_addr);
    memset(&local_addr, 0, sizeof(local_addr));
    if(getsockname(s_tcp_sock, (struct sockaddr *)&local_addr, &slen) == 0)
        p_task_arg->local_port = ntohs(local_addr.sin_port);
    return c_ret_ok;
}

/* 发完整个buf. *off为已发字节, 未完成保持偏移下次续发. 短写不算成功 */
static unsigned int tcp_send_all(const char *buf, unsigned int len, unsigned int *off)
{
    int ret;
    int err;
    unsigned int remain;

    if((s_tcp_sock < 0) || (buf == NULL) || (len == 0) || (off == NULL))
        return c_ret_nk;
    if(*off >= len)
    {
        *off = 0;
        return c_ret_ok;
    }
    remain = len - *off;
    ret = send(s_tcp_sock, buf + *off, (size_t)remain, 0);
    if(ret > 0)
    {
        *off += (unsigned int)ret;
        if(*off >= len)
        {
            *off = 0;
            return c_ret_ok;
        }
        return c_ret_wt;
    }
    err = errno;
    if((ret == 0) || (err == EAGAIN) || (err == EWOULDBLOCK) ||
       (err == EINPROGRESS) || (err == ENOMEM))
        return c_ret_wt;
    dbgtx("tcp send err errno=%d\r\n", err);
    return c_ret_nk;
}

static unsigned int tcp_send(void)
{
    return tcp_send_all(c_tcp_txt, sizeof(c_tcp_txt) - 1, &s_tx_off);
}

static void tcp_lcd_show(struct st_task_tcp_run *p_task_arg, unsigned int flag)
{
    char msg[96];
    char show[22];

    if(p_task_arg == NULL) return;

    snprintf(s_tcp_view.dst, sizeof(s_tcp_view.dst), "%s:%u", p_task_arg->run.ip, p_task_arg->run.port);
    snprintf(s_tcp_view.local, sizeof(s_tcp_view.local), "%s:%u", p_task_arg->local_ip, p_task_arg->local_port);
    s_tcp_view.connecting = (flag == 0);
    s_tcp_view.fail = ((flag == 1) || (flag == 3));
    s_tcp_view.ok = (flag == 2);
    s_tcp_view.hello_sent = (flag == 2);
    if(flag == 4)
    {
        s_tcp_view.ok = 0;
        s_tcp_view.hello_sent = 0;
        s_tcp_view.connecting = 0;
        s_tcp_view.fail = 0;
    }
    s_tcp_view.cache_wait = (s_cache_busy || s_cache_wait || s_await_del);
    if(lvgl_ui_ready())
        return;

    if(flag == 0)
        msg_send("task_lcd_msg", "fill:x=0,y=0,w=320,h=240,color=0x0000");
    else
        msg_send("task_lcd_msg", "fill:x=0,y=24,w=320,h=216,color=0x0000");
    msg_send("task_lcd_msg", "str:x=10,y=5,size=16,color=0xffff,bk=0x0000,txt=TCP:");

    if(flag == 0)
    {
        msg_send("task_lcd_msg", "str:x=10,y=40,size=16,color=0x07ff,bk=0x0000,txt=tcp connecting......");
    }
    else if(flag == 1)
    {
        msg_send("task_lcd_msg", "str:x=10,y=40,size=16,color=0xf800,bk=0x0000,txt=tcp connect fail");
    }
    else if(flag == 3)
    {
        /* 链路已通, 只是 WAIT_UPOK 没等到回声, 不是握手失败 */
        msg_send("task_lcd_msg", "str:x=10,y=40,size=16,color=0xf800,bk=0x0000,txt=tcp ack timeout");
    }
    else if(flag == 4)
    {
        msg_send("task_lcd_msg", "str:x=10,y=40,size=16,color=0xffe0,bk=0x0000,txt=tcp stopped");
        msg_send("task_lcd_msg", "str:x=10,y=60,size=16,color=0xffff,bk=0x0000,txt=offline, data to SD");
        msg_send("task_lcd_msg", "str:x=10,y=80,size=16,color=0x07ff,bk=0x0000,txt=KEY2 long to connect");
    }
    else
    {
        sprintf(msg, "str:x=10,y=40,size=16,color=0x07e0,bk=0x0000,txt=ip:%s", p_task_arg->local_ip);
        msg_send("task_lcd_msg", msg);

        memset(show, 0, sizeof(show));
        strncpy(show, p_task_arg->run.ip, sizeof(show) - 1);
        sprintf(msg, "str:x=10,y=60,size=16,color=0x001f,bk=0x0000,txt=dst:%s", show);
        msg_send("task_lcd_msg", msg);

        sprintf(msg, "str:x=10,y=80,size=16,color=0x001f,bk=0x0000,txt=port:%u lport:%u",
                p_task_arg->run.port, p_task_arg->local_port);
        msg_send("task_lcd_msg", msg);

        msg_send("task_lcd_msg", "str:x=10,y=100,size=16,color=0x07e0,bk=0x0000,txt=sent:ALIENTEK DATA");
        msg_send("task_lcd_msg", "str:x=10,y=120,size=16,color=0x07ff,bk=0x0000,txt=wait rx...");
    }
    msg_send("task_lcd_msg", "setup_done");
}

static void tcp_lcd_show_rx(struct st_task_tcp_run *p_task_arg)
{
    char msg[96];
    char txt[32];

    if((p_task_arg == NULL) || (p_task_arg->rx_hex[0] == 0)) return;
    memset(s_tcp_view.rx, 0, sizeof(s_tcp_view.rx));
    strncpy(s_tcp_view.rx, p_task_arg->rx_hex, sizeof(s_tcp_view.rx) - 1);
    if(lvgl_ui_ready())
        return;
    if(task_get("task_lcd") != NULL) return;

    msg_send("task_lcd_msg", "fill:x=0,y=120,w=320,h=40,color=0x0000");
    memset(txt, 0, sizeof(txt));
    snprintf(txt, sizeof(txt), "rx:%s", p_task_arg->rx_hex);
    sprintf(msg, "str:x=10,y=120,size=16,color=0x07e0,bk=0x0000,txt=%s", txt);
    msg_send("task_lcd_msg", msg);
    sprintf(msg, "str:x=10,y=140,size=16,color=0x07e0,bk=0x0000,txt=len:%u", p_task_arg->rx_len);
    msg_send("task_lcd_msg", msg);
    msg_send("task_lcd_msg", "setup_done");
}

/* 非阻塞抽干recv. 返回ok继续, nk对端关闭或出错 */
static unsigned int tcp_rx(struct st_task_tcp_run *p_task_arg)
{
    unsigned char rxbuf[c_tcp_rx_max];
    char ascii[33];
    int ret;
    int err;
    unsigned int nloop;

    if((p_task_arg == NULL) || (s_tcp_sock < 0)) return c_ret_nk;

    nloop = 0;
    while(nloop < c_tcp_rx_loop_max)
    {
        nloop++;
        ret = recv(s_tcp_sock, rxbuf, sizeof(rxbuf), 0);
        if(ret == 0)
        {
            dbgtx("tcp peer close\r\n");
            return c_ret_nk;
        }
        if(ret < 0)
        {
            err = errno;
            if((err == EAGAIN) || (err == EWOULDBLOCK)) break;
            dbgtx("tcp recv err errno=%d\r\n", err);
            return c_ret_nk;
        }

        tcp_ascii_put(ascii, sizeof(ascii), rxbuf, ret);
        dbgtx("tcp rx len=%d ascii=%s\r\n", ret, ascii);

        tcp_ascii_put(p_task_arg->rx_hex, sizeof(p_task_arg->rx_hex), rxbuf, ret);
        p_task_arg->rx_len = (unsigned int)ret;
        s_tcp_lcd_pend = 1;
        tcp_ack_feed(rxbuf, ret);
    }

    if(s_tcp_lcd_pend && (task_get("task_lcd") == NULL))
    {
        tcp_lcd_show_rx(p_task_arg);
        s_tcp_lcd_pend = 0;
    }
    return c_ret_ok;
}

/* ======================================================================
 * TCP任务: idle建socket -> conn握手 -> send -> show -> keep(收包+心跳)
 * keep里对端关/STA没了/发送失败 -> reconn退避后重开, 直到stop
 * ====================================================================== */
void task_tcp_proc(void)
{
    #define c_step_idle    0
    #define c_step_conn    1
    #define c_step_send    2
    #define c_step_show    3
    #define c_step_keep    4
    #define c_step_reconn  5
    struct st_task_tcp_run *p_task_arg;
    unsigned int ret;

    p_task_arg = (struct st_task_tcp_run *)sys.curr_task->dat;

    if(task_tcp_set.stop_req_seq_bk != task_tcp_set.stop_req_seq)
    {
        task_tcp_set.stop_req_seq_bk = task_tcp_set.stop_req_seq;
        tcp_fifo_flush_cache();                 /* 未发出的FIFO落盘 */
        tcp_sd_ensure();
        tcp_abort();                            /* 关socket, 不再自动重连 */
        s_sd_pending = 1;                       /* 卡上/队列里的积压, 下次setup补发 */
        tcp_lcd_show(p_task_arg, 4);
        dbgtx("tcp stopped, socket closed, wait KEY2\r\n");
        sys.curr_task->step = c_step_idle;
        sys.curr_task->done = c_done_nk;
        return;
    }
    switch(sys.curr_task->step)
    {
        case c_step_idle:
            if(task_tcp_set.setup_req_seq_bk == task_tcp_set.setup_req_seq) return;
            task_tcp_set.setup_req_seq_bk = task_tcp_set.setup_req_seq;
            p_task_arg->run = task_tcp_set.set;
            p_task_arg->reconn_n = 0;
            if((p_task_arg->run.ip[0] == 0) || (p_task_arg->run.port == 0))
            {
                tcp_fail(p_task_arg, "tcp ip/port empty");
                sys.curr_task->step = c_step_show;
                return;
            }
            if(tcp_open(p_task_arg) != c_ret_ok)
            {
                if(p_task_arg->fail)
                {
                    sys.curr_task->step = c_step_show;
                    return;
                }
                tcp_reconn_begin(p_task_arg, "tcp open fail");
                tcp_lcd_show(p_task_arg, 1);
                sys.curr_task->step = c_step_reconn;
                return;
            }
            p_task_arg->busy = 1;
            tcp_lcd_show(p_task_arg, 0);
            sys.curr_task->tmr = tick_get();
            sys.curr_task->step = c_step_conn;
        break;

        case c_step_conn:
            if(tcp_sta_alive() != c_ret_ok)
            {
                tcp_reconn_begin(p_task_arg, "tcp sta lost");
                tcp_lcd_show(p_task_arg, 1);
                sys.curr_task->step = c_step_reconn;
                return;
            }
            ret = tcp_conn_poll(p_task_arg);
            if(ret == c_ret_wt)
            {
                if(tick_cmp(sys.curr_task->tmr, c_tcp_tmo_ms) == c_ret_ok)
                {
                    tcp_reconn_begin(p_task_arg, "tcp connect timeout");
                    tcp_lcd_show(p_task_arg, 1);
                    sys.curr_task->step = c_step_reconn;
                }
                return;
            }
            if(ret != c_ret_ok)
            {
                tcp_reconn_begin(p_task_arg, "tcp connect fail");
                tcp_lcd_show(p_task_arg, 1);
                sys.curr_task->step = c_step_reconn;
                return;
            }
            dbgtx("tcp connected dst=%s:%u local=%s:%u\r\n",
                  p_task_arg->run.ip, p_task_arg->run.port,
                  p_task_arg->local_ip, p_task_arg->local_port);
            s_tx_off = 0;
            sys.curr_task->tmr = tick_get();
            sys.curr_task->step = c_step_send;
        break;

        case c_step_send:
            if(tcp_sta_alive() != c_ret_ok)
            {
                tcp_reconn_begin(p_task_arg, "tcp sta lost");
                tcp_lcd_show(p_task_arg, 1);
                sys.curr_task->step = c_step_reconn;
                return;
            }
            {
                static char hello[80];
                static int hlen;

                if(s_tx_off == 0)
                {
                    hlen = snprintf(hello, sizeof(hello), "[HELLO %s:%u] %s\r\n",
                                    p_task_arg->run.ip, p_task_arg->run.port, p_task_arg->local_ip);
                    if(hlen < 0)
                        hlen = 0;
                }
                ret = tcp_send_all(hello, (unsigned int)hlen, &s_tx_off);
            }
            if(ret == c_ret_wt)
            {
                if(tick_cmp(sys.curr_task->tmr, c_tcp_tmo_ms) == c_ret_ok)
                {
                    tcp_reconn_begin(p_task_arg, "tcp send timeout");
                    tcp_lcd_show(p_task_arg, 1);
                    sys.curr_task->step = c_step_reconn;
                }
                return;
            }
            if(ret != c_ret_ok)
            {
                tcp_reconn_begin(p_task_arg, "tcp send err");
                tcp_lcd_show(p_task_arg, 1);
                sys.curr_task->step = c_step_reconn;
                return;
            }
            p_task_arg->busy = 0;
            p_task_arg->ok = 1;
            p_task_arg->reconn_n = 0;
            s_tcp_ready = 1;
            s_sd_pending = 1;                   /* 每次连上先问SD有没有积压 */
            s_send_tmr = tick_get();
            dbgtx("tcp ok dst=%s:%u local=%s:%u hello sent\r\n",
                  p_task_arg->run.ip, p_task_arg->run.port,
                  p_task_arg->local_ip, p_task_arg->local_port);
            sys.curr_task->step = c_step_show;
        break;

        case c_step_show:
            if(p_task_arg->ok)
            {
                tcp_lcd_show(p_task_arg, 2);
                dbgtx("tcp display done, keep hb\r\n");
                sys.curr_task->tmr = tick_get();
                sys.curr_task->step = c_step_keep;
                return;
            }
            tcp_lcd_show(p_task_arg, 1);
            dbgtx("tcp display done\r\n");
            sys.curr_task->step = c_step_idle;
            sys.curr_task->done = c_done_ok;        /* 仅配置错误才退出 */
        break;

        case c_step_keep:
            if(tcp_sta_alive() != c_ret_ok)
            {
                tcp_reconn_begin(p_task_arg, "tcp sta lost");
                tcp_lcd_show(p_task_arg, 1);
                sys.curr_task->step = c_step_reconn;
                return;
            }
            if(task_tcp_set.send_req_seq_bk != task_tcp_set.send_req_seq)
            {
                if((s_ack_kind != c_ack_none) || s_cache_busy || s_await_del || s_cache_wait)
                    goto tcp_keep_upload;       /* 有在途报文时不插KEY0, 避免打乱WAIT_UPOK */
                ret = tcp_send();
                if(ret == c_ret_wt)
                {
                    if(tick_cmp(s_send_tmr, c_tcp_send_tmo_ms) == c_ret_ok)
                    {
                        tcp_reconn_begin(p_task_arg, "tcp send timeout");
                        tcp_lcd_show(p_task_arg, 1);
                        sys.curr_task->step = c_step_reconn;
                    }
                    return;
                }
                if(ret != c_ret_ok)
                {
                    tcp_reconn_begin(p_task_arg, "tcp send err");
                    tcp_lcd_show(p_task_arg, 1);
                    sys.curr_task->step = c_step_reconn;
                    return;
                }
                task_tcp_set.send_req_seq_bk = task_tcp_set.send_req_seq;
                dbgtx("tcp send ascii=ALIENTEK DATA\r\n");
                sys.curr_task->tmr = tick_get();
                s_send_tmr = tick_get();
            }

            tcp_keep_upload:
            /* ---- 0. WAIT_UPOK: 对端回声对齐后才推进CACHE/FIFO ---- */
            if(s_ack_kind != c_ack_none)
            {
                if(tcp_rx(p_task_arg) != c_ret_ok)
                {
                    tcp_reconn_begin(p_task_arg, "tcp disconnect");
                    tcp_lcd_show(p_task_arg, 1);
                    sys.curr_task->step = c_step_reconn;
                    return;
                }
                if(tcp_ack_done() == c_ret_ok)
                {
                    if(s_ack_kind == c_ack_cache)
                    {
                        msg_send("task_sd_msg", "del");
                        s_cache_busy = 0;
                        s_cache_len = 0;
                        s_await_del = 1;
                        s_hs_tmr = tick_get();
                        dbgtx("tcp upload cache ok\r\n");
                        ESP_LOGI(TAG, "upload cache ok");
                    }
                    else if(s_ack_kind == c_ack_fifo)
                    {
                        dbgtx("tcp up fifo[%u] %s", s_fifo_head, s_tcp_fifo[s_fifo_head]);
                        s_fifo_head = (s_fifo_head + 1) % c_tcp_fifo_num;
                        s_fifo_cnt--;
                        s_fifo_off = 0;
                    }
                    tcp_ack_clear();
                    sys.curr_task->tmr = tick_get();
                    s_send_tmr = tick_get();
                    return;
                }
                if(tick_cmp(s_hs_tmr, c_tcp_hs_tmo_ms) == c_ret_ok)
                {
                    tcp_reconn_begin(p_task_arg, "tcp ack timeout");
                    tcp_lcd_show(p_task_arg, 3);
                    sys.curr_task->step = c_step_reconn;
                }
                return;
            }

            /* ---- 1. 补发CACHE: 向SD要文件, 发完等回声再 del ---- */
            if(s_await_del)
            {
                if(tcp_rx(p_task_arg) != c_ret_ok)
                {
                    tcp_reconn_begin(p_task_arg, "tcp disconnect");
                    tcp_lcd_show(p_task_arg, 1);
                    sys.curr_task->step = c_step_reconn;
                    return;
                }
                if(tick_cmp(s_hs_tmr, c_tcp_hs_tmo_ms) == c_ret_ok)
                {
                    msg_send("task_sd_msg", "del");
                    s_hs_tmr = tick_get();
                    dbgtx("tcp del retry\r\n");
                }
                return;
            }
            if(s_cache_busy)
            {
                if(s_cache_off == 0)
                    s_send_tmr = tick_get();
                ret = tcp_send_all(s_cache_tx, s_cache_len, &s_cache_off);
                if(ret == c_ret_wt)
                {
                    if(tick_cmp(s_send_tmr, c_tcp_send_tmo_ms) == c_ret_ok)
                    {
                        tcp_reconn_begin(p_task_arg, "tcp send timeout");
                        tcp_lcd_show(p_task_arg, 1);
                        sys.curr_task->step = c_step_reconn;
                    }
                    return;
                }
                if(ret != c_ret_ok)
                {
                    tcp_reconn_begin(p_task_arg, "tcp cache send err");
                    tcp_lcd_show(p_task_arg, 1);
                    sys.curr_task->step = c_step_reconn;
                    return;
                }
                tcp_ack_begin(c_ack_cache, s_cache_tx, s_cache_len);
                s_send_tmr = tick_get();
                return;
            }
            if(s_cache_wait)
            {
                if(tcp_rx(p_task_arg) != c_ret_ok)
                {
                    tcp_reconn_begin(p_task_arg, "tcp disconnect");
                    tcp_lcd_show(p_task_arg, 1);
                    sys.curr_task->step = c_step_reconn;
                    return;
                }
                if(tick_cmp(s_hs_tmr, c_tcp_hs_tmo_ms) == c_ret_ok)
                {
                    msg_send("task_sd_msg", "get");
                    s_hs_tmr = tick_get();
                    dbgtx("tcp get retry\r\n");
                }
                return;
            }
            if(s_sd_pending)
            {
                msg_send("task_sd_msg", "get");
                s_cache_wait = 1;
                s_hs_tmr = tick_get();
                return;
            }

            /* ---- 2. 发送FIFO实时数据, 回声确认后再出队 ---- */
            if(s_fifo_cnt > 0)
            {
                if(s_fifo_off == 0)
                    s_send_tmr = tick_get();
                ret = tcp_send_all(s_tcp_fifo[s_fifo_head],
                                   (unsigned int)strlen(s_tcp_fifo[s_fifo_head]),
                                   &s_fifo_off);
                if(ret == c_ret_wt)
                {
                    if(tick_cmp(s_send_tmr, c_tcp_send_tmo_ms) == c_ret_ok)
                    {
                        tcp_reconn_begin(p_task_arg, "tcp send timeout");
                        tcp_lcd_show(p_task_arg, 1);
                        sys.curr_task->step = c_step_reconn;
                    }
                    return;
                }
                if(ret != c_ret_ok)
                {
                    tcp_reconn_begin(p_task_arg, "tcp send err");
                    tcp_lcd_show(p_task_arg, 1);
                    sys.curr_task->step = c_step_reconn;
                    return;
                }
                tcp_ack_begin(c_ack_fifo, s_tcp_fifo[s_fifo_head],
                              (unsigned int)strlen(s_tcp_fifo[s_fifo_head]));
                s_send_tmr = tick_get();
                return;
            }

            /* ---- 3. 收包 ---- */
            if(tcp_rx(p_task_arg) != c_ret_ok)
            {
                tcp_reconn_begin(p_task_arg, "tcp disconnect");
                tcp_lcd_show(p_task_arg, 1);
                sys.curr_task->step = c_step_reconn;
                return;
            }

            /* ---- 4. 心跳保活 (对齐3x8c应用层心跳) ---- */
            if(tick_cmp(sys.curr_task->tmr, c_tcp_hb_ms) != c_ret_ok) break;
            ret = tcp_send();
            if(ret == c_ret_wt)
            {
                if(tick_cmp(s_send_tmr, c_tcp_send_tmo_ms) == c_ret_ok)
                {
                    tcp_reconn_begin(p_task_arg, "tcp send timeout");
                    tcp_lcd_show(p_task_arg, 1);
                    sys.curr_task->step = c_step_reconn;
                }
                return;
            }
            if(ret != c_ret_ok)
            {
                tcp_reconn_begin(p_task_arg, "tcp hb send err");
                tcp_lcd_show(p_task_arg, 1);
                sys.curr_task->step = c_step_reconn;
                return;
            }
            dbgtx("tcp hb dst=%s:%u\r\n", p_task_arg->run.ip, p_task_arg->run.port);
            sys.curr_task->tmr = tick_get();
            s_send_tmr = tick_get();
        break;

        case c_step_reconn:
            if(tick_cmp(sys.curr_task->tmr, p_task_arg->reconn_ms) != c_ret_ok) return;
            if(tcp_sta_alive() != c_ret_ok)
            {
                sys.curr_task->tmr = tick_get();    /* 等STA, 不叠加退避、不反复冲FIFO */
                return;
            }
            if(tcp_open(p_task_arg) != c_ret_ok)
            {
                if(p_task_arg->fail)
                {
                    sys.curr_task->step = c_step_show;
                    return;
                }
                tcp_reconn_begin(p_task_arg, "tcp open retry");
                return;
            }
            p_task_arg->busy = 1;
            tcp_lcd_show(p_task_arg, 0);
            sys.curr_task->tmr = tick_get();
            sys.curr_task->step = c_step_conn;
        break;

        default: sys.curr_task->step = c_step_idle; break;
    }
}

void task_tcp_init(void)
{
    snprintf(s_tcp_view.dst, sizeof(s_tcp_view.dst), "%s:%u", c_tcp_ip, c_tcp_port);
    msg_add("task_tcp_msg", task_tcp_msg_parse);
    dbgtx("tcp_init ok\r\n");
}
INIT_REG(task_tcp_init, 2);     /* 优先级2: 晚于wifi_init(1); 勿用tcp_init, 会链到lwIP */

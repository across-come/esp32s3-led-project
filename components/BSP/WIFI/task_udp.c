#include "user_ext.h"
#include "task_udp.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>

struct st_task_udp_set task_udp_set = {0};

static int s_udp_sock = -1;                      /* -1未打开; >=0 为lwIP socket fd */
static struct sockaddr_in s_udp_dest;            /* sendto目标: 云端IP/端口, 网络字节序 */
static const char c_udp_txt[] = "ALIENTEK DATA \r\n"; /* 网页ASCII, 对齐正点原子 */
static unsigned int s_udp_lcd_pend = 0;          /* 1=有新包待刷LCD, 等task_lcd空闲 */

static void udp_lcd_show(struct st_task_udp_run *p_task_arg, unsigned int flag);
static void udp_lcd_show_rx(struct st_task_udp_run *p_task_arg);

/* 可打印ASCII拷入dst; \r\n和逗号改成空格, 避免拆LCD的str:指令 */
static void udp_ascii_put(char *dst, unsigned int dst_sz, const unsigned char *src, int len)
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
 * UDP任务 串口指令解析
 * ======================================================================
 * exe=dbg_msg_send(msg:task_udp_msg,dat:help)
 * exe=dbg_msg_send(msg:task_udp_msg,dat:setup)
 * exe=dbg_msg_send(msg:task_udp_msg,dat:stop)
 * exe=dbg_msg_send(msg:task_udp_msg,dat:udp?)
 * exe=dbg_msg_send(msg:task_udp_msg,dat:udp:ip=120.76.240.167,port=53254)
 */
unsigned int task_udp_msg_parse(char *buf)
{
    char ip[c_wifi_ip_max + 1];
    unsigned int port;

    if(buf == NULL) return c_ret_nk;

    if(strcmp(buf, "help") == 0)
    {
        dbgtx("setup  (keep+hb until stop)\r\n");
        dbgtx("stop\r\n");
        dbgtx("udp?\r\n");
        dbgtx("udp:ip=ddd.ddd.ddd.ddd,port=ddddd\r\n");
        return c_ret_ok;
    }
    else if(strcmp(buf, "udp?") == 0)
    {
        memset(task_udp_set.set.ip, 0, sizeof(task_udp_set.set.ip));
        strncpy(task_udp_set.set.ip, c_udp_ip, c_wifi_ip_max);
        task_udp_set.set.port = c_udp_port;
        dbgtx("udp ip=%s port=%u\r\n", task_udp_set.set.ip, task_udp_set.set.port);
        return c_ret_ok;
    }
    else if(sscanf(buf, "udp:ip=%15[^,],port=%u", ip, &port) == 2) /* %15[^,]: 最多15字符直到逗号 */
    {
        memset(task_udp_set.set.ip, 0, sizeof(task_udp_set.set.ip));
        strncpy(task_udp_set.set.ip, ip, c_wifi_ip_max);
        task_udp_set.set.port = port;
        dbgtx("udp ip=%s port=%u\r\n", task_udp_set.set.ip, task_udp_set.set.port);
        return c_ret_ok;
    }
    else if(strcmp(buf, "setup") == 0)
    {
        if(task_get("task_udp") != NULL)
        {
            dbgtx("task_udp is running, cmd NK\r\n");
            return c_ret_ok;
        }
        if(task_udp_set.set.ip[0] == 0)
        {
            strncpy(task_udp_set.set.ip, c_udp_ip, c_wifi_ip_max);
            task_udp_set.set.port = c_udp_port;
        }
        /* seq++ 通知proc消费一次setup; 真正socket在idle里open, 这里不阻塞 */
        task_udp_set.setup_req_seq++;
        /* c_auto_quit: done置位后调度器删任务; dat为本次st_task_udp_run */
        task_add("task_udp", task_udp_proc, c_auto_quit, sizeof(struct st_task_udp_run));
        return c_ret_ok;
    }
    else if(strcmp(buf, "stop") == 0)
    {
        if(task_get("task_udp") == NULL) return c_ret_nk;
        task_udp_set.stop_req_seq++;    /* 只举手, close在proc开头udp_abort */
        return c_ret_ok;
    }
    return c_ret_nk;
}

static void udp_abort(void)
{
    if(s_udp_sock >= 0)
    {
        close(s_udp_sock);              /* 关fd, 内核释放本地端口 */
        s_udp_sock = -1;
    }
    s_udp_lcd_pend = 0;
}

static unsigned int udp_backoff_ms(unsigned int n)
{
    unsigned int ms = c_udp_reconn_min_ms;
    unsigned int i;

    for(i = 1; i < n; i++)
    {
        if(ms >= (c_udp_reconn_max_ms / 2))
            return c_udp_reconn_max_ms;
        ms *= 2;
    }
    return ms;
}

/* 关socket, 算退避; step由proc切到c_step_reconn */
static void udp_reconn_begin(struct st_task_udp_run *p_task_arg, const char *msg)
{
    if(p_task_arg == NULL) return;
    udp_abort();
    p_task_arg->ok = 0;
    p_task_arg->fail = 0;
    p_task_arg->busy = 0;
    if(p_task_arg->reconn_n < 16)
        p_task_arg->reconn_n++;
    p_task_arg->reconn_ms = udp_backoff_ms(p_task_arg->reconn_n);
    sys.curr_task->tmr = tick_get();
    if(msg != NULL)
        dbgtx("%s, wait=%ums n=%u\r\n", msg, p_task_arg->reconn_ms, p_task_arg->reconn_n);
    else
        dbgtx("udp reconn wait=%ums n=%u\r\n", p_task_arg->reconn_ms, p_task_arg->reconn_n);
}

static void udp_fail(struct st_task_udp_run *p_task_arg, const char *msg)
{
    if(p_task_arg == NULL) return;
    p_task_arg->busy = 0;
    p_task_arg->fail = 1;
    udp_abort();
    if(msg != NULL) dbgtx("%s\r\n", msg);
}

/* 非阻塞socket/bind, 失败置fail */
static unsigned int udp_open(struct st_task_udp_run *p_task_arg)
{
    struct sockaddr_in local_addr;
    socklen_t slen;
    int flags;

    if(p_task_arg == NULL) return c_ret_nk;

    p_task_arg->ok = 0;
    p_task_arg->fail = 0;
    p_task_arg->busy = 0;
    p_task_arg->local_port = 0;
    p_task_arg->local_ip[0] = 0;
    p_task_arg->rx_len = 0;
    p_task_arg->rx_hex[0] = 0;
    udp_abort();

    /* 必须先有STA地址, 否则bind/sendto没有出网口; 不置fail, 由proc进reconn */
    if(wifi_sta_ip_get(p_task_arg->local_ip, sizeof(p_task_arg->local_ip)) != c_ret_ok)
    {
        dbgtx("udp no sta ip, connect wifi first\r\n");
        udp_abort();
        return c_ret_nk;
    }
    if((p_task_arg->run.ip[0] == 0) || (p_task_arg->run.port == 0))
    {
        udp_fail(p_task_arg, "udp ip/port empty");
        return c_ret_nk;
    }

    //存储一个 IPv4 的通信端点
    memset(&s_udp_dest, 0, sizeof(s_udp_dest));
    s_udp_dest.sin_family = AF_INET;
    s_udp_dest.sin_port = htons((uint16_t)p_task_arg->run.port); /* 主机序→网络序(大端) */
    s_udp_dest.sin_addr.s_addr = inet_addr(p_task_arg->run.ip);  /* "a.b.c.d"→32bit网络序 */
    if(s_udp_dest.sin_addr.s_addr == INADDR_NONE)                /* 非法IP字符串 */
    {
        dbgtx("udp inet_addr err ip=%s\r\n", p_task_arg->run.ip);
        udp_fail(p_task_arg, NULL);
        return c_ret_nk;
    }

    /* AF_INET=IPv4, SOCK_DGRAM=UDP数据报, IPPROTO_IP=由类型推断; 失败返回-1 */
    s_udp_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if(s_udp_sock < 0)
    {
        dbgtx("udp socket err\r\n");
        udp_abort();
        return c_ret_nk;
    }
    /* F_GETFL: 读出当前文件状态标志, 失败返回-1. 再 F_SETFL 在原标志上或上O_NONBLOCK
     * 非阻塞后 sendto/recvfrom 没缓冲/没数据立刻返回, 不卡住协作式调度 */
    flags = fcntl(s_udp_sock, F_GETFL, 0); //设置非阻塞
    if(flags < 0) flags = 0;
    fcntl(s_udp_sock, F_SETFL, flags | O_NONBLOCK);

    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_port = htons(0);                 /* 端口0=让内核分配临时源端口 */
    local_addr.sin_addr.s_addr = htonl(INADDR_ANY);  /* 0.0.0.0: 所有网卡, 才能收到回包 */
    /* 把socket绑到本地地址; UDP必须bind后recvfrom才知道从哪收. 成功返回0, 失败非0 */
    if(bind(s_udp_sock, (struct sockaddr *)&local_addr, sizeof(local_addr)) != 0)
    {
        dbgtx("udp bind err\r\n");
        udp_abort();
        return c_ret_nk;
    }
    slen = sizeof(local_addr);                      /* 传入缓冲区大小, 内核会改成实际长度 */
    /* bind(port=0)后真正源端口才确定; getsockname把已绑定地址写回local_addr, 成功返回0
     * ntohs: 网络序→主机序, 给LCD/日志显示本机端口 */
    if(getsockname(s_udp_sock, (struct sockaddr *)&local_addr, &slen) == 0)
        p_task_arg->local_port = ntohs(local_addr.sin_port);

    dbgtx("udp connecting %s:%u local=%s:%u\r\n",
          p_task_arg->run.ip, p_task_arg->run.port,
          p_task_arg->local_ip, p_task_arg->local_port);
    return c_ret_ok;
}

/* ok已发出 wt重试 nk失败 */
static unsigned int udp_send(void)
{
    int ret;
    int err;

    if(s_udp_sock < 0) return c_ret_nk;
    /* 向s_udp_dest发ASCII探测包; 非连接UDP必须带目的地址 */
    ret = sendto(s_udp_sock, c_udp_txt, sizeof(c_udp_txt) - 1, 0,
                 (struct sockaddr *)&s_udp_dest, sizeof(s_udp_dest));
    if(ret >= 0) return c_ret_ok;
    err = errno;
    /* EAGAIN/EWOULDBLOCK: 发送缓冲暂满; EINPROGRESS: 还在处理; ENOMEM: 暂时没内存
     * 这些都可以下tick再试, 不要当致命错误关socket */
    if((err == EAGAIN) || (err == EWOULDBLOCK) || (err == EINPROGRESS) || (err == ENOMEM))
        return c_ret_wt;
    dbgtx("udp send err errno=%d\r\n", err);
    return c_ret_nk;
}

static void udp_lcd_show(struct st_task_udp_run *p_task_arg, unsigned int flag)
{
    char msg[96];
    char show[22];

    if(p_task_arg == NULL) return;

    /* 全屏重画: fill清屏, 多条str入队, 最后setup_done才真正启动LCD任务 */
    msg_send("task_lcd_msg", "fill:x=0,y=0,w=320,h=240,color=0x0000");
    msg_send("task_lcd_msg", "str:x=10,y=5,size=16,color=0xffff,bk=0x0000,txt=UDP:");

    if(flag == 0)           /* 连接中(本文件未走这条, 预留对齐wifi屏显) */
    {
        msg_send("task_lcd_msg", "str:x=10,y=40,size=16,color=0x07ff,bk=0x0000,txt=udp connecting......");
    }
    else if(flag == 1)
    {
        msg_send("task_lcd_msg", "str:x=10,y=40,size=16,color=0xf800,bk=0x0000,txt=udp connect fail");
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
    msg_send("task_lcd_msg", "setup_done");          /* 启动LCD任务把上面队列画完 */
}

static void udp_lcd_show_rx(struct st_task_udp_run *p_task_arg)
{
    char msg[96];
    char txt[32];

    if((p_task_arg == NULL) || (p_task_arg->rx_hex[0] == 0)) return;
    if(task_get("task_lcd") != NULL) return;         /* LCD忙则本tick不刷, 留给pend */

    msg_send("task_lcd_msg", "fill:x=0,y=120,w=320,h=40,color=0x0000");
    memset(txt, 0, sizeof(txt));
    snprintf(txt, sizeof(txt), "rx:%s", p_task_arg->rx_hex);
    sprintf(msg, "str:x=10,y=120,size=16,color=0x07e0,bk=0x0000,txt=%s", txt);
    msg_send("task_lcd_msg", msg);
    sprintf(msg, "str:x=10,y=140,size=16,color=0x07e0,bk=0x0000,txt=len:%u", p_task_arg->rx_len);
    msg_send("task_lcd_msg", msg);
    msg_send("task_lcd_msg", "setup_done");
}

/* UDP接收: 对齐 task_rx, 非阻塞把积压包读完 */
static void udp_rx(struct st_task_udp_run *p_task_arg)
{
    struct sockaddr_in from;
    socklen_t slen;
    unsigned char rxbuf[c_udp_rx_max];
    char ascii[33];
    esp_ip4_addr_t src;
    int ret;

    if((p_task_arg == NULL) || (s_udp_sock < 0)) return;

    /* 对齐task_rx: 非阻塞把内核里积压的UDP一次抽干, 没数据立刻退 */
    while(1)
    {
        slen = sizeof(from);            /* 每次recvfrom都要重填, 内核会改slen */
        memset(&from, 0, sizeof(from));
        /* 收一包并带回源地址from; 非阻塞无数据返回-1且errno=EAGAIN, 有数据返回长度 */
        ret = recvfrom(s_udp_sock, rxbuf, sizeof(rxbuf), 0, (struct sockaddr *)&from, &slen);
        if(ret <= 0) break;

        udp_ascii_put(ascii, sizeof(ascii), rxbuf, ret);
        src.addr = from.sin_addr.s_addr;
        dbgtx("udp rx " IPSTR ":%u len=%d ascii=%s\r\n",
              IP2STR(&src), ntohs(from.sin_port), ret, ascii);

        udp_ascii_put(p_task_arg->rx_hex, sizeof(p_task_arg->rx_hex), rxbuf, ret);
        p_task_arg->rx_len = (unsigned int)ret;
        s_udp_lcd_pend = 1;
    }

    if(s_udp_lcd_pend && (task_get("task_lcd") == NULL))
    {
        udp_lcd_show_rx(p_task_arg);
        s_udp_lcd_pend = 0;
    }
}

/* ======================================================================
 * UDP任务: idle建连 -> send -> show -> keep(收包+心跳)
 * keep里STA没了/发送失败 -> reconn退避后重开, 直到stop, 不置done
 * ====================================================================== */
void task_udp_proc(void)
{
    #define c_step_idle    0
    #define c_step_send    1
    #define c_step_show    2
    #define c_step_keep    3
    #define c_step_reconn  4
    struct st_task_udp_run *p_task_arg;
    unsigned int ret;
    char ip[16];

    p_task_arg = (struct st_task_udp_run *)sys.curr_task->dat;

    /* 任意step都先看stop: seq与bk不一致说明串口要退出 */
    if(task_udp_set.stop_req_seq_bk != task_udp_set.stop_req_seq)
    {
        task_udp_set.stop_req_seq_bk = task_udp_set.stop_req_seq;
        udp_abort();
        sys.curr_task->step = c_step_idle;
        sys.curr_task->done = c_done_nk;            /* auto_quit拆掉task_udp */
        return;
    }
    switch(sys.curr_task->step)
    {
        case c_step_idle:
            if(task_udp_set.setup_req_seq_bk == task_udp_set.setup_req_seq) return;
            task_udp_set.setup_req_seq_bk = task_udp_set.setup_req_seq; /* 消费一次setup */
            p_task_arg->run = task_udp_set.set;     /* 拷本次目标, 之后改set不影响在跑的 */
            p_task_arg->reconn_n = 0;
            if((p_task_arg->run.ip[0] == 0) || (p_task_arg->run.port == 0))
            {
                udp_fail(p_task_arg, "udp ip/port empty");
                sys.curr_task->step = c_step_show;
                return;
            }
            if(udp_open(p_task_arg) != c_ret_ok)
            {
                if(p_task_arg->fail)
                {
                    sys.curr_task->step = c_step_show;
                    return;
                }
                udp_reconn_begin(p_task_arg, "udp open fail");
                udp_lcd_show(p_task_arg, 0);
                sys.curr_task->step = c_step_reconn;
                return;
            }
            p_task_arg->busy = 1;
            sys.curr_task->tmr = tick_get();        /* send超时起点 */
            sys.curr_task->step = c_step_send;
        break;

        case c_step_send:
            ret = udp_send();                       /* ASCII: ALIENTEK DATA */
            if(ret == c_ret_wt)
            {
                if(tick_cmp(sys.curr_task->tmr, c_udp_tmo_ms) == c_ret_ok)
                {
                    udp_reconn_begin(p_task_arg, "udp send timeout");
                    udp_lcd_show(p_task_arg, 0);
                    sys.curr_task->step = c_step_reconn;
                }
                return;                             /* 未超时: 下tick再sendto */
            }
            if(ret != c_ret_ok)
            {
                udp_reconn_begin(p_task_arg, "udp send err");
                udp_lcd_show(p_task_arg, 0);
                sys.curr_task->step = c_step_reconn;
                return;
            }
            p_task_arg->busy = 0;
            p_task_arg->ok = 1;
            p_task_arg->reconn_n = 0;
            dbgtx("udp ok dst=%s:%u local=%s:%u ascii=ALIENTEK DATA\r\n",
                  p_task_arg->run.ip, p_task_arg->run.port,
                  p_task_arg->local_ip, p_task_arg->local_port);
            udp_send();                             /* 再发一包, 方便云端对上源端口 */
            dbgtx("udp kick rx, enable echo/cyclic on server\r\n");
            sys.curr_task->step = c_step_show;
        break;

        case c_step_show:
            if(p_task_arg->ok)
            {
                udp_lcd_show(p_task_arg, 2);
                dbgtx("udp display done, keep hb\r\n");
                sys.curr_task->tmr = tick_get();    /* 心跳计时, 已在send里发过 */
                sys.curr_task->step = c_step_keep;  /* 不置done, 常驻收包+心跳 */
                return;
            }
            udp_lcd_show(p_task_arg, 1);
            dbgtx("udp display done\r\n");
            sys.curr_task->step = c_step_idle;
            sys.curr_task->done = c_done_ok;        /* 仅配置错误才退出 */
        break;

        case c_step_keep:
            if(wifi_sta_ip_get(ip, sizeof(ip)) != c_ret_ok)
            {
                udp_reconn_begin(p_task_arg, "udp sta lost");
                udp_lcd_show(p_task_arg, 0);
                sys.curr_task->step = c_step_reconn;
                return;
            }
            udp_rx(p_task_arg);
            if(tick_cmp(sys.curr_task->tmr, c_udp_hb_ms) != c_ret_ok) break;
            ret = udp_send();
            if(ret == c_ret_wt) return;             /* 缓冲暂满, 下tick再发, tmr保持到期 */
            if(ret != c_ret_ok)
            {
                udp_reconn_begin(p_task_arg, "udp hb send err");
                udp_lcd_show(p_task_arg, 0);
                sys.curr_task->step = c_step_reconn;
                return;
            }
            dbgtx("udp hb dst=%s:%u\r\n", p_task_arg->run.ip, p_task_arg->run.port);
            sys.curr_task->tmr = tick_get();
        break;

        case c_step_reconn:
            if(tick_cmp(sys.curr_task->tmr, p_task_arg->reconn_ms) != c_ret_ok) return;
            if(wifi_sta_ip_get(ip, sizeof(ip)) != c_ret_ok)
            {
                udp_reconn_begin(p_task_arg, "udp wait sta");
                return;
            }
            if(udp_open(p_task_arg) != c_ret_ok)
            {
                udp_reconn_begin(p_task_arg, "udp open retry");
                return;
            }
            p_task_arg->busy = 1;
            sys.curr_task->tmr = tick_get();
            sys.curr_task->step = c_step_send;
        break;

        default: sys.curr_task->step = c_step_idle; break;
    }
}

void task_udp_init(void)
{
    msg_add("task_udp_msg", task_udp_msg_parse);    
    dbgtx("udp_init ok\r\n");
}
INIT_REG(task_udp_init, 2);     /* 优先级2: 晚于wifi_init(1), UDP要用STA IP */

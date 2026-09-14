#include "user_ext.h"
#include "task_tcps.h"
#include "esp_netif.h"
#include "lwip/sockets.h"
#include "lwip/inet.h"
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>

struct st_task_tcps_set task_tcps_set = {0};

static int s_tcps_listen = -1;                   /* 监听fd, bind+listen后一直保持 */
static int s_tcps_conn = -1;                     /* accept得到的客户端fd, 无连接为-1 */
static const char c_tcps_txt[] = "ALIENTEK DATA \r\n";  /* 对齐正点原子发送内容 */
static unsigned int s_tcps_lcd_pend = 0;

static void tcps_lcd_show(struct st_task_tcps_run *p_task_arg, unsigned int flag);
static void tcps_lcd_show_rx(struct st_task_tcps_run *p_task_arg);

static void tcps_ascii_put(char *dst, unsigned int dst_sz, const unsigned char *src, int len)
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
 * TCP Server任务 串口指令解析
 * 对齐正点原子07_WiFi_TCPServer: 板子是TCPServer, 对端网助是TCPClient
 * send 对应例程 KEY0 (不另开FreeRTOS发送线程)
 * ======================================================================
 * exe=dbg_msg_send(msg:task_tcps_msg,dat:help)
 * exe=dbg_msg_send(msg:task_tcps_msg,dat:setup)
 * exe=dbg_msg_send(msg:task_tcps_msg,dat:stop)
 * exe=dbg_msg_send(msg:task_tcps_msg,dat:send)
 * exe=dbg_msg_send(msg:task_tcps_msg,dat:tcps?)
 * exe=dbg_msg_send(msg:task_tcps_msg,dat:tcps:port=8080)
 */
unsigned int task_tcps_msg_parse(char *buf)
{
    unsigned int port;

    if(buf == NULL) return c_ret_nk;

    if(strcmp(buf, "help") == 0)
    {
        dbgtx("setup\r\n");
        dbgtx("stop\r\n");
        dbgtx("send\r\n");
        dbgtx("tcps?\r\n");
        dbgtx("tcps:port=ddddd\r\n");
        return c_ret_ok;
    }
    else if(strcmp(buf, "tcps?") == 0)
    {
        task_tcps_set.set.port = c_tcps_port;
        dbgtx("tcps port=%u\r\n", task_tcps_set.set.port);
        return c_ret_ok;
    }
    else if(sscanf(buf, "tcps:port=%u", &port) == 1)
    {
        task_tcps_set.set.port = port;
        dbgtx("tcps port=%u\r\n", task_tcps_set.set.port);
        return c_ret_ok;
    }
    else if(strcmp(buf, "setup") == 0)
    {
        if(task_get("task_tcps") != NULL)
        {
            dbgtx("task_tcps is running, cmd NK\r\n");
            return c_ret_ok;
        }
        if(task_tcps_set.set.port == 0)
            task_tcps_set.set.port = c_tcps_port;
        task_tcps_set.setup_req_seq++;
        task_add("task_tcps", task_tcps_proc, c_auto_quit, sizeof(struct st_task_tcps_run));
        return c_ret_ok;
    }
    else if(strcmp(buf, "send") == 0)
    {
        if(task_get("task_tcps") == NULL) return c_ret_nk;
        task_tcps_set.send_req_seq++;
        return c_ret_ok;
    }
    else if(strcmp(buf, "stop") == 0)
    {
        if(task_get("task_tcps") == NULL) return c_ret_nk;
        task_tcps_set.stop_req_seq++;
        return c_ret_ok;
    }
    return c_ret_nk;
}

static void tcps_conn_close(void)
{
    if(s_tcps_conn >= 0)
    {
        close(s_tcps_conn);
        s_tcps_conn = -1;
    }
    s_tcps_lcd_pend = 0;
}

static void tcps_abort(void)
{
    tcps_conn_close();
    if(s_tcps_listen >= 0)
    {
        close(s_tcps_listen);
        s_tcps_listen = -1;
    }
}

static void tcps_fail(struct st_task_tcps_run *p_task_arg, const char *msg)
{
    if(p_task_arg == NULL) return;
    p_task_arg->busy = 0;
    p_task_arg->ok = 0;
    p_task_arg->fail = 1;
    tcps_abort();
    if(msg != NULL) dbgtx("%s\r\n", msg);
}

static void tcps_sock_nb(int fd)
{
    int flags;

    if(fd < 0) return;
    flags = fcntl(fd, F_GETFL, 0);
    if(flags < 0) flags = 0;
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

/* socket+SO_REUSEADDR+bind(INADDR_ANY)+listen, 失败置fail */
static unsigned int tcps_open(struct st_task_tcps_run *p_task_arg)
{
    struct sockaddr_in local_addr;
    int on;
    int ret;

    if(p_task_arg == NULL) return c_ret_nk;

    p_task_arg->ok = 0;
    p_task_arg->fail = 0;
    p_task_arg->busy = 0;
    p_task_arg->local_port = 0;
    p_task_arg->peer_port = 0;
    p_task_arg->local_ip[0] = 0;
    p_task_arg->peer_ip[0] = 0;
    p_task_arg->rx_len = 0;
    p_task_arg->rx_hex[0] = 0;
    tcps_abort();

    if(wifi_sta_ip_get(p_task_arg->local_ip, sizeof(p_task_arg->local_ip)) != c_ret_ok)
    {
        tcps_fail(p_task_arg, "tcps no sta ip, connect wifi first");
        return c_ret_nk;
    }
    if(p_task_arg->run.port == 0)
    {
        tcps_fail(p_task_arg, "tcps port empty");
        return c_ret_nk;
    }

    /* SOCK_STREAM+IPPROTO_TCP, 对齐例程; 板子当Server, 不connect远端 */
    s_tcps_listen = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if(s_tcps_listen < 0)
    {
        tcps_fail(p_task_arg, "tcps socket err");
        return c_ret_nk;
    }

    /* 重启后端口可能仍在TIME_WAIT, 允许立刻再bind同一端口 */
    on = 1;
    setsockopt(s_tcps_listen, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
    tcps_sock_nb(s_tcps_listen);

    memset(&local_addr, 0, sizeof(local_addr));
    local_addr.sin_family = AF_INET;
    local_addr.sin_port = htons((uint16_t)p_task_arg->run.port);
    local_addr.sin_addr.s_addr = htonl(INADDR_ANY); /* 0.0.0.0: 所有网卡都收连接 */
    ret = bind(s_tcps_listen, (struct sockaddr *)&local_addr, sizeof(local_addr));
    if(ret != 0)
    {
        dbgtx("tcps bind err errno=%d\r\n", errno);
        tcps_fail(p_task_arg, "tcps bind err");
        return c_ret_nk;
    }

    /* backlog=4 对齐例程: 未accept前最多排队4个SYN */
    ret = listen(s_tcps_listen, c_tcps_backlog);
    if(ret != 0)
    {
        dbgtx("tcps listen err errno=%d\r\n", errno);
        tcps_fail(p_task_arg, "tcps listen err");
        return c_ret_nk;
    }

    p_task_arg->local_port = p_task_arg->run.port;
    dbgtx("tcps listen %s:%u\r\n", p_task_arg->local_ip, p_task_arg->local_port);
    return c_ret_ok;
}

/* 非阻塞accept. ok已接上 wt继续等 nk监听失败 */
static unsigned int tcps_accept(struct st_task_tcps_run *p_task_arg)
{
    struct sockaddr_in conn_addr;
    socklen_t addr_len;
    esp_ip4_addr_t peer;
    int fd;
    int err;

    if((p_task_arg == NULL) || (s_tcps_listen < 0)) return c_ret_nk;

    addr_len = sizeof(conn_addr);
    memset(&conn_addr, 0, sizeof(conn_addr));
    fd = accept(s_tcps_listen, (struct sockaddr *)&conn_addr, &addr_len);
    if(fd < 0)
    {
        err = errno;
        if((err == EAGAIN) || (err == EWOULDBLOCK)) return c_ret_wt;
        dbgtx("tcps accept err errno=%d\r\n", err);
        return c_ret_nk;
    }

    tcps_conn_close();
    s_tcps_conn = fd;
    tcps_sock_nb(s_tcps_conn);

    peer.addr = conn_addr.sin_addr.s_addr;
    snprintf(p_task_arg->peer_ip, sizeof(p_task_arg->peer_ip), IPSTR, IP2STR(&peer));
    p_task_arg->peer_port = ntohs(conn_addr.sin_port);
    dbgtx("tcps accept peer=%s:%u\r\n", p_task_arg->peer_ip, p_task_arg->peer_port);
    return c_ret_ok;
}

static unsigned int tcps_send(void)
{
    int ret;
    int err;

    if(s_tcps_conn < 0) return c_ret_nk;
    ret = send(s_tcps_conn, c_tcps_txt, sizeof(c_tcps_txt) - 1, 0);
    if(ret >= 0) return c_ret_ok;
    err = errno;
    if((err == EAGAIN) || (err == EWOULDBLOCK) || (err == EINPROGRESS) || (err == ENOMEM))
        return c_ret_wt;
    dbgtx("tcps send err errno=%d\r\n", err);
    return c_ret_nk;
}

/* flag: 1失败 2监听等客户端 3已有连接 */
static void tcps_lcd_show(struct st_task_tcps_run *p_task_arg, unsigned int flag)
{
    char msg[96];

    if(p_task_arg == NULL) return;

    msg_send("task_lcd_msg", "fill:x=0,y=0,w=320,h=240,color=0x0000");
    msg_send("task_lcd_msg", "str:x=10,y=5,size=16,color=0xffff,bk=0x0000,txt=TCPS:");

    if(flag == 1)
    {
        msg_send("task_lcd_msg", "str:x=10,y=40,size=16,color=0xf800,bk=0x0000,txt=tcps listen fail");
    }
    else if(flag == 2)
    {
        sprintf(msg, "str:x=10,y=40,size=16,color=0x07e0,bk=0x0000,txt=ip:%s", p_task_arg->local_ip);
        msg_send("task_lcd_msg", msg);
        sprintf(msg, "str:x=10,y=60,size=16,color=0x001f,bk=0x0000,txt=port:%u", p_task_arg->local_port);
        msg_send("task_lcd_msg", msg);
        msg_send("task_lcd_msg", "str:x=10,y=80,size=16,color=0x07ff,bk=0x0000,txt=wait client...");
    }
    else
    {
        sprintf(msg, "str:x=10,y=40,size=16,color=0x07e0,bk=0x0000,txt=ip:%s", p_task_arg->local_ip);
        msg_send("task_lcd_msg", msg);
        sprintf(msg, "str:x=10,y=60,size=16,color=0x001f,bk=0x0000,txt=port:%u", p_task_arg->local_port);
        msg_send("task_lcd_msg", msg);
        sprintf(msg, "str:x=10,y=80,size=16,color=0x07e0,bk=0x0000,txt=peer:%s", p_task_arg->peer_ip);
        msg_send("task_lcd_msg", msg);
        sprintf(msg, "str:x=10,y=100,size=16,color=0x001f,bk=0x0000,txt=pport:%u", p_task_arg->peer_port);
        msg_send("task_lcd_msg", msg);
        msg_send("task_lcd_msg", "str:x=10,y=120,size=16,color=0x07ff,bk=0x0000,txt=wait rx...");
    }
    msg_send("task_lcd_msg", "setup_done");
}

static void tcps_lcd_show_rx(struct st_task_tcps_run *p_task_arg)
{
    char msg[96];
    char txt[32];

    if((p_task_arg == NULL) || (p_task_arg->rx_hex[0] == 0)) return;
    if(task_get("task_lcd") != NULL) return;

    msg_send("task_lcd_msg", "fill:x=0,y=120,w=320,h=80,color=0x0000");
    memset(txt, 0, sizeof(txt));
    snprintf(txt, sizeof(txt), "rx:%s", p_task_arg->rx_hex);
    sprintf(msg, "str:x=10,y=120,size=16,color=0x07e0,bk=0x0000,txt=%s", txt);
    msg_send("task_lcd_msg", msg);
    sprintf(msg, "str:x=10,y=140,size=16,color=0x07e0,bk=0x0000,txt=len:%u", p_task_arg->rx_len);
    msg_send("task_lcd_msg", msg);
    msg_send("task_lcd_msg", "setup_done");
}

/* 非阻塞抽干recv. ok继续, nk对端关闭或出错(监听仍在) */
static unsigned int tcps_rx(struct st_task_tcps_run *p_task_arg)
{
    unsigned char rxbuf[c_tcps_rx_max];
    char ascii[33];
    int ret;
    int err;

    if((p_task_arg == NULL) || (s_tcps_conn < 0)) return c_ret_nk;

    while(1)
    {
        ret = recv(s_tcps_conn, rxbuf, sizeof(rxbuf), 0);
        if(ret == 0)
        {
            dbgtx("tcps peer close\r\n");
            return c_ret_nk;
        }
        if(ret < 0)
        {
            err = errno;
            if((err == EAGAIN) || (err == EWOULDBLOCK)) break;
            dbgtx("tcps recv err errno=%d\r\n", err);
            return c_ret_nk;
        }

        tcps_ascii_put(ascii, sizeof(ascii), rxbuf, ret);
        dbgtx("tcps rx %s:%u len=%d ascii=%s\r\n",
              p_task_arg->peer_ip, p_task_arg->peer_port, ret, ascii);

        tcps_ascii_put(p_task_arg->rx_hex, sizeof(p_task_arg->rx_hex), rxbuf, ret);
        p_task_arg->rx_len = (unsigned int)ret;
        s_tcps_lcd_pend = 1;
    }

    if(s_tcps_lcd_pend && (task_get("task_lcd") == NULL))
    {
        tcps_lcd_show_rx(p_task_arg);
        s_tcps_lcd_pend = 0;
    }
    return c_ret_ok;
}

/* ======================================================================
 * TCP Server: idle绑定监听 -> show屏显 -> accept等客户端 -> recv
 * 客户端断开只关conn, 回到accept, 对齐例程外层while
 * ====================================================================== */
void task_tcps_proc(void)
{
    #define c_step_idle    0
    #define c_step_show    1
    #define c_step_accept  2
    #define c_step_recv    3
    struct st_task_tcps_run *p_task_arg;
    unsigned int ret;

    p_task_arg = (struct st_task_tcps_run *)sys.curr_task->dat;

    if(task_tcps_set.stop_req_seq_bk != task_tcps_set.stop_req_seq)
    {
        task_tcps_set.stop_req_seq_bk = task_tcps_set.stop_req_seq;
        tcps_abort();
        sys.curr_task->step = c_step_idle;
        sys.curr_task->done = c_done_nk;
        return;
    }
    switch(sys.curr_task->step)
    {
        case c_step_idle:
            if(task_tcps_set.setup_req_seq_bk == task_tcps_set.setup_req_seq) return;
            task_tcps_set.setup_req_seq_bk = task_tcps_set.setup_req_seq;
            p_task_arg->run = task_tcps_set.set;
            if(tcps_open(p_task_arg) != c_ret_ok)
            {
                sys.curr_task->step = c_step_show;
                return;
            }
            p_task_arg->ok = 1;
            p_task_arg->busy = 1;
            sys.curr_task->step = c_step_show;
        break;

        case c_step_show:
            if(p_task_arg->ok)
            {
                tcps_lcd_show(p_task_arg, 2);
                dbgtx("tcps display done, wait client\r\n");
                sys.curr_task->step = c_step_accept;
                return;
            }
            tcps_lcd_show(p_task_arg, 1);
            dbgtx("tcps display done\r\n");
            sys.curr_task->step = c_step_idle;
            sys.curr_task->done = c_done_ok;
        break;

        case c_step_accept:
            ret = tcps_accept(p_task_arg);
            if(ret == c_ret_wt) return;             /* 一直等客户端, 不超时 */
            if(ret != c_ret_ok)
            {
                tcps_fail(p_task_arg, "tcps accept fail");
                sys.curr_task->step = c_step_show;
                return;
            }
            p_task_arg->busy = 0;
            tcps_lcd_show(p_task_arg, 3);
            dbgtx("tcps connected, wait rx\r\n");
            sys.curr_task->step = c_step_recv;
        break;

        case c_step_recv:
            if(task_tcps_set.send_req_seq_bk != task_tcps_set.send_req_seq)
            {
                task_tcps_set.send_req_seq_bk = task_tcps_set.send_req_seq;
                if(tcps_send() == c_ret_ok)
                    dbgtx("tcps send ALIENTEK DATA\r\n");
            }
            if(tcps_rx(p_task_arg) != c_ret_ok)
            {
                tcps_conn_close();                  /* 只关客户端, 监听留下 */
                p_task_arg->peer_ip[0] = 0;
                p_task_arg->peer_port = 0;
                p_task_arg->busy = 1;
                tcps_lcd_show(p_task_arg, 2);
                dbgtx("tcps disconnect, wait client\r\n");
                sys.curr_task->step = c_step_accept;
            }
        break;

        default: sys.curr_task->step = c_step_idle; break;
    }
}

void task_tcps_init(void)
{
    msg_add("task_tcps_msg", task_tcps_msg_parse);
    dbgtx("tcps_init ok\r\n");
}
INIT_REG(task_tcps_init, 2);    /* 优先级2: 晚于wifi_init(1) */

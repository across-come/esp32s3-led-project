#include "user_ext.h"
#include "task_data.h"
#include "esp_random.h"

/* 只通过消息交给 TCP: 在线入FIFO, 离线或FIFO满由 TCP 转 append 给 SD
 * 本模块不 include task_tcp.h / task_sd.h
 * 开机不自动跑, 等 exe setup
 */

#define c_dg_line_max       64
#define c_dg_msg_max        (c_dg_line_max + 8)

static unsigned int s_dg_seq = 0;
static unsigned int s_dg_tmr = 0;
static unsigned int s_stop_req = 0;
static unsigned int s_stop_bk = 0;
static char s_dg_last[c_dg_line_max] = {0};

unsigned int data_gen_seq_get(void)
{
    return s_dg_seq;
}

const char *data_gen_last(void)
{
    return s_dg_last;
}

/* exe=dbg_msg_send(msg:task_data_msg,dat:help)
 * exe=dbg_msg_send(msg:task_data_msg,dat:setup)
 * exe=dbg_msg_send(msg:task_data_msg,dat:stop)
 */
unsigned int task_data_msg_parse(char *buf)
{
    if(buf == NULL)
        return c_ret_nk;

    if(strcmp(buf, "help") == 0)
    {
        dbgtx("setup  (start 5s push)\r\n");
        dbgtx("stop\r\n");
        return c_ret_ok;
    }
    else if(strcmp(buf, "setup") == 0)
    {
        if(task_get("task_data") != NULL)
        {
            dbgtx("task_data is running, cmd NK\r\n");
            return c_ret_ok;
        }
        s_dg_tmr = 0;
        s_stop_req = s_stop_bk;
        task_add("task_data", data_gen_proc, c_auto_quit, 0);
        return c_ret_ok;
    }
    else if(strcmp(buf, "stop") == 0)
    {
        if(task_get("task_data") == NULL)
            return c_ret_nk;
        s_stop_req++;
        return c_ret_ok;
    }
    return c_ret_nk;
}

void data_gen_proc(void)
{
    char line[c_dg_line_max];
    char msg[c_dg_msg_max];
    int len;

    if(s_stop_bk != s_stop_req)
    {
        s_stop_bk = s_stop_req;
        sys.curr_task->done = c_done_nk;
        return;
    }

    if(s_dg_tmr == 0)
        s_dg_tmr = tick_get();
    if(tick_cmp(s_dg_tmr, c_dg_interval_ms) != c_ret_ok)
        return;
    s_dg_tmr = tick_get();

    len = snprintf(line, sizeof(line), "[SEQ=%06u T=%u R=%u]\r\n",
                   s_dg_seq++, tick_get(), (unsigned int)esp_random());
    if(len <= 0)
        return;
    memset(s_dg_last, 0, sizeof(s_dg_last));
    strncpy(s_dg_last, line, sizeof(s_dg_last) - 1);

    snprintf(msg, sizeof(msg), "push:%s", line);
    msg_send("task_tcp_msg", msg);
}

void data_gen_init(void)
{
    msg_add("task_data_msg", task_data_msg_parse);
    dbgtx("data_gen_init ok\r\n");
}
INIT_REG(data_gen_init, 3);     /* 优先级3: 晚于wifi(1)/tcp(2), 只注册消息 */

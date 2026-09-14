#ifndef __user_main_h__
#define __user_main_h__

#include "user_ext.h"

struct st_uart_tx_arg
{
    uart_port_t         port;
    unsigned char       *buf;
    unsigned int        idx;
    unsigned int        len;
    unsigned int        tmr;
};

/* KV 无名队列: 把要打印的字符串先存起来, 由task_tx发送 */
struct st_kv
{
    void                *dat;
};

typedef void (*flag_func)(void *dat);
struct st_flag
{
    char                    *name;
    void                    *dat;
    flag_func               func;
    unsigned char           seq;
    unsigned char           seq_bk;
};

typedef void (*task_func)(void);
struct st_task
{
    char                    *name;
    void                    *dat;
    task_func               func;
    unsigned char           done;
    unsigned char           wait;
    unsigned char           quit;
    #define c_auto_quit     1
    #define c_auto_wait     0
    unsigned char           auto_quit;
    unsigned char           first_run;
    unsigned char           step;
    unsigned int            tmr;
};

typedef unsigned int (*exe_func)(char *buf);
struct st_exe
{
    char                    *name;
    char                    *desc;
    exe_func                func;
};

struct st_var
{
    char                    *name;
    unsigned int            addr;
    unsigned int            type;
};

typedef unsigned int (*msg_func)(char *buf);
struct st_msg
{
    char                    *name;
    msg_func                func;
};

struct st_sys
{
    unsigned int            tick;

    struct st_task          *task;
    unsigned int            task_cnt;
    struct st_task          *curr_task;

    struct st_flag          *flag;
    unsigned int            flag_cnt;
    unsigned int            flag_seq;
    unsigned int            flag_seq_bk;

    struct st_kv            *kv;
    unsigned int            kv_cnt;

    struct st_var           *var;
    unsigned int            var_cnt;

    struct st_exe           *exe;
    unsigned int            exe_cnt;
    struct st_msg           *msg;
    unsigned int            msg_cnt;
};
extern struct st_sys sys;

#endif
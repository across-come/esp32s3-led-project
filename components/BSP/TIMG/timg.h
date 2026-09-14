#ifndef __timg_h__
#define __timg_h__
#include "driver/gptimer.h"

/* 定时器配置结构体 */
typedef struct
{
    gptimer_clock_source_t  clk_src;            /* 时钟源选择（GPTIMER_CLK_SRC_XTAL\GPTIMER_CLK_SRC_DEFAULT(默认)） */
    uint64_t                timing_time;        /* 定时时间(us) */
    uint64_t                alarm_value;        /* 警报值 */
    uint64_t                timer_count_value;  /* ISR中记录的计数值 */
    volatile unsigned int   seq;                /* 中断事件计数(ISR中++) */
    unsigned int            seq_bk;             /* 任务已处理计数 */
} timg_config_t;

/* 函数声明 */
void timg_init(void);
void task_timg(void);

#endif
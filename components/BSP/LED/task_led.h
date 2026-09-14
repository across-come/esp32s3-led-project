#ifndef __task_led_h__
#define __task_led_h__

#include "user_ext.h"

#define LED_GPIO_PIN    GPIO_NUM_1

enum GPIO_OUTPUT_STATE
{
    PIN_RESET,
    PIN_SET
};

#define LED0(x) do{x ? gpio_set_level(LED_GPIO_PIN, PIN_SET) : gpio_set_level(LED_GPIO_PIN, PIN_RESET);}while(0)
#define LED0_TOGGLE() do{gpio_set_level(LED_GPIO_PIN, !gpio_get_level(LED_GPIO_PIN));}while(0)

/* LED任务参数 */
struct st_led_arg
{
    unsigned int dly;
    unsigned int on;
    unsigned int off;
    unsigned int cnt_inloop;
    unsigned int cnt_outloop;
    unsigned int intv;
};

struct st_task_led_run
{
    struct st_led_arg   run;
    unsigned int        cnt_inloop_bk;
};

struct st_task_led_set
{
    struct st_led_arg   set;
    unsigned int        setup_done_req_seq;
    unsigned int        setup_done_req_seq_bk;
    unsigned int        stop_req_seq;
    unsigned int        stop_req_seq_bk;
};

extern struct st_task_led_set task_led_set;

extern unsigned int task_led_msg_proc(char *buf);
extern void task_led_proc(void);
extern void led_init(void);

#endif
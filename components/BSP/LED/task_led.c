#include "user_ext.h"

struct st_task_led_set task_led_set = {0};

/* ============ LED任务 串口指令(直接发送以下 exe= 字符串) ============
 * exe=dbg_msg_send(msg:task_led_msg,dat:help)              //帮助
 * exe=dbg_msg_send(msg:task_led_msg,dat:dly:1000)          //启动前延时ms
 * exe=dbg_msg_send(msg:task_led_msg,dat:on:1000)           //亮的时间ms
 * exe=dbg_msg_send(msg:task_led_msg,dat:off:1000)          //灭的时间ms
 * exe=dbg_msg_send(msg:task_led_msg,dat:cnt_inloop:10)     //每轮内循环闪烁次数
 * exe=dbg_msg_send(msg:task_led_msg,dat:cnt_outloop:5)     //外循环轮数
 * exe=dbg_msg_send(msg:task_led_msg,dat:intv:2000)         //每轮间隔ms
 * exe=dbg_msg_send(msg:task_led_msg,dat:on)                //直接点亮
 * exe=dbg_msg_send(msg:task_led_msg,dat:off)               //直接熄灭
 * exe=dbg_msg_send(msg:task_led_msg,dat:setup_done)        //启动闪烁任务
 * exe=dbg_msg_send(msg:task_led_msg,dat:stop)              //停止闪烁任务
 */
unsigned int task_led_msg_proc(char *buf)
{
    unsigned int val;

    if(buf == NULL) return c_ret_nk;

    if(strcmp(buf,"help") == 0)
    {
        dbgtx("dly:ddd\r\n");
        dbgtx("on\r\n");
        dbgtx("off\r\n");
        dbgtx("on:ddd\r\n");
        dbgtx("off:ddd\r\n");
        dbgtx("intv:ddd\r\n");
        dbgtx("cnt_inloop:ddd\r\n");
        dbgtx("cnt_outloop:ddd\r\n");
        dbgtx("setup_done\r\n");
        dbgtx("stop\r\n");
        return c_ret_ok;
    }
    else if(sscanf(buf, "dly:%d", &val) == 1)
    {
        task_led_set.set.dly = val;
        return c_ret_ok;
    }
    else if(strcmp(buf, "on") == 0)
    {
        gpio_set_level(LED_GPIO_PIN, PIN_SET);
        return c_ret_ok;
    }
    else if(strcmp(buf, "off") == 0)
    {
        gpio_set_level(LED_GPIO_PIN, PIN_RESET);
        return c_ret_ok;
    }
    else if(sscanf(buf, "on:%d", &val) == 1)
    {
        task_led_set.set.on = val;
        return c_ret_ok;
    }
    else if(sscanf(buf, "off:%d", &val) == 1)
    {
        task_led_set.set.off = val;
        return c_ret_ok;
    }
    else if(sscanf(buf, "intv:%d", &val) == 1)
    {
        task_led_set.set.intv = val;
        return c_ret_ok;
    }
    else if(sscanf(buf, "cnt_inloop:%d", &val) == 1)
    {
        task_led_set.set.cnt_inloop = val;
        return c_ret_ok;
    }
    else if(sscanf(buf, "cnt_outloop:%d", &val) == 1)
    {
        task_led_set.set.cnt_outloop = val;
        return c_ret_ok;
    }
    else if(strcmp(buf, "setup_done") == 0)
    {
        if(task_get("task_led") != NULL)
        {
            dbgtx("task_led is running, cmd NK\r\n");
            return c_ret_ok;
        }
        task_led_set.setup_done_req_seq++;
        task_add("task_led", task_led_proc, c_auto_quit, sizeof(struct st_task_led_run));
        return c_ret_ok;
    }
    else if(strcmp(buf, "stop") == 0)
    {
        if(task_get("task_led") == NULL) return c_ret_nk;
        task_led_set.stop_req_seq++;
        return c_ret_ok;
    }
    return c_ret_nk;
}

void task_led_proc(void)
{
    #define c_step_idle             0
    #define c_step_wait_dly         1
    #define c_step_wait_on          2
    #define c_step_wait_off         3
    #define c_step_wait_intv        4
    struct st_task_led_run  *p_task_arg;

    if(task_led_set.stop_req_seq_bk != task_led_set.stop_req_seq)
    {
        task_led_set.stop_req_seq_bk = task_led_set.stop_req_seq;
        gpio_set_level(LED_GPIO_PIN, PIN_SET);
        sys.curr_task->step = c_step_idle;
        sys.curr_task->done = c_done_nk;
        return;
    }

    p_task_arg = (struct st_task_led_run *)sys.curr_task->dat;
    switch(sys.curr_task->step)
    {
        case c_step_idle:
            if(task_led_set.setup_done_req_seq_bk == task_led_set.setup_done_req_seq) return;
            task_led_set.setup_done_req_seq_bk = task_led_set.setup_done_req_seq;

            p_task_arg->run = task_led_set.set;
            if((p_task_arg->run.on == 0)||(p_task_arg->run.off == 0)||(p_task_arg->run.cnt_inloop == 0)||(p_task_arg->run.cnt_outloop == 0))
                return;
            p_task_arg->cnt_inloop_bk = p_task_arg->run.cnt_inloop;
            gpio_set_level(LED_GPIO_PIN, PIN_SET);
            sys.curr_task->tmr  = tick_get();
            sys.curr_task->step = c_step_wait_dly;
        break;

        case c_step_wait_dly:
            if(tick_cmp(sys.curr_task->tmr, p_task_arg->run.dly) != c_ret_ok) return;
            gpio_set_level(LED_GPIO_PIN, PIN_RESET);
            sys.curr_task->tmr  = tick_get();
            sys.curr_task->step = c_step_wait_on;
        break;

        case c_step_wait_on:
            if(tick_cmp(sys.curr_task->tmr, p_task_arg->run.on) != c_ret_ok) return;
            gpio_set_level(LED_GPIO_PIN, PIN_SET);
            sys.curr_task->tmr  = tick_get();
            sys.curr_task->step = c_step_wait_off;
        break;

        case c_step_wait_off:
            if(tick_cmp(sys.curr_task->tmr, p_task_arg->run.off) != c_ret_ok) return;
            sys.curr_task->tmr  = tick_get();
            if(p_task_arg->run.cnt_inloop) p_task_arg->run.cnt_inloop--;
            if(p_task_arg->run.cnt_inloop == 0)
                sys.curr_task->step = c_step_wait_intv;
            else
            {
                gpio_set_level(LED_GPIO_PIN, PIN_RESET);
                sys.curr_task->step = c_step_wait_on;
            }
        break;

        case c_step_wait_intv:
            if(tick_cmp(sys.curr_task->tmr, p_task_arg->run.intv) != c_ret_ok) return;
            if(p_task_arg->run.cnt_outloop != 0xffffffff)
            {
                if(p_task_arg->run.cnt_outloop) p_task_arg->run.cnt_outloop--;
            }
            if(p_task_arg->run.cnt_outloop == 0)
            {
                sys.curr_task->step = c_step_idle;
                sys.curr_task->done = c_done_ok;
            }
            else
            {
                gpio_set_level(LED_GPIO_PIN, PIN_RESET);
                sys.curr_task->tmr  = tick_get();
                p_task_arg->run.cnt_inloop = p_task_arg->cnt_inloop_bk;
                sys.curr_task->step = c_step_wait_on;
            }
        break;

        default: sys.curr_task->step = c_step_idle; break;
    }
}

void led_init(void)
{
    gpio_config_t gpio_init_struct = {0};

    gpio_init_struct.intr_type = GPIO_INTR_DISABLE;
    gpio_init_struct.mode = GPIO_MODE_INPUT_OUTPUT;
    gpio_init_struct.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_init_struct.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_init_struct.pin_bit_mask = 1ull << LED_GPIO_PIN;

    ESP_ERROR_CHECK(gpio_config(&gpio_init_struct));
    gpio_set_level(LED_GPIO_PIN, PIN_SET);

    msg_add("task_led_msg", task_led_msg_proc);
}
INIT_REG(led_init, 3);
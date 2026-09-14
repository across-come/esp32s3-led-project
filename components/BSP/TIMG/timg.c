#include "timg.h"
#include "user_ext.h"
#include "esp_log.h"

static gptimer_handle_t gptimer = NULL;
static timg_config_t *timgr_config = NULL;

//定时器中断回调(ISR环境, 只做最轻的事, 不能调用非IRAM函数)
static bool IRAM_ATTR timer_group_isr_callback(gptimer_handle_t timer, const gptimer_alarm_event_data_t *edata, void *user_ctx)
{
    timg_config_t *user_data = (timg_config_t *)user_ctx;

    /* 记录当前计数值 */
    user_data->timer_count_value = edata->count_value;
    user_data->seq++;                       /* 中断事件计数, 由task_timg消费 */
    return false;
}

/* 定时器任务: 由task_proc周期调用, 检测到新事件才处理(任务上下文, 可安全操作外设)
 * 注: 暂时注释, 由LEDC的task_ledc_pwm替代
void task_timg(void)
{
    if(timgr_config->seq_bk != timgr_config->seq)
    {
        timgr_config->seq_bk = timgr_config->seq;
        LED0_TOGGLE();                      // LED翻转放在任务里, 不在ISR
        ESP_LOGI("Timer:", "Timer auto reloaded, count value in ISR: %llu", timgr_config->timer_count_value);
    }
}
*/

//初始化通用定时器
void timg_init(void)
{
    gptimer_config_t timer_config = {0};
    gptimer_alarm_config_t alarm_config = {0};
    gptimer_event_callbacks_t cbs = {0};

    timgr_config = malloc(sizeof(timg_config_t));

    timgr_config->clk_src           = GPTIMER_CLK_SRC_DEFAULT;      /* 时钟源 */
    timgr_config->timing_time       = 1 * 1000000;                  /* 定时时间(us) */
    timgr_config->alarm_value       = timgr_config->timing_time;    /* 警报值 */
    timgr_config->timer_count_value = 0;
    timgr_config->seq               = 0;
    timgr_config->seq_bk            = 0;

    /* 创建定时器: 1us一个tick */
    timer_config.clk_src        = timgr_config->clk_src;
    timer_config.direction      = GPTIMER_COUNT_UP;               /* 向上计数 */
    timer_config.resolution_hz  = 1000000;                        /* 分辨率1MHz */
    ESP_ERROR_CHECK(gptimer_new_timer(&timer_config, &gptimer));

    /* 设置警报: 计数到alarm_value触发, 自动重装载回reload_count */
    alarm_config.alarm_count          = timgr_config->alarm_value;
    alarm_config.reload_count         = 0;
    alarm_config.flags.auto_reload_on_alarm = 1;                  /* 自动重装载 */
    ESP_ERROR_CHECK(gptimer_set_alarm_action(gptimer, &alarm_config));

    /* 注册ISR回调 */
    cbs.on_alarm = timer_group_isr_callback;
    ESP_ERROR_CHECK(gptimer_register_event_callbacks(gptimer, &cbs, timgr_config));

    /* 使能定时器(新API必须enable后才能start) */
    ESP_ERROR_CHECK(gptimer_enable(gptimer));

    /* 当前计数值清零 */
    ESP_ERROR_CHECK(gptimer_set_raw_count(gptimer, 0));

    /* 注册到框架任务, 由task_proc周期调用 */
    //task_add("task_timg", task_timg, c_auto_quit, 0);   /* 暂时注释, 由task_ledc_pwm替代 */

    /* 开启定时器 */
    ESP_ERROR_CHECK(gptimer_start(gptimer));
}
INIT_REG(timg_init, 4);
// #include "ledc.h"
// #include "user_ext.h"

// uint32_t ledc_duty_pow(uint32_t duty, uint8_t m, uint8_t n)
// {
//     uint32_t result = 1;

//     while (n--)
//     {
//         result *= m; //m固定为2，2的n次幂
//     }

//     return (result * duty) / 100;
// }

// /* PWM呼吸任务: 由task_proc周期调用, 状态机延时控制呼吸节奏 */
// void task_ledc_pwm(void)
// {
//     #define c_ledc_pwm_step_ms  10    /* 每步占空比变化的间隔ms */

//     static unsigned char dir = 1;
//     static uint16_t ledpwmval = 0;
//     static unsigned int step = 0, tmr = 0;

//     switch(step)
//     {
//         case 0:
//             if (dir == 1)
//             {
//                 ledpwmval++;
//                 if (ledpwmval >= 99)
//                 {
//                     ledpwmval = 99; 
//                     dir = 0;
//                 }
//             }
//             else
//             {
//                 ledpwmval--; /* dir==0 ledpwmval递减 */
//                 if (ledpwmval <= 1)
//                 {
//                     ledpwmval = 1; 
//                     dir = 1;
//                 }
//             }
//             tmr = tick_get();

//             step = __LINE__; return; case __LINE__:         /* 等c_ledc_pwm_step_ms后再改下一次 */
//             if(tick_cmp(tmr, c_ledc_pwm_step_ms) != c_ret_ok)
//                 return;
//              /* 设置占空比 */
//             ledc_pwm_set_duty(ledpwmval);
//             dbgtx("ledpwmval:%d, tmr:%d\n", ledpwmval, tmr);
//             step = 0;
//         break;

//         default:
//             step = 0;
//         break;
//     }
// }

// void ledc_pwm_set_duty(uint16_t duty)
// {
//     uint32_t duty_tmp = ledc_duty_pow(duty, 2, LEDC_TIMER_14_BIT);
//     ledc_set_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0, duty_tmp);    /* 设置占空比 */
//     ledc_update_duty(LEDC_LOW_SPEED_MODE, LEDC_CHANNEL_0);                    /* 更新占空比 */
// }

// /**
//  * @brief       初始化LEDC
//  * @param       ledc_config: ledc配置结构体
//  * @retval      无
//  */
// void ledc_init(void)
// {
//     unsigned int duty = 0;
//     duty = ledc_duty_pow(duty, 2, LEDC_TIMER_14_BIT);

//     ledc_timer_config_t ledc_timer = {
//         .speed_mode       = LEDC_LOW_SPEED_MODE,            /* 低速模式(ESP32P4仅支持低速模式) */
//         .duty_resolution  = LEDC_TIMER_14_BIT,   /* 占空比分辨率 */
//         .timer_num        = LEDC_TIMER_0,         /* 定时器选择 */
//         .freq_hz          = 1000,           /* 设置频率 */
//         .clk_cfg          = LEDC_AUTO_CLK            /* 设置时钟源 */
//     };
//     /* 配置ledc定时器 */
//     ESP_ERROR_CHECK(ledc_timer_config(&ledc_timer));

//     /* 配置pwm通道 */
//     ledc_channel_config_t ledc_channel = {
//         .speed_mode     = LEDC_LOW_SPEED_MODE,      /* 低速模式 */
//         .channel        = LEDC_CHANNEL_0,     /* PWM输出通道 */
//         .timer_sel      = LEDC_TIMER_0,   /* 那个定时器提供计数值 */
//         .intr_type      = LEDC_INTR_DISABLE,        /* 关闭LEDC中断 */
//         .gpio_num       = LEDC_PWM_CH0_GPIO,    /* 输出GPIO管脚 */
//         .duty           = duty,        /* 占空比 */
//         .hpoint         = 0                         /* 设置hpoint数值 */
//     };
//     /* Lpoint = duty + hpoint */

//     /* 配置pwm通道 */
//     ESP_ERROR_CHECK(ledc_channel_config(&ledc_channel));

// /* 注册到框架任务, 由task_proc周期调用 */
//     task_add("task_ledc_pwm", task_ledc_pwm, c_auto_quit, 0);
// }
// //INIT_REG(ledc_init, 5);   /* 暂屏蔽: GPIO1被LEDC占用时demo任务无法闪灯, 需要时恢复 */
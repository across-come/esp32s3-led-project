#include "user_ext.h"
#include "task_xl9555.h"
#include "iic.h"

/* ============ 读取XL9555的IO值 ============
 * 从输入寄存器0开始连续读len字节(读2字节得到P0/P1全部IO状态)
 */
unsigned int xl9555_read_byte(uint8_t *data, size_t len)
{
    if(i2c_read(XL9555_ADDR, XL9555_INPUT_PORT0_REG, data, len) != c_ret_ok)
        return c_ret_nk;
    return c_ret_ok;
}

/* ============ 向XL9555寄存器写入数据 ============ */
unsigned int xl9555_write_byte(uint8_t reg, uint8_t *data, size_t len)
{
    if(i2c_write(XL9555_ADDR, reg, data, len) != c_ret_ok)
        return c_ret_nk;
    return c_ret_ok;
}

/* ============ 控制某个IO的电平 ============ */
uint16_t xl9555_pin_write(uint16_t pin, int val)
{
    uint8_t w_data[2];
    uint16_t temp = 0x0000;

    xl9555_read_byte(w_data, 2);

    if (pin <= 0x0080)
    {
        if (val)
        {
            w_data[0] |= (uint8_t)(0xFF & pin);
        }
        else
        {
            w_data[0] &= ~(uint8_t)(0xFF & pin);
        }
    }
    else
    {
        if (val)
        {
            w_data[1] |= (uint8_t)(0xFF & (pin >> 8));
        }
        else
        {
            w_data[1] &= ~(uint8_t)(0xFF & (pin >> 8));
        }
    }

    temp = ((uint16_t)w_data[1] << 8) | w_data[0];

    xl9555_write_byte(XL9555_OUTPUT_PORT0_REG, w_data, 2);

    return temp;
}

/* ============ 获取某个IO状态 ============ */
int xl9555_pin_read(uint16_t pin)
{
    uint16_t ret;
    uint8_t r_data[2];

    xl9555_read_byte(r_data, 2);

    ret = r_data[1] << 8 | r_data[0];

    return (ret & pin) ? 1 : 0;
}

/* ============ XL9555的IO方向配置 ============
 * config_value: 0为输出, 1为输入
 */
unsigned int xl9555_ioconfig(uint16_t config_value)
{
    uint8_t data[2];

    data[0] = (uint8_t)(0xFF & config_value);
    data[1] = (uint8_t)(0xFF & (config_value >> 8));

    if(xl9555_write_byte(XL9555_CONFIG_PORT0_REG, data, 2) != c_ret_ok)
    {
        dbgtx("xl9555_ioconfig %X nk\r\n", config_value);
        return c_ret_nk;
    }

    return c_ret_ok;
}

unsigned int xl9555_input_get(uint16_t *val)
{
    uint8_t data[2];

    if(val == NULL)
        return c_ret_nk;
    if(xl9555_read_byte(data, 2) != c_ret_ok)
        return c_ret_nk;
    *val = ((uint16_t)data[1] << 8) | data[0];
    return c_ret_ok;
}

/* ============ 输出演示任务: 板载LED0闪烁 ============
 * 注意: 需屏蔽ledc_init的INIT_REG注册(GPIO1与LEDC_PWM_CH0共用), 否则PWM占着引脚不闪
 */
#define c_xl9555_demo_en   1

void xl9555_demo_proc(void)
{
    #define c_step_led_on   0
    #define c_step_led_off  1
    #define c_step_wait     3

    switch(sys.curr_task->step)
    {
        case c_step_led_on:
            LED0(0);                            /* 点亮 */
            sys.curr_task->tmr = tick_get();
            sys.curr_task->step = c_step_led_off;
        break;

        case c_step_led_off:
            if(tick_cmp(sys.curr_task->tmr, 500) != c_ret_ok) return;
            LED0(1);                            /* 熄灭 */
            sys.curr_task->tmr = tick_get();
            sys.curr_task->step = c_step_wait;
        break;

        case c_step_wait:
            if(tick_cmp(sys.curr_task->tmr, 500) != c_ret_ok) return;
            sys.curr_task->step = c_step_led_on;
        break;

        default:
            sys.curr_task->step = c_step_led_on;
        break;
    }
}

/* ============ 初始化XL9555 ============ */
void xl9555_init(void)
{
    uint8_t r_data[2];

    /* 上电先读取一次清除中断标志 */
    xl9555_read_byte(r_data, 2);
    /* 配置那些扩展管脚为输入输出模式 (P0.0/P0.1/KEY0~3为输入, 其余输出) */
    xl9555_ioconfig(0xF003);
    if(xl9555_read_byte(r_data, 2) == c_ret_ok)
        dbgtx("xl9555 in=0x%04x\r\n", ((uint16_t)r_data[1] << 8) | r_data[0]);
    else
        dbgtx("xl9555 in nk\r\n");
    /* 关闭蜂鸣器 */
    xl9555_pin_write(BEEP_IO, 1);
    /* 关闭喇叭 */
    xl9555_pin_write(SPK_EN_IO, 1);
    /* LCD相关引脚驱到断电电平 (ioconfig后默认输出0, P1.3低电平会给LCD上电) */
    xl9555_pin_write(SLCD_PWR_IO, 0);   /* SPI LCD电源关(PWR高有效) */
    xl9555_pin_write(LCD_BL_IO, 0);     /* RGB背光关 */
    xl9555_pin_write(SLCD_RST_IO, 1);   /* SPI LCD复位释放 */
    xl9555_pin_write(CT_RST_IO, 1);     /* 触摸复位释放 */

#if c_xl9555_demo_en == 1
    task_add("task_xdemo", xl9555_demo_proc, c_auto_quit, 0);
#endif

    dbgtx("xl9555_init ok\r\n");
}
INIT_REG(xl9555_init, 4);       /* 优先级4: 必须晚于iic_init(2), 先于ledc_init(5)也行 */

/* ============ 外部中断服务函数 (可选: 输入变化时INT拉低) ============ */
static volatile unsigned int xl9555_int_flag = 0;

static void IRAM_ATTR xl9555_exit_gpio_isr_handler(void *arg)
{
    xl9555_int_flag = 1;
}

/* ============ 外部中断初始化 (默认不使能, 按键任务已轮询) ============ */
unsigned int xl9555_int_init(void)
{
    gpio_config_t gpio_init_struct = {0};

    gpio_init_struct.mode         = GPIO_MODE_INPUT;        /* 选择为输入模式 */
    gpio_init_struct.pull_up_en   = GPIO_PULLUP_ENABLE;     /* 上拉使能 */
    gpio_init_struct.pull_down_en = GPIO_PULLDOWN_DISABLE;  /* 下拉失能 */
    gpio_init_struct.intr_type    = GPIO_INTR_NEGEDGE;      /* 下降沿触发 */
    gpio_init_struct.pin_bit_mask = 1ull << XL9555_INT_IO;  /* 设置的引脚的位掩码 */
    gpio_config(&gpio_init_struct);                         /* 配置使能 */

    gpio_install_isr_service(0);

    gpio_isr_handler_add(XL9555_INT_IO, xl9555_exit_gpio_isr_handler, (void*)XL9555_INT_IO);

    return c_ret_ok;
}
#include "user_ext.h"
#include "task_at24c02.h"

/* ============ 读写演示任务: 计数值断电保存, 每1s+1写回并打印 ============ */
#define c_at24c02_demo_en   0

void at24c02_demo_proc(void)
{
    #define c_step_read     0
    #define c_step_write    1
    static uint8_t cnt = 0;

    switch(sys.curr_task->step)
    {
        case c_step_read:                       /* 开机读一次上次保存的计数值 */
            at24c02_read_one_byte(0, &cnt);
            dbgtx("at24c02 cnt:%d\r\n", cnt);
            sys.curr_task->tmr = tick_get();
            sys.curr_task->step = c_step_write;
        break;

        case c_step_write:                      /* 每1s 计数值+1 写回 */
            if(tick_cmp(sys.curr_task->tmr, 1000) != c_ret_ok) return;
            cnt++;
            at24c02_write_one_byte(0, cnt);
            dbgtx("at24c02 cnt:%d\r\n", cnt);
            sys.curr_task->tmr = tick_get();
        break;

        default:
            sys.curr_task->step = c_step_read;
        break;
    }
}

/* ============ 初始化AT24C02 (总线由iic_init建立, 此处只做自检) ============ */
void at24c02_init(void)
{
    if(at24c02_check() == c_ret_ok)
        dbgtx("at24c02 check ok\r\n");
    else
        dbgtx("at24c02 check nk\r\n");

#if c_at24c02_demo_en == 1
    task_add("task_at24c02", at24c02_demo_proc, c_auto_quit, 0);
#endif
}
INIT_REG(at24c02_init, 3);

/* ============ 在AT24C02指定地址读出一个数据 ============ */
unsigned int at24c02_read_one_byte(uint8_t addr, uint8_t *pdata)
{
    return i2c_read(AT_ADDR, addr, pdata, 1);
}

/* ============ 在AT24C02指定地址写入一个数据 ============ */
unsigned int at24c02_write_one_byte(uint8_t addr, uint8_t data)
{
    unsigned int ret;

    ret = i2c_write(AT_ADDR, addr, &data, 1);

    dly_ms(10);     /* AT24C02写周期约5ms, 等写完才能继续操作 */
    return ret;
}

/* ============ 检查AT24C02是否正常 ============
 * 检测原理: 在末地址写0x55再读回, 一致则正常
 */
unsigned int at24c02_check(void)
{
    uint8_t temp;

    if(at24c02_read_one_byte(AT24C02, &temp) != c_ret_ok)
        return c_ret_nk;

    if(temp == 0x55)                            /* 读取数据正常 */
        return c_ret_ok;

    if(at24c02_write_one_byte(AT24C02, 0x55) != c_ret_ok)   /* 第一次初始化, 先写入 */
        return c_ret_nk;
    if(at24c02_read_one_byte(AT24C02, &temp) != c_ret_ok)
        return c_ret_nk;
    if(temp == 0x55)
        return c_ret_ok;

    return c_ret_nk;
}

/* ============ 从指定地址开始读出len字节 (24C02支持顺序读) ============ */
unsigned int at24c02_read(uint8_t addr, uint8_t *pbuf, uint8_t datalen)
{
    return i2c_read(AT_ADDR, addr, pbuf, datalen);
}

/* ============ 从指定地址开始写入len字节 (逐字节写+等写周期) ============ */
unsigned int at24c02_write(uint8_t addr, uint8_t *pbuf, uint8_t datalen)
{
    while(datalen--)
    {
        if(at24c02_write_one_byte(addr, *pbuf) != c_ret_ok)
            return c_ret_nk;
        addr++;
        pbuf++;
    }
    return c_ret_ok;
}
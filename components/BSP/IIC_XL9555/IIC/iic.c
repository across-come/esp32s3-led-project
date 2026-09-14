#include "user_ext.h"
#include "iic.h"

/* ============ 软件IIC底层操作 ============
 * SCL/SDA配置为开漏输出(内部上拉), 输出1=释放线路, 读SDA即读线路实际电平
 */
#define i2c_scl_high()      gpio_set_level(IIC_SCL_GPIO_PIN, 1)
#define i2c_scl_low()       gpio_set_level(IIC_SCL_GPIO_PIN, 0)
#define i2c_sda_high()      gpio_set_level(IIC_SDA_GPIO_PIN, 1)
#define i2c_sda_low()       gpio_set_level(IIC_SDA_GPIO_PIN, 0)
#define i2c_sda_read()      gpio_get_level(IIC_SDA_GPIO_PIN)

/* 时钟相位延时us (4us≈100KHz), 可调大放慢速度, 适配长线/慢从机 */
#define i2c_dly(us)         dly_ns(us)

/* ============ 起始信号 ============ */
static void i2c_start(void)
{
    i2c_scl_low();
    i2c_sda_high();
    i2c_scl_high();
    i2c_dly(4);
    i2c_sda_low();
    i2c_dly(4);
    i2c_scl_low();
    i2c_dly(4);
}

/* ============ 停止信号 ============ */
static void i2c_stop(void)
{
    i2c_scl_low();
    i2c_sda_low();
    i2c_scl_high();
    i2c_dly(4);
    i2c_sda_high();
    i2c_dly(8);
}

/* ============ 发送一个字节并检查ACK ============ */
static unsigned int i2c_sendbyte_checkack(unsigned char dat)
{
    unsigned int x, ret;
    for(x = ret = 0; x < 8; x++)
    {
        i2c_scl_low();
        if(dat & 0x80)  i2c_sda_high();
        else            i2c_sda_low();
        dat <<= 1;
        i2c_dly(4);
        i2c_scl_high();
        i2c_dly(4);
    }
    i2c_scl_low();
    i2c_sda_high();                     /* 释放SDA, 从机拉低应答 */
    i2c_dly(4);
    i2c_scl_high();
    i2c_dly(2);
    ret |= i2c_sda_read();
    i2c_dly(2);
    i2c_scl_low();
    return ret ? c_ret_nk : c_ret_ok;
}

/* ============ 读一个字节, 发NACK(最后一位) ============ */
static unsigned int i2c_readbyte_sendnack(void)
{
    unsigned int x, ret;

    i2c_scl_low();
    i2c_sda_high();

    for(x = ret = 0; x < 8; x++)
    {
        i2c_scl_low();
        i2c_dly(4);
        i2c_scl_high();
        i2c_dly(2);
        ret <<= 1;
        ret |= i2c_sda_read();
        i2c_dly(2);
    }

    i2c_scl_low();
    i2c_sda_high();                     /* NACK */
    i2c_dly(4);
    i2c_scl_high();
    i2c_dly(4);
    i2c_scl_low();
    return ret;
}

/* ============ 读一个字节, 发ACK(中间位) ============ */
static unsigned int i2c_readbyte_sendack(void)
{
    unsigned int ret, x;

    i2c_scl_low();
    i2c_sda_high();                     /* 控制权交给从机 */

    for(ret = x = 0; x < 8; x++)
    {
        i2c_scl_low();
        i2c_dly(4);                     /* 等待从机发, 此时scl为低, sda可改变 */
        i2c_scl_high();
        i2c_dly(2);
        ret <<= 1;
        ret |= i2c_sda_read();
        i2c_dly(2);
    }

    i2c_scl_low();
    i2c_sda_low();                      /* 发送ACK给从机 */
    i2c_dly(4);
    i2c_scl_high();
    i2c_dly(4);
    i2c_scl_low();                      /* 结束发送周期 */
    return ret;
}

/* ============ 总线错误恢复: 9个时钟脉冲 ============
 * 主设备释放SDA只打SCL时钟, 让从机状态机/位计数器复位,
 * 同时给从机机会完成内部操作(如EEPROM写周期), 再发STOP
 */
static void i2c_error(void)
{
    unsigned int x;

    i2c_scl_low();
    i2c_sda_high();                     /* 从机控制发送 */

    for(x = 0; x < 9; x++)              /* 9个时钟脉冲 (8位数据+1位ACK) */
    {
        i2c_scl_low();
        i2c_dly(4);
        i2c_scl_high();
        i2c_dly(4);
    }
    i2c_stop();
}

/* ============ 读数据 ============
 * addr != 0xFFFFFFFF: 写寄存器后重复START, 再 SLA+R 读 (对齐 IDF transmit_receive)
 * addr == 0xFFFFFFFF: 只发设备地址(读位)直接读
 * 失败自动9脉冲恢复总线并重试3次
 */
unsigned int i2c_read(unsigned int saddr, unsigned int addr, unsigned char *buf, unsigned int len)
{
    unsigned int cnt, idx, x;
    unsigned char tmp[3];

    if((buf == NULL) || (len == 0))
        return c_ret_nk;

    idx = 0;
    tmp[idx++] = (saddr << 1);              /* 7位地址左移1位+写位 */

    if(addr != 0xFFFFFFFF)
    {
        if(addr > 0xFF)
        {
            tmp[idx++] = (addr >> 8) & 0xFF;
            tmp[idx++] = addr & 0xFF;
        }
        else
        {
            tmp[idx++] = (unsigned char)addr;
        }
    }

    for(cnt = 0; cnt < 3; cnt++)
    {
        if(addr != 0xFFFFFFFF)
        {
            i2c_start();
            for(x = 0; x < idx; x++)
            {
                if(i2c_sendbyte_checkack(tmp[x]) == c_ret_nk)
                {
                    i2c_error();
                    break;
                }
            }
            if(x < idx)
                continue;
            /* 禁止在这里 STOP: 否则从机释放总线, 后面空时钟读到上拉全1 */
        }

        i2c_start();                        /* 重复START, 转读 */
        if(i2c_sendbyte_checkack((saddr << 1) | 1) == c_ret_nk)
        {
            i2c_error();
            continue;
        }

        for(x = 0; x < len - 1; x++)
            buf[x] = i2c_readbyte_sendack();
        buf[x] = i2c_readbyte_sendnack();

        i2c_stop();
        return c_ret_ok;
    }
    return c_ret_nk;
}

/* ============ 写数据 ============
 * addr != 0xFFFFFFFF: 发设备地址+寄存器地址+数据
 * addr == 0xFFFFFFFF: 只发设备地址+数据
 * 失败自动9脉冲恢复总线并重试3次
 */
unsigned int i2c_write(unsigned int saddr, unsigned int addr, unsigned char *buf, unsigned int len)
{
    unsigned int cnt, idx, x;
    unsigned char tmp[3];

    idx = 0;

    tmp[idx++] = (saddr << 1);              /* 7位地址左移1位+写位 */
    if(addr > 0xff)
    {
        tmp[idx++] = (addr >> 8) & 0xff;
        tmp[idx++] = addr & 0xff;
    }
    else
    {
        tmp[idx++] = addr;
    }

    for(cnt = 0; cnt < 3; cnt++)
    {
        i2c_start();
        if(addr != 0xffffffff)
        {
            for(x = 0; x < idx; x++)
            {
                if(i2c_sendbyte_checkack(tmp[x]) == c_ret_nk)
                {
                    i2c_error();
                    break;
                }
            }
            if(x < idx) continue;           /* 重试 */
        }
        else
        {
            if(i2c_sendbyte_checkack(tmp[0]) == c_ret_nk)
            {
                i2c_error();
                continue;
            }
        }

        for(x = 0; x < len; x++)
        {
            if(i2c_sendbyte_checkack(buf[x]) == c_ret_nk)
            {
                i2c_error();
                break;
            }
        }
        if(x < len) continue;               /* 重试 */

        i2c_stop();
        return c_ret_ok;
    }
    return c_ret_nk;
}

/* ============ IIC总线扫描: 打印总线上有应答的从机地址 ============ */
void i2c_test(void)
{
    unsigned int x, cnt;
    unsigned char tmp[1];

    for(cnt = 0; cnt < 255; cnt++)
    {
        tmp[0] = cnt;
        for(x = 0; x < 3; x++)
        {
            i2c_start();
            if(i2c_sendbyte_checkack((tmp[0] << 1)) == c_ret_nk)
            {
                i2c_error();
                break;
            }
            else
            {
                dbgtx("i2c_addr_test:0x%x(%d)\r\n", tmp[0], tmp[0]);
                break;
            }
        }
        i2c_stop();
    }
}

/* ============ 软件IIC引脚初始化 (开漏输出+内部上拉) ============ */
void iic_init(void)
{
    gpio_config_t gpio_init_struct = {0};

    gpio_init_struct.intr_type = GPIO_INTR_DISABLE;
    gpio_init_struct.mode = GPIO_MODE_INPUT_OUTPUT_OD;      /* 开漏: 输出1即释放线路 */
    gpio_init_struct.pull_up_en = GPIO_PULLUP_ENABLE;       /* 内部上拉 */
    gpio_init_struct.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_init_struct.pin_bit_mask = (1ULL << IIC_SCL_GPIO_PIN) |
                                    (1ULL << IIC_SDA_GPIO_PIN);
    ESP_ERROR_CHECK(gpio_config(&gpio_init_struct));

    gpio_set_level(IIC_SCL_GPIO_PIN, 1);
    gpio_set_level(IIC_SDA_GPIO_PIN, 1);

    dbgtx("soft_iic_init ok\r\n");
}
INIT_REG(iic_init, 2);      /* 优先级2: 必须先于挂载到总线上的设备(xl9555_init/at24c02_init) */
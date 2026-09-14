#ifndef __TASK_AT24C02_H
#define __TASK_AT24C02_H

#include "iic.h"

/* 24C02设备地址 */
#define AT_ADDR         0x50
#define AT24C02         255                 /* 24C02容量255字节, 末地址 */

/* 函数声明 */
void at24c02_init(void);                                            /* 初始化AT24C02 */
unsigned int at24c02_check(void);                                   /* 检查器件: c_ret_ok正常 */
unsigned int at24c02_read_one_byte(uint8_t addr, uint8_t *pdata);   /* 指定地址读取一个字节 */
unsigned int at24c02_write_one_byte(uint8_t addr, uint8_t data);    /* 指定地址写入一个字节 */
unsigned int at24c02_write(uint8_t addr, uint8_t *pbuf, uint8_t datalen);    /* 从指定地址写len字节 */
unsigned int at24c02_read(uint8_t addr, uint8_t *pbuf, uint8_t datalen);     /* 从指定地址读len字节 */

#endif
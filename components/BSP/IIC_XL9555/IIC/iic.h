#ifndef __IIC_H
#define __IIC_H

#include "driver/gpio.h"

/* IIC0 引脚与参数定义 */
#define IIC_SDA_GPIO_PIN   GPIO_NUM_41      /* IIC0_SDA引脚 */
#define IIC_SCL_GPIO_PIN   GPIO_NUM_42      /* IIC0_SCL引脚 */

/* ============ 软件IIC通用接口 ============
 * saddr: 从机7位地址
 * addr : 寄存器地址, 8位/16位自动识别(>0xFF发两个字节); 0xFFFFFFFF=无寄存器地址直接读写
 * buf  : 数据缓冲
 * len  : 数据长度
 * 返回: c_ret_ok成功 / c_ret_nk失败(内部自动9脉冲恢复总线并重试3次)
 */
unsigned int i2c_read(unsigned int saddr, unsigned int addr, unsigned char *buf, unsigned int len);
unsigned int i2c_write(unsigned int saddr, unsigned int addr, unsigned char *buf, unsigned int len);

void iic_init(void);                         /* 初始化软件IIC引脚 */

#endif
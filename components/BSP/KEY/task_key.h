#ifndef __task_key_h__
#define __task_key_h__

#include "user_ext.h"

/* 板载 KEY0~3: XL9555 P1.7~P1.4 低有效, 扫描/长短按在 task_key.c
 * BOOT: ESP32 GPIO0
 */
#define BOOT_GPIO_PIN   GPIO_NUM_0

#define KEY_PRESS_LEVEL     0
#define KEY_RELEASED_LEVEL  1

#define c_key_num          4
#define c_key_db_ms        20
#define c_key_long_ms      1000
#define c_key_scan_ms      10

#define c_key_val_long     0x80
#define c_key_val_0        0x01
#define c_key_val_1        0x02
#define c_key_val_2        0x04
#define c_key_val_3        0x08

/* 短按 KEY0=STA  KEY1=上  KEY2=下  KEY3=返回
 * 长按 KEY0=采集  KEY1=扫描  KEY2=TCP开关  KEY3=回主页
 */

#endif

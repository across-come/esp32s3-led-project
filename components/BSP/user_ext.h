#ifndef __user_ext_h__
#define __user_ext_h__

#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/uart_select.h"
#include "driver/uart.h"
#include "driver/gpio.h"

#include "task_led.h"
#include "task_key.h"
#include "user_bsp.h"
#include "user_dbg.h"

#define c_ret_nk    0
#define c_ret_ok    1
#define c_ret_wt    2

#endif

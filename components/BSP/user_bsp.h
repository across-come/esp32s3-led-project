#ifndef __user_bsp_h__
#define __user_bsp_h__

#include "user_ext.h"
#include "user_main.h"

/* UART 调试串口 */
#define USART_UX            UART_NUM_0
#define USART_TX_GPIO_PIN   GPIO_NUM_43
#define USART_RX_GPIO_PIN   GPIO_NUM_44

/* 内存池 (128KB, 块16B, 静态.bss数组在内部SRAM, DMA可达)
 * ESP32-S3内部SRAM 512KB, IDF可用动态RAM约374KB(实测321+21+32KB),
 * 内存池建议≤192KB, 128KB稳妥; 8MB PSRAM非DMA可达只能放非DMA缓冲 */
#define c_mem_buf_size  (128 * 1024)
#define c_mem_blk_size  16
#define c_mem_blk_num   (c_mem_buf_size / c_mem_blk_size)

#ifndef NULL
#define NULL    ((void*)0)
#endif

#define c_done_ok       0
#define c_done_nk       1
#define c_done_wt       2

#define arr_size(arr)   (sizeof(arr) / sizeof(arr[0]))

extern unsigned int int_cnt;
#define int_en()        do{if(int_cnt) int_cnt--; if(int_cnt == 0) portENABLE_INTERRUPTS();}while(0)
#define int_dis()       do{int_cnt++; portDISABLE_INTERRUPTS();}while(0)

/* 初始化注册: 构造函数注册进表, user_init()统一遍历执行 */
typedef void (*init_cb)(void);
#define c_init_max  24
unsigned int init_register(unsigned int prio, init_cb func);
#define INIT_REG(fn, n) __attribute__((constructor)) static void __init_reg_##fn(void) { init_register((n), fn); }
extern void user_init(void);

/* 调试追踪 (ESP32暂不启用) */
#define c_trace_en  0
#if c_trace_en == 1
#define __trace__   trace_proc((char*)__func__);
#else
#define __trace__
#endif

#define dbgtx_fl(format, ...)			dbgtx("[%s<%d>]: " format, __func__, __LINE__, ##__VA_ARGS__)

/* 高精度时间: ESP32用esp_timer(us), 无DWT */
#define c_dwt_us    1
extern unsigned int dwt_get(void);
extern unsigned int dwt_get_us(void);
extern unsigned int dwt_cmp(unsigned int tmr, unsigned int tmo);
extern void dly_ns(unsigned int us);
extern void dly_ms(unsigned int ms);

extern unsigned int tick_get(void);
extern unsigned int tick_cmp(unsigned int tmr, unsigned int tmo);

extern void *bsp_alloc(unsigned int size);
extern unsigned int bsp_free(void *px);
extern void *bsp_realloc(void *ptr, unsigned int size);

extern unsigned int str2num(char *buf, unsigned int *wx, double *dx);

/* KV (无名队列) */
extern struct st_kv *kv_add(unsigned int len);
extern unsigned int kv_del(struct st_kv *p_kv);

/* FLAG */
extern struct st_flag *flag_add(char *name, flag_func func, unsigned int arg_len);
extern void flag_set(struct st_flag *p_item);
extern void flag_proc(void);

/* TASK */
extern void task_del(const char *name);
extern struct st_task *task_add(const char *name, task_func func, unsigned int auto_quit, unsigned int arg_len);
extern struct st_task *task_get(const char *name);
extern void task_proc(void);

#endif
#ifndef __user_dbg_h__
#define __user_dbg_h__

#include "user_ext.h"

/* UART/打印 */
extern void uart_write(unsigned char *buf, unsigned int len);
extern void dbgtx(char *fmt, ...);
extern void print_version(void);

/* MSG */
extern unsigned int msg_send(char *name, char *dat);
extern unsigned int msg_add(char *name, msg_func func);

/* VAR */
extern unsigned int var_add(char *name, unsigned int addr, unsigned int type);

/* EXE */
extern unsigned int exe_add(char *name, char *desc, exe_func func);

/* DBG */
extern void bsp_init(void);
extern void uart_init(void);

#endif
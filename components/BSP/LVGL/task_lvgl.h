#ifndef __TASK_LVGL_H
#define __TASK_LVGL_H

void task_lvgl_proc(void);
void task_lvgl_init(void);
unsigned int lvgl_ui_ready(void);               /* 1=LVGL 已接管画屏, 旧 task_lcd 文案应停 */
unsigned int task_lvgl_msg_parse(char *buf);    /* up/down/ok/back / page:xxx */

#endif

#ifndef __TASK_DATA_H
#define __TASK_DATA_H

#define c_dg_interval_ms    5000    /* 数据产生间隔(ms) */

void data_gen_init(void);
void data_gen_proc(void);
unsigned int task_data_msg_parse(char *buf);
unsigned int data_gen_seq_get(void);
const char *data_gen_last(void);

#endif

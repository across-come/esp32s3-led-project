#ifndef __TASK_SD_H
#define __TASK_SD_H

/* TF卡任务. 与 TCP/data 只通过字符串消息互通
 * 入 task_sd_msg:   setup / stop / append:<line> / get / del / sd?
 * 出 task_tcp_msg:  cache:pending / cache:empty / cache:fail / cache:ready:<file>
 * 底层: 正点原子 25_sd, SPI2 + esp_vfs_fat_sdspi_mount; 文件队列对齐 3x8c (get占住, 确认后 del)
 */

#define c_sd_line_max       64          /* 单条缓存数据 */
#define c_sd_q_max          128         /* append 排队深度 */
#define c_sd_file_max       2100        /* 整文件读出发给TCP */

/* 任务私有: task_add 分配, 每次 setup 新建并清零 */
struct st_task_sd_run
{
    unsigned int    mount_ok;           /* 1=本轮 mount+cache_init 成功; 失败仍进 ready, 但 get/del 只回 empty */
};

/* parse <-> proc 握手 */
struct st_task_sd_set
{
    unsigned int    setup_req_seq;
    unsigned int    setup_req_seq_bk;
    unsigned int    stop_req_seq;
    unsigned int    stop_req_seq_bk;
    unsigned int    get_req_seq;
    unsigned int    get_req_seq_bk;
    unsigned int    del_req_seq;
    unsigned int    del_req_seq_bk;
};

extern struct st_task_sd_set task_sd_set;

unsigned int task_sd_msg_parse(char *buf);
void task_sd_proc(void);
void task_sd_init(void);

void sd_cs_idle(void);                  /* 兼容旧接口; 硬件SDSPI下CS由驱动管理, 空操作 */
const char *sd_fat_drv(void);           /* 已挂载返回 "0:" 等盘符, 否则NULL */
unsigned int sd_mount_fail(void);       /* 最近一次 mount 失败 */

#endif

#include "user_ext.h"
#include "task_sd.h"
#include "spi.h"
#include "cache_store.h"
#include "driver/sdspi_host.h"
#include "sdmmc_cmd.h"
#include "esp_vfs_fat.h"
#include "esp_err.h"
#include "esp_log.h"
#include "ff.h"

/* ======================================================================
 * TF卡: 正点原子 25_sd (SPI2 + IDF SDSPI) + FAT32 + parse/任务状态机
 * 文件队列对齐 3x8c: append 落盘, get 占住, TCP 确认后 del 才推进
 * SCK/MOSI 与 LCD 共用 SPI2, CS 分时: LCD=IO21 TF=IO2, MISO=IO13
 * ====================================================================== */

#define SD_NUM_CS           GPIO_NUM_2
#define SD_MOUNT_POINT      "/sdcard"

static const char *TAG = "sd";

struct st_task_sd_set task_sd_set = {0};

static char s_file[c_sd_file_max];              /* get 成功后的整文件 */
static unsigned int s_file_len = 0;
static char s_ready_msg[c_sd_file_max + 16];    /* cache:ready:<file>, 静态避免占调度栈 */

/* IDF 挂载结果, stop 时 unmount 不释放 SPI2 (LCD 还在用) */
struct st_sd_vol
{
    sdmmc_card_t   *card;           /* esp_vfs_fat_sdspi_mount 填入 */
    char            drv[4];         /* FatFs 盘符 "0:" */
    unsigned char   ok;             /* 1=已挂载 */
};

/* parse 入队, proc 出队写卡; hold/pend 与 TCP get/del 握手 */
struct st_sd_q
{
    char            line[c_sd_q_max][c_sd_line_max];    /* append 环形队列 */
    unsigned int    head;           /* 下一笔要写出的下标 */
    unsigned int    tail;           /* 下一笔要写入的下标 */
    unsigned int    cnt;            /* 当前排队条数 */
    unsigned int    hold;           /* 1=已 get 未 del, 文件占住不推进队头 */
    unsigned int    pend_ntf;       /* 1=已发过 cache:pending, 避免每写一行都通知 */
};

static struct st_sd_vol s_vol = {0};
static unsigned int s_sd_fail = 0;
static struct st_sd_q s_q = {0};

static unsigned int sd_q_push(const char *line, unsigned int len);
static unsigned int sd_q_pop(char *out, unsigned int out_n);
static void sd_notify_tcp(const char *dat);
static void sd_notify_ready(void);
static void sd_notify_pending(void);
static void sd_notify_after_del(void);
static esp_err_t sd_mount(void);
static esp_err_t sd_unmount(void);

/* ============ SD任务 串口指令 ============
 * exe=dbg_msg_send(msg:task_sd_msg,dat:help)
 * exe=dbg_msg_send(msg:task_sd_msg,dat:setup)
 * exe=dbg_msg_send(msg:task_sd_msg,dat:stop)
 * exe=dbg_msg_send(msg:task_sd_msg,dat:sd?)
 * exe=dbg_msg_send(msg:task_sd_msg,dat:append:[SEQ=1 T=1 R=1])
 * exe=dbg_msg_send(msg:task_sd_msg,dat:get)
 * exe=dbg_msg_send(msg:task_sd_msg,dat:del)
 */
unsigned int task_sd_msg_parse(char *buf)
{
    const char *pay;
    unsigned int len;

    if(buf == NULL)
        return c_ret_nk;

    if(strcmp(buf, "help") == 0)
    {
        dbgtx("setup  (start task + mount fat32 + cache)\r\n");
        dbgtx("stop\r\n");
        dbgtx("sd?\r\n");
        dbgtx("append:<line>\r\n");
        dbgtx("get\r\n");
        dbgtx("del\r\n");
        return c_ret_ok;
    }
    else if(strcmp(buf, "sd?") == 0)
    {
        dbgtx("sd task=%s q=%u hold=%u file_len=%u\r\n",
              (task_get("task_sd") != NULL) ? "run" : "idle",
              s_q.cnt, s_q.hold, s_file_len);
        return c_ret_ok;
    }
    else if(strcmp(buf, "setup") == 0)
    {
        task_sd_set.setup_req_seq++;
        if(task_get("task_sd") == NULL)
            task_add("task_sd", task_sd_proc, c_auto_quit, sizeof(struct st_task_sd_run));
        return c_ret_ok;
    }
    else if(strcmp(buf, "stop") == 0)
    {
        if(task_get("task_sd") == NULL)
            return c_ret_nk;
        task_sd_set.stop_req_seq++;
        return c_ret_ok;
    }
    else if(strncmp(buf, "append:", 7) == 0)
    {
        pay = buf + 7;
        len = (unsigned int)strlen(pay);
        if(sd_q_push(pay, len) != c_ret_ok)
        {
            dbgtx("sd append q full\r\n");
            return c_ret_nk;
        }
        return c_ret_ok;
    }
    else if(strcmp(buf, "get") == 0)
    {
        task_sd_set.get_req_seq++;
        return c_ret_ok;
    }
    else if(strcmp(buf, "del") == 0)
    {
        task_sd_set.del_req_seq++;
        return c_ret_ok;
    }
    return c_ret_nk;
}

void task_sd_proc(void)
{
    #define c_step_idle     0
    #define c_step_mount    1
    #define c_step_ready    2
    struct st_task_sd_run *p_task_arg;
    char line[c_sd_line_max];
    char fname[CACHE_PATH_MAX];
    unsigned int n;

    p_task_arg = (struct st_task_sd_run *)sys.curr_task->dat;

    if(task_sd_set.stop_req_seq_bk != task_sd_set.stop_req_seq)
    {
        task_sd_set.stop_req_seq_bk = task_sd_set.stop_req_seq;
        sd_unmount();
        p_task_arg->mount_ok = 0;
        s_q.hold = 0;
        s_q.pend_ntf = 0;
        s_file_len = 0;
        sys.curr_task->step = c_step_idle;
        sys.curr_task->done = c_done_nk;
        return;
    }

    switch(sys.curr_task->step)
    {
        case c_step_idle:
            if(task_sd_set.setup_req_seq_bk == task_sd_set.setup_req_seq)
                return;
            task_sd_set.setup_req_seq_bk = task_sd_set.setup_req_seq;
            p_task_arg->mount_ok = 0;
            s_q.hold = 0;
            sys.curr_task->step = c_step_mount;
        break;

        case c_step_mount:
            if(sd_mount() != ESP_OK)
            {
                dbgtx("sd mount fail\r\n");
                ESP_LOGE(TAG, "mount fail");
                sd_notify_tcp("cache:fail");
                sys.curr_task->step = c_step_ready;
                return;
            }
            if(cache_init() != c_ret_ok)
            {
                dbgtx("sd cache init fail\r\n");
                ESP_LOGE(TAG, "cache init fail");
                sd_notify_tcp("cache:fail");
                sys.curr_task->step = c_step_ready;
                return;
            }
            p_task_arg->mount_ok = 1;
            dbgtx("sd task ready\r\n");
            ESP_LOGI(TAG, "task ready");
            sd_notify_pending();
            sys.curr_task->step = c_step_ready;
        break;

        case c_step_ready:
            if(task_sd_set.setup_req_seq_bk != task_sd_set.setup_req_seq)
            {
                task_sd_set.setup_req_seq_bk = task_sd_set.setup_req_seq;
                sys.curr_task->step = c_step_mount;
                return;
            }
            if(p_task_arg->mount_ok == 0)
            {
                if(task_sd_set.del_req_seq_bk != task_sd_set.del_req_seq)
                {
                    task_sd_set.del_req_seq_bk = task_sd_set.del_req_seq;
                    s_q.hold = 0;
                    sd_notify_tcp("cache:empty");
                }
                if(task_sd_set.get_req_seq_bk != task_sd_set.get_req_seq)
                {
                    task_sd_set.get_req_seq_bk = task_sd_set.get_req_seq;
                    sd_notify_tcp("cache:empty");
                }
                return;
            }

            if(s_q.cnt > 0)
            {
                n = sd_q_pop(line, sizeof(line));
                if(n == 0)
                    return;
                if(cache_append(line, (int)n) != c_ret_ok)
                    dbgtx("sd append fail\r\n");
                sd_notify_pending();
                return;
            }

            if(task_sd_set.del_req_seq_bk != task_sd_set.del_req_seq)
            {
                task_sd_set.del_req_seq_bk = task_sd_set.del_req_seq;
                if(s_q.hold)
                {
                    cache_delete_first();
                    s_q.hold = 0;
                    s_file_len = 0;
                }
                sd_notify_after_del();
                return;
            }

            if(task_sd_set.get_req_seq_bk != task_sd_set.get_req_seq)
            {
                task_sd_set.get_req_seq_bk = task_sd_set.get_req_seq;
                if(s_q.hold && (s_file_len > 0))
                {
                    sd_notify_ready();
                    return;
                }
                fname[0] = 0;
                if(cache_get_first(s_file, sizeof(s_file),
                                   fname, sizeof(fname)) != c_ret_ok)
                {
                    s_file_len = 0;
                    s_q.hold = 0;
                    s_q.pend_ntf = 0;
                    sd_notify_tcp("cache:empty");
                    return;
                }
                s_file_len = (unsigned int)strlen(s_file);
                s_q.hold = 1;
                dbgtx("sd get %s len=%u\r\n", fname, s_file_len);
                ESP_LOGI(TAG, "get %s len=%u", fname, s_file_len);
                sd_notify_ready();
                return;
            }
        break;

        default:
            sys.curr_task->step = c_step_idle;
        break;
    }
}

static unsigned int sd_q_push(const char *line, unsigned int len)
{
    if((line == NULL) || (len == 0) || (len >= c_sd_line_max))
        return c_ret_nk;
    if(s_q.cnt >= c_sd_q_max)
        return c_ret_nk;
    memcpy(s_q.line[s_q.tail], line, len);
    s_q.line[s_q.tail][len] = 0;
    s_q.tail = (s_q.tail + 1) % c_sd_q_max;
    s_q.cnt++;
    return c_ret_ok;
}

static unsigned int sd_q_pop(char *out, unsigned int out_n)
{
    unsigned int n;

    if((s_q.cnt == 0) || (out == NULL) || (out_n == 0))
        return 0;
    n = (unsigned int)strlen(s_q.line[s_q.head]);
    if(n >= out_n)
        n = out_n - 1;
    memcpy(out, s_q.line[s_q.head], n);
    out[n] = 0;
    s_q.head = (s_q.head + 1) % c_sd_q_max;
    s_q.cnt--;
    return n;
}

static void sd_notify_tcp(const char *dat)
{
    if(dat == NULL)
        return;
    msg_send("task_tcp_msg", (char *)dat);
}

static void sd_notify_ready(void)
{
    unsigned int n = s_file_len;

    if(n >= c_sd_file_max)
        n = c_sd_file_max - 1;
    memcpy(s_ready_msg, "cache:ready:", 12);
    memcpy(s_ready_msg + 12, s_file, n);
    s_ready_msg[12 + n] = 0;
    msg_send("task_tcp_msg", s_ready_msg);
}

static void sd_notify_pending(void)
{
    if(s_q.pend_ntf)
        return;
    if(cache_pending() != c_ret_ok)
        return;
    s_q.pend_ntf = 1;
    sd_notify_tcp("cache:pending");
}

static void sd_notify_after_del(void)
{
    if(cache_pending() == c_ret_ok)
    {
        s_q.pend_ntf = 1;
        sd_notify_tcp("cache:pending");
    }
    else
    {
        s_q.pend_ntf = 0;
        sd_notify_tcp("cache:empty");
    }
}

void sd_cs_idle(void)
{
    /* 硬件 SDSPI 管理 CS, 不再 GPIO 抢脚 */
}

const char *sd_fat_drv(void)
{
    if(s_vol.ok == 0)
        return NULL;
    return s_vol.drv;
}

unsigned int sd_mount_fail(void)
{
    return s_sd_fail;
}

/* 对齐 25_sd spi_sd.c: 总线已由 spi_init 挂好, 这里只 mount */
static esp_err_t sd_mount(void)
{
    esp_err_t ret;
    unsigned int mb;
    esp_vfs_fat_sdmmc_mount_config_t mount_config = {
        .format_if_mount_failed = true,     /* 空卡无文件系统时格式化, 便于实验室 TF */
        .max_files = 6,
        .allocation_unit_size = 4 * 1024
    };
    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    sdspi_device_config_t slot_config = SDSPI_DEVICE_CONFIG_DEFAULT();

    if(s_vol.ok)
        return ESP_OK;

    host.slot = SPI_HOST;
    slot_config.host_id = SPI_HOST;
    slot_config.gpio_cs = SD_NUM_CS;
    slot_config.gpio_cd = GPIO_NUM_NC;
    slot_config.gpio_wp = GPIO_NUM_NC;
    slot_config.gpio_int = GPIO_NUM_NC;

    ESP_LOGI(TAG, "sdspi mount begin");
    ret = esp_vfs_fat_sdspi_mount(SD_MOUNT_POINT, &host, &slot_config,
                                  &mount_config, &s_vol.card);
    if(ret != ESP_OK)
    {
        dbgtx("sd sdspi mount err %s\r\n", esp_err_to_name(ret));
        ESP_LOGE(TAG, "sdspi mount err %s", esp_err_to_name(ret));
        s_vol.card = NULL;
        s_sd_fail = 1;
        return ESP_FAIL;
    }

    snprintf(s_vol.drv, sizeof(s_vol.drv), "0:");
    s_vol.ok = 1;
    s_sd_fail = 0;
    mb = 0;
    if((s_vol.card != NULL) && (s_vol.card->csd.sector_size != 0))
        mb = (unsigned int)(((uint64_t)s_vol.card->csd.capacity *
                             s_vol.card->csd.sector_size) / (1024 * 1024));
    dbgtx("sd fat32 mount ok drv=%s vfs=%s %uMB\r\n", s_vol.drv, SD_MOUNT_POINT, mb);
    ESP_LOGI(TAG, "fat32 mount ok drv=%s %uMB", s_vol.drv, mb);
    return ESP_OK;
}

static esp_err_t sd_unmount(void)
{
    if(s_vol.ok == 0)
        return ESP_OK;
    esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_vol.card); /* 只摘 SD 设备, 不 spi_bus_free */
    s_vol.card = NULL;
    s_vol.ok = 0;
    s_vol.drv[0] = 0;
    dbgtx("sd unmount ok\r\n");
    return ESP_OK;
}

void task_sd_init(void)
{
    msg_add("task_sd_msg", task_sd_msg_parse);
    dbgtx("sd_task_init ok\r\n");
}
INIT_REG(task_sd_init, 5);      /* 优先级5: 晚于spi_init(2)/xl9555(4), 早于task_lcd_init(6) */

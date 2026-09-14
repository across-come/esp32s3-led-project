#include "user_ext.h"
#include "esp_timer.h"
#include "esp_rom_sys.h"

struct st_sys sys = {0};
unsigned int int_cnt = 0;

unsigned char mmbuf[c_mem_buf_size] = {0};
unsigned char mmctl[c_mem_blk_num]  = {0};

/* ============ 初始化注册表 ============ */
struct st_init_item
{
    unsigned int    prio;
    init_cb         func;
};
static struct st_init_item init_tab[c_init_max] = {0};
static unsigned int init_cnt = 0;

unsigned int init_register(unsigned int prio, init_cb func)
{
    if(init_cnt >= c_init_max)
        return c_ret_nk;
    init_tab[init_cnt].prio = prio;
    init_tab[init_cnt].func = func;
    init_cnt++;
    return c_ret_ok;
}

void user_init(void)
{
    /* 按优先级排序, 优先级小的先执行 */
    for(unsigned int i = 0; i < init_cnt; i++)
    {
        for(unsigned int j = i + 1; j < init_cnt; j++)
        {
            if(init_tab[j].prio < init_tab[i].prio)
            {
                struct st_init_item tmp = init_tab[i];
                init_tab[i] = init_tab[j];
                init_tab[j] = tmp;
            }
        }
    }
    for(unsigned int i = 0; i < init_cnt; i++)
    {
        init_tab[i].func();
    }
}

/* ============ 高精度时间(ESP32用esp_timer替代DWT, 单位us) ============ */
void dwt_init(void)
{
    /* ESP32无DWT, esp_timer由系统自动初始化 */
}
INIT_REG(dwt_init, 1);

unsigned int dwt_get(void)
{
	return (unsigned int)esp_timer_get_time();
}

unsigned int dwt_get_us(void)
{
	return (unsigned int)esp_timer_get_time();
}

unsigned int dwt_cmp(unsigned int tmr, unsigned int tmo)
{
	return ((dwt_get() - tmr) >= tmo) ? c_ret_ok : c_ret_nk;
}

void dly_ns(unsigned int us)
{
	esp_rom_delay_us(us);
}

void dly_ms(unsigned int ms)
{
	for(unsigned int i = 0; i < ms; i++)
		dly_ns(1000);
}

/* ============ tick(ms) ============ */
unsigned int tick_get(void)
{
    return (unsigned int)(xTaskGetTickCount() * portTICK_PERIOD_MS);
}

unsigned int tick_cmp(unsigned int tmr, unsigned int tmo)
{
    return ((tick_get() - tmr) >= tmo) ? c_ret_ok : c_ret_nk;
}

/* ============ 内存池 ============ */
void *bsp_alloc(unsigned int size)
{
    unsigned int num, cnt, i, j;

    if(size == 0)
        return NULL;

    num = (size - 1) / c_mem_blk_size + 1;

    int_dis();
    for(cnt = i = 0; i < c_mem_blk_num; i++)
    {
        if(mmctl[i] != 0)
            cnt = 0;
        else
        {
            cnt++;
            if(cnt >= num)
            {
                for(j = i + 1 - cnt; j < i; j++)
                    mmctl[j] = 0xff;
                mmctl[j] = 0x7f;
                memset(mmbuf + (i + 1 - cnt) * c_mem_blk_size, 0 , num * c_mem_blk_size);
                int_en();
                return mmbuf + (i + 1 - cnt) * c_mem_blk_size;
            }
        }
    }
    int_en();
    return NULL;
}

unsigned int bsp_free(void *px)
{
    unsigned int i, j;

    if(((unsigned char*)px < mmbuf) || ((unsigned char*)px >= mmbuf + c_mem_buf_size) || ((((unsigned char*)px - mmbuf) % c_mem_blk_size) != 0))
        return c_ret_nk;

    i = ((unsigned int)px - (unsigned int)mmbuf) / c_mem_blk_size;

    int_dis();
    if(mmctl[i] != 0xff && mmctl[i] != 0x7f)
    {
        int_en();
        return c_ret_nk;
    }
    for(; i < c_mem_blk_num; i++)
    {
        j = mmctl[i];
        mmctl[i] = 0;
        if(j == 0x7f)
            break;
    }
    int_en();
    return c_ret_ok;
}

static unsigned int bsp_get_alloc_size(void *px)
{
    unsigned int i, cnt = 0;

    if(((unsigned char*)px < mmbuf) || ((unsigned char*)px >= (mmbuf + c_mem_buf_size)) || ((((unsigned char*)px - mmbuf) % c_mem_blk_size) != 0))
        return 0;

    int_dis();
    for(i = ((unsigned int)px - (unsigned int)mmbuf) / c_mem_blk_size; i < c_mem_blk_num; i++)
    {
        cnt++;
        if(mmctl[i] <= 0x7f)
            break;
    }
    int_en();
    return cnt * c_mem_blk_size;
}

void *bsp_realloc(void *ptr, unsigned int size)
{
    unsigned char *px;

    if((px = (unsigned char*)bsp_alloc(size)) != NULL)
    {
        if(ptr != NULL)
        {
            memcpy(px, ptr, bsp_get_alloc_size(ptr));
            bsp_free(ptr);
        }
        return px;
    }
    return NULL;
}

/* ============ 字符串转数字 ============ */
unsigned int str2num(char *buf, unsigned int *wx, double *dx)
{
	unsigned int 	i, base;
	char			c;

	if((buf == NULL)||((wx == NULL)&&(dx == NULL)))
		return c_ret_nk;

	for(base = 10, i = 0; buf[i] != 0; i++)
	{
		c = buf[i];

		if((c >= '0')&&(c <= '9'))
		{
			continue;
		}
		else if((c == 'x')||(c == 'X'))
		{
			if((i == 1)&&(buf[0] == '0'))
				base = 16;
			else
				return c_ret_nk;
		}
		else if(((c >= 'a')&&(c <= 'f'))||((c >= 'A')&&(c <= 'F')))
		{
			if(base != 16)
				return c_ret_nk;
		}
		else if(c == '.')
		{
			if((i == 0)||(buf[i+1] == 0)||(base != 10))
				return c_ret_nk;
			base = 15;
		}
		else if(c == '-')
		{
			if((i != 0)||(buf[i+1] == 0))
				return c_ret_nk;
		}
		else
		{
			return c_ret_nk;
		}
	}
	if(base == 15)
	{
		if(dx != NULL)
		{
			*dx = strtod(buf, NULL);
			if(wx != NULL)
				*wx = (unsigned int)(*dx + 0.25);
		}
		else
			*wx = (unsigned int)strtod(buf, NULL);
	}
	else
	{
		if(wx != NULL)
		{
			*wx = strtoul(buf, NULL, base);
			if(dx != NULL)
				*dx = (double)*wx;
		}
		else
			*dx = (double)strtoul(buf, NULL, base);
	}
	return c_ret_ok;
}

/* ============ KV 无名队列 ============ */
struct st_kv *kv_add(unsigned int len)
{
    unsigned int idx;

    while(1)
    {
        for(idx = 0; idx < sys.kv_cnt; idx++)
        {
            if(sys.kv[idx].dat == NULL)
            {
                if((len == 0) || ((sys.kv[idx].dat = bsp_alloc(len)) != NULL))
                {
                    if(len != 0)
                        memset(sys.kv[idx].dat, 0, len);
                    return &sys.kv[idx];
                }
                return NULL;
            }
        }
        sys.kv = (struct st_kv*)bsp_realloc(sys.kv, sizeof(struct st_kv) * (sys.kv_cnt + 1));
        if(sys.kv == NULL)
            return NULL;
        sys.kv_cnt++;
    }
}

unsigned int kv_del(struct st_kv *p_kv)
{
    if(p_kv == NULL)
        return c_ret_nk;

    bsp_free(p_kv->dat);
    p_kv->dat = NULL;
    return c_ret_ok;
}

/* ============ FLAG ============ */
struct st_flag *flag_add(char *name, flag_func func, unsigned int arg_len)
{
    unsigned int idx;

    while(1)
    {
        for(idx = 0; idx < sys.flag_cnt; idx++)
        {
            if(sys.flag[idx].name == NULL)
            {
                if((sys.flag[idx].name = (char*)bsp_alloc(strlen(name) + 1)) != NULL)
                {
                    if((arg_len == 0) || ((sys.flag[idx].dat = bsp_alloc(arg_len)) != NULL))
                    {
                        strcpy(sys.flag[idx].name, name);
                        if(arg_len != 0)
                            memset(sys.flag[idx].dat, 0, arg_len);
                        else
                            sys.flag[idx].dat = NULL;
                        sys.flag[idx].func = func;
                        sys.flag[idx].seq_bk = sys.flag[idx].seq = 0;
                        return &sys.flag[idx];
                    }
                    else
                    {
                        bsp_free(sys.flag[idx].name);
                        sys.flag[idx].name = NULL;
                        return NULL;
                    }
                }
            }
        }
        {
            if((sys.flag = (struct st_flag*)bsp_realloc(sys.flag, sizeof(struct st_flag) * (sys.flag_cnt + 1))) == NULL)
                return NULL;
            sys.flag_cnt++;
        }
    }
}

void flag_set(struct st_flag *p_item)
{
    if(p_item != NULL)
    {
        p_item->seq++;
        sys.flag_seq++;
    }
}

void flag_proc(void)
{
     unsigned int idx;

     if(sys.flag_seq == sys.flag_seq_bk) return;
     sys.flag_seq_bk++;
     for(idx = 0; idx < sys.flag_cnt; idx++)
     {
        if(sys.flag[idx].name == NULL) continue;
        if(sys.flag[idx].seq_bk != sys.flag[idx].seq)
        {
            sys.flag[idx].seq_bk = sys.flag[idx].seq;
            if(sys.flag[idx].func != NULL)
                sys.flag[idx].func(sys.flag[idx].dat);
        }
     }
}

/* ============ TASK ============ */
void task_del(const char *name)
{
    unsigned int idx;

    for(idx = 0; idx < sys.task_cnt; idx++)
    {
        if((sys.task[idx].name != NULL) && (strcmp(sys.task[idx].name, name) == 0))
        {
            bsp_free(sys.task[idx].name);
            bsp_free(sys.task[idx].dat);
            sys.task[idx].name = NULL;
            sys.task[idx].dat = NULL;
            return;
        }
    }
}

struct st_task *task_add(const char *name, task_func func, unsigned int auto_quit, unsigned int arg_len)
{
    unsigned int idx, name_len;

    task_del(name);
    name_len = strlen(name) + 1;

    while(1)
    {
        for(idx = 0; idx < sys.task_cnt; idx++)
        {
            if(sys.task[idx].name == NULL)
            {
                if((sys.task[idx].name = (char *)bsp_alloc(name_len)) != NULL)
                {
                    if((arg_len == 0) || ((sys.task[idx].dat = bsp_alloc(arg_len)) != NULL))
                    {
                        memcpy(sys.task[idx].name, name, name_len);
                        if(arg_len != 0)
                            memset(sys.task[idx].dat, 0, arg_len);
                        else
                            sys.task[idx].dat = NULL;
                        sys.task[idx].func = func;
                        sys.task[idx].auto_quit = auto_quit;
                        sys.task[idx].quit = 0;
                        sys.task[idx].done = c_done_wt;
                        sys.task[idx].first_run = 0;
                        sys.task[idx].step = 0;
                        sys.task[idx].tmr  = tick_get();
                        return &sys.task[idx];
                    }
                    else
                    {
                        bsp_free(sys.task[idx].name);
                        sys.task[idx].name = NULL;
                        return NULL;
                    }
                }
                return NULL;
            }
        }
        {
            unsigned int offset = 0;

            if(sys.curr_task != NULL)
                offset = sys.curr_task - sys.task;
            if((sys.task = (struct st_task*)bsp_realloc(sys.task, sizeof(struct st_task) * (sys.task_cnt + 1))) != NULL)
                sys.curr_task = sys.task + offset;

            sys.task_cnt++;
        }
    }
}

struct st_task *task_get(const char *name)
{
    unsigned int idx;

    for(idx = 0; idx < sys.task_cnt; idx++)
    {
        if((sys.task[idx].name != NULL) && (strcmp(sys.task[idx].name, name) == 0))
        {
            return &sys.task[idx];
        }
    }
    return NULL;
}

void task_proc(void)
{
    unsigned int idx;
    flag_proc();
    for(idx = 0; idx < sys.task_cnt; idx++)
    {
        sys.curr_task = &sys.task[idx];
        if(sys.curr_task->name == NULL) continue;
        if(sys.curr_task->wait == 0)
        {
            if(sys.curr_task->func)
            {
                if(sys.curr_task->done == c_done_wt)
                {
                    if(sys.curr_task->first_run == 0)
                    {
                        sys.curr_task->first_run = 1;
                        dbgtx("\r\n****** task:%s first_run tmr=%d ******\r\n", sys.curr_task->name, tick_get());
                    }
                    sys.curr_task->func();
                }
                else if((sys.curr_task->quit) || (sys.curr_task->auto_quit))
                {
                    dbgtx("\r\n****** task:%s run_quit tmr=%d ******\r\n", sys.curr_task->name, tick_get());
                    bsp_free(sys.curr_task->name);
                    bsp_free(sys.curr_task->dat);
                    sys.curr_task->name = NULL;
                    sys.curr_task->dat = NULL;
                }
            }
        }
        sys.curr_task = NULL;
    }
}

/* ============ 任务等待/恢复 (kv改为无名队列后, 用固定表存储) ============ */
#define c_task_wait_num    8

struct st_wait_item
{
    char            name[32];
    struct st_task  *task;
};
static struct st_wait_item wait_tab[c_task_wait_num] = {0};

void __task_curr_wait(const char *file_name, const char *func_name)
{
    char buf[32] = {0};
    unsigned int idx;

    snprintf(buf, sizeof(buf), "%s_%s", file_name, func_name);
    sys.curr_task->wait = 1;

    for(idx = 0; idx < c_task_wait_num; idx++)
    {
        if(wait_tab[idx].name[0] == 0)
        {
            strncpy(wait_tab[idx].name, buf, sizeof(wait_tab[idx].name) - 1);
            wait_tab[idx].task = sys.curr_task;
            return;
        }
    }
}

void __task_curr_run(const char *file_name, const char *func_name)
{
    char buf[32] = {0};
    unsigned int idx;

    snprintf(buf, sizeof(buf), "%s_%s", file_name, func_name);
    for(idx = 0; idx < c_task_wait_num; idx++)
    {
        if(strcmp(wait_tab[idx].name, buf) == 0)
        {
            wait_tab[idx].task->wait = 0;
            wait_tab[idx].name[0] = 0;
            return;
        }
    }
}
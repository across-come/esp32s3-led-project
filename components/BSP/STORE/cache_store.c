#include "user_ext.h"
#include "cache_store.h"
#include "task_sd.h"
#include "esp_log.h"
#include "ff.h"

/* 断网缓存索引: 启动扫一次目录, 之后只改内存, 不再每 tick 扫盘 */
struct st_cache
{
    char            dir[16];        /* "0:/CACHE" */
    unsigned int    ready;          /* 1=init 成功, 可 append/get/del */
    unsigned int    cur_seq;        /* 正在写的文件序号, 对应 Cxxxxx.DAT */
    unsigned int    cur_count;      /* 当前文件已写条数, 满 CACHE_ITEM_MAX 则换新文件 */
    unsigned int    old_seq;        /* 最早未上传文件序号, get/del 的队头 */
    unsigned int    nfile;          /* 盘上现存缓存文件数 */
};

static struct st_cache s_cache = {0};

static void cache_path(char *out, unsigned int out_n, unsigned int seq)
{
    snprintf(out, out_n, "%s/C%05u.DAT", s_cache.dir, seq);
}

static FF_DIR s_dir;
static FILINFO s_fi;
static FIL s_fil;

/* 仅 init 时扫一次, 建立 oldest / next_seq / nfile */
static int cache_scan_init(unsigned int *p_min, unsigned int *p_max, unsigned int *p_n)
{
    FRESULT fr;
    unsigned int n = 0;
    unsigned int mn = 0xFFFFFFFFu;
    unsigned int mx = 0;
    unsigned int seq;
    unsigned int dummy;

    if(p_min == NULL) p_min = &dummy;
    if(p_max == NULL) p_max = &dummy;
    if(p_n == NULL) p_n = &dummy;

    fr = f_opendir(&s_dir, s_cache.dir);
    if(fr != FR_OK)
        return c_ret_nk;

    for(;;)
    {
        fr = f_readdir(&s_dir, &s_fi);
        if((fr != FR_OK) || (s_fi.fname[0] == 0))
            break;
        if(s_fi.fattrib & AM_DIR)
            continue;
        if(sscanf(s_fi.fname, "C%5u.DAT", &seq) != 1)
            continue;
        n++;
        if(seq < mn) mn = seq;
        if(seq > mx) mx = seq;
    }
    f_closedir(&s_dir);

    *p_n = n;
    if(n == 0)
    {
        *p_min = 0;
        *p_max = 0;
        return c_ret_ok;
    }
    *p_min = mn;
    *p_max = mx;
    return c_ret_ok;
}

int cache_init(void)
{
    const char *drv;
    FRESULT fr;
    unsigned int mn = 0;
    unsigned int mx = 0;
    unsigned int n = 0;

    s_cache.ready = 0;
    s_cache.nfile = 0;
    s_cache.cur_count = 0;
    s_cache.cur_seq = 0;
    s_cache.old_seq = 0;
    s_cache.dir[0] = 0;

    drv = sd_fat_drv();
    if(drv == NULL)
    {
        dbgtx("cache init: sd not mounted\r\n");
        return c_ret_nk;
    }
    snprintf(s_cache.dir, sizeof(s_cache.dir), "%s/CACHE", drv);

    fr = f_mkdir(s_cache.dir);
    if((fr != FR_OK) && (fr != FR_EXIST))
    {
        dbgtx("cache mkdir err fr=%d %s\r\n", (int)fr, s_cache.dir);
        return c_ret_nk;
    }

    if(cache_scan_init(&mn, &mx, &n) != c_ret_ok)
    {
        dbgtx("cache scan err %s\r\n", s_cache.dir);
        return c_ret_nk;
    }

    s_cache.nfile = n;
    if(n == 0)
    {
        s_cache.old_seq = 0;
        s_cache.cur_seq = 0;
    }
    else
    {
        s_cache.old_seq = mn;
        s_cache.cur_seq = mx + 1;       /* 不续写旧文件, 避免条数计数错 */
        s_cache.cur_count = 0;
    }
    s_cache.ready = 1;
    dbgtx("cache init ok dir=%s files=%u old=%u next=%u\r\n",
          s_cache.dir, s_cache.nfile, s_cache.old_seq, s_cache.cur_seq);
    ESP_LOGI("sd", "cache init ok files=%u old=%u next=%u",
             s_cache.nfile, s_cache.old_seq, s_cache.cur_seq);
    return c_ret_ok;
}

int cache_append(const char *data, int len)
{
    FRESULT fr;
    char fname[CACHE_PATH_MAX];
    unsigned int bw;
    unsigned int new_file;

    if((s_cache.ready == 0) || (data == NULL) || (len <= 0) || (len > 512))
        return c_ret_nk;

    if(s_cache.cur_count >= CACHE_ITEM_MAX)
    {
        s_cache.cur_seq++;
        s_cache.cur_count = 0;
    }

    cache_path(fname, sizeof(fname), s_cache.cur_seq);
    new_file = (s_cache.cur_count == 0) ? 1 : 0;
    fr = f_open(&s_fil, fname, FA_WRITE | FA_OPEN_ALWAYS);
    if(fr != FR_OK)
    {
        dbgtx("cache open err fr=%d %s\r\n", (int)fr, fname);
        return c_ret_nk;
    }
    f_lseek(&s_fil, f_size(&s_fil));
    fr = f_write(&s_fil, data, (UINT)len, &bw);
    if((fr != FR_OK) || (bw != (UINT)len))
    {
        dbgtx("cache write err fr=%d\r\n", (int)fr);
        f_close(&s_fil);
        return c_ret_nk;
    }
    f_sync(&s_fil);
    f_close(&s_fil);

    if(new_file)
    {
        if(s_cache.nfile == 0)
            s_cache.old_seq = s_cache.cur_seq;
        s_cache.nfile++;
        ESP_LOGI("sd", "append new file C%05u.DAT nfile=%u", s_cache.cur_seq, s_cache.nfile);
    }
    s_cache.cur_count++;
    return c_ret_ok;
}

int cache_pending(void)
{
    if((s_cache.ready == 0) || (s_cache.nfile == 0))
        return c_ret_nk;
    return c_ret_ok;
}

unsigned int cache_nfile(void)
{
    return s_cache.nfile;
}

int cache_get_first(char *buf, int buflen, char *fname, int fname_len)
{
    FRESULT fr;
    char tmp[CACHE_PATH_MAX];
    UINT br;
    unsigned int skip;

    if((s_cache.ready == 0) || (buf == NULL) || (fname == NULL) || (buflen < 2))
        return c_ret_nk;
    if(s_cache.nfile == 0)
        return c_ret_nk;

    /* 正在写的文件要先封口, 避免边发边 append 同一文件 */
    if(s_cache.old_seq == s_cache.cur_seq)
    {
        s_cache.cur_seq++;
        s_cache.cur_count = 0;
    }

    for(skip = 0; skip < 64; skip++)
    {
        if(s_cache.nfile == 0)
            return c_ret_nk;
        cache_path(tmp, sizeof(tmp), s_cache.old_seq);
        fr = f_open(&s_fil, tmp, FA_READ);
        if(fr != FR_OK)
        {
            s_cache.old_seq++;          /* 序号空洞: 只推进, nfile仍是实文件数 */
            continue;
        }
        if(f_size(&s_fil) >= (FSIZE_t)buflen)
        {
            f_close(&s_fil);
            dbgtx("cache file too big, drop %s\r\n", tmp);
            f_unlink(tmp);
            s_cache.old_seq++;
            if(s_cache.nfile > 0)
                s_cache.nfile--;
            return c_ret_nk;
        }
        fr = f_read(&s_fil, (BYTE *)buf, (UINT)buflen - 1, &br);
        f_close(&s_fil);
        if((fr != FR_OK) || (br == 0))
        {
            f_unlink(tmp);
            s_cache.old_seq++;
            if(s_cache.nfile > 0)
                s_cache.nfile--;
            return c_ret_nk;
        }
        buf[br] = 0;
        if(fname_len > 0)
            snprintf(fname, (unsigned int)fname_len, "%s", tmp);
        return c_ret_ok;
    }
    return c_ret_nk;
}

int cache_delete_first(void)
{
    char tmp[CACHE_PATH_MAX];

    if((s_cache.ready == 0) || (s_cache.nfile == 0))
        return c_ret_nk;

    cache_path(tmp, sizeof(tmp), s_cache.old_seq);
    if(f_unlink(tmp) != FR_OK)
    {
        /* 文件已不在也推进序号, 避免死循环 */
        dbgtx("cache unlink warn %s\r\n", tmp);
    }
    else
        dbgtx("cache del %s\r\n", tmp);

    s_cache.old_seq++;
    if(s_cache.nfile > 0)
        s_cache.nfile--;
    ESP_LOGI("sd", "del %s left=%u", tmp, s_cache.nfile);
    return c_ret_ok;
}

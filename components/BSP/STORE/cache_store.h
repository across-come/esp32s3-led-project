#ifndef __CACHE_STORE_H
#define __CACHE_STORE_H

/* ============ 断网缓存 (仅 task_sd 内部使用, TCP请走 msg_send) ============
 * 文件: Cxxxxx.DAT (8.3), 每文件最多 CACHE_ITEM_MAX 条
 * 运行时用内存记 oldest/count, 不在 keep 里每 tick 扫目录
 */

#define CACHE_ITEM_MAX      32
#define CACHE_PATH_MAX      32          /* "0:/CACHE/C00000.DAT" + NUL */
#define CACHE_FILE_MAX      2100        /* 32条 x 约64字节, 整文件一次读出发送 */

int cache_init(void);
int cache_append(const char *data, int len);
int cache_pending(void);                /* 有积压文件: c_ret_ok */
unsigned int cache_nfile(void);
int cache_get_first(char *buf, int buflen, char *fname, int fname_len);
int cache_delete_first(void);

#endif

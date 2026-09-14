#include "user_ext.h"
#include "task_wifi.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_smartconfig.h"
#include "nvs_flash.h"
#include "freertos/queue.h"
#include "task_lvgl.h"

struct st_task_wifi_set task_wifi_set = {0};
static struct st_wifi_view s_wifi_view = {0};

const struct st_wifi_view *wifi_view_get(void)
{
    return &s_wifi_view;
}


#define c_wifi_ev_scan_done     1
#define c_wifi_ev_sta_disc      2
#define c_wifi_ev_got_ip        3
#define c_wifi_ev_sc_scan_done  4
#define c_wifi_ev_sc_found_ch   5
#define c_wifi_ev_sc_got_ssid   6
#define c_wifi_ev_sc_ack        7
#define c_wifi_ev_q_len         8

struct st_wifi_ev
{
    unsigned int                      id;
    unsigned int                      scan_status;
    char                              sta_ip[16];
    smartconfig_event_got_ssid_pswd_t sc;
    unsigned char                     rvd[33];
    unsigned int                      rvd_ok;
};

static QueueHandle_t s_wifi_ev_q = NULL;

static void wifi_lcd_show_all(struct st_task_wifi_run *p_task_arg);
static void wifi_lcd_show_sta(struct st_task_wifi_run *p_task_arg, unsigned int flag);
static void wifi_lcd_show_sc(struct st_task_wifi_run *p_task_arg, unsigned int flag);

/* ======================================================================
 * WiFi任务 串口指令解析 (在最上面)
 * ======================================================================
 * exe=dbg_msg_send(msg:task_wifi_msg,dat:help)
 * exe=dbg_msg_send(msg:task_wifi_msg,dat:setup)
 * exe=dbg_msg_send(msg:task_wifi_msg,dat:stop)
 * exe=dbg_msg_send(msg:task_wifi_msg,dat:wifi_scan)
 * exe=dbg_msg_send(msg:task_wifi_msg,dat:wifi_sta:ssid=123,pwd=aa1234567)
 * exe=dbg_msg_send(msg:task_wifi_msg,dat:wifi_smartconfig)
 */
unsigned int task_wifi_msg_parse(char *buf)
{
    char ssid[c_wifi_ssid_max + 1];
    char pwd[c_wifi_pwd_max + 1];

    if(buf == NULL) return c_ret_nk;

    if(strcmp(buf, "help") == 0)
    {
        dbgtx("setup\r\n");
        dbgtx("stop\r\n");
        dbgtx("wifi_scan\r\n");
        dbgtx("wifi_sta:ssid=xxx,pwd=yyy\r\n");
        dbgtx("wifi_disc\r\n");
        dbgtx("wifi_smartconfig\r\n");
        return c_ret_ok;
    }
    else if(strcmp(buf, "wifi_scan") == 0)
    {
        task_wifi_set.set.cmd = c_wifi_cmd_scan;
        return c_ret_ok;
    }
    else if(strcmp(buf, "wifi_smartconfig") == 0)
    {
        task_wifi_set.set.cmd = c_wifi_cmd_sc;
        return c_ret_ok;
    }
    else if(sscanf(buf, "wifi_sta:ssid=%32[^,],pwd=%64[^\n]", ssid, pwd) == 2)
    {
        memset(task_wifi_set.set.ssid, 0, sizeof(task_wifi_set.set.ssid));
        memset(task_wifi_set.set.pwd, 0, sizeof(task_wifi_set.set.pwd));
        strncpy(task_wifi_set.set.ssid, ssid, c_wifi_ssid_max);
        strncpy(task_wifi_set.set.pwd, pwd, c_wifi_pwd_max);
        task_wifi_set.set.cmd = c_wifi_cmd_sta;
        dbgtx("wifi sta ssid=%s pwd_len=%u\r\n", task_wifi_set.set.ssid, (unsigned int)strlen(task_wifi_set.set.pwd));
        return c_ret_ok;
    }
    else if(strcmp(buf, "wifi_disc") == 0)
    {
        esp_wifi_disconnect();
        s_wifi_view.got_ip = 0;
        s_wifi_view.connecting = 0;
        s_wifi_view.fail = 0;
        s_wifi_view.ip[0] = 0;
        dbgtx("wifi disc\r\n");
        return c_ret_ok;
    }
    else if(strcmp(buf, "setup") == 0)
    {
        if(task_get("task_wifi") != NULL)
        {
            dbgtx("task_wifi is running, cmd NK\r\n");
            return c_ret_ok;
        }
        task_wifi_set.setup_req_seq++;
        task_add("task_wifi", task_wifi_proc, c_auto_quit, sizeof(struct st_task_wifi_run));
        return c_ret_ok;
    }
    else if(strcmp(buf, "stop") == 0)
    {
        if(task_get("task_wifi") == NULL) return c_ret_nk;
        task_wifi_set.stop_req_seq++;
        return c_ret_ok;
    }
    return c_ret_nk;
}

/* ======================================================================
 * STA事件: 回调只往队列拷一份, 不碰 sys.task/dat, 不dbgtx
 * 由 task_wifi_proc -> wifi_ev_drain 在主循环消费
 * ====================================================================== */
static void wifi_sc_abort(struct st_task_wifi_run *p_run)
{
    if(p_run == NULL) return;
    if(p_run->sc_busy)
    {
        p_run->sc_busy = 0;
        esp_smartconfig_stop();
    }
}

static unsigned int wifi_sta_backoff_ms(unsigned int retry)
{
    unsigned int ms = c_wifi_sta_backoff_min_ms;
    unsigned int i;

    for(i = 1; i < retry; i++)
    {
        if(ms >= (c_wifi_sta_backoff_max_ms / 2))
            return c_wifi_sta_backoff_max_ms;
        ms *= 2;
    }
    return ms;
}

static void wifi_sta_arm_reconn(struct st_task_wifi_run *p_run)
{
    if(p_run == NULL) return;
    if(p_run->sta_retry >= c_wifi_sta_retry_max)
    {
        p_run->sta_busy = 0;
        p_run->sta_fail = 1;
        p_run->sta_reconn = 0;
        dbgtx("wifi sta retry max, retry=%u\r\n", p_run->sta_retry);
        return;
    }
    p_run->sta_retry++;
    p_run->sta_reconn = 1;
    p_run->sta_backoff_tmr = tick_get();
    p_run->sta_backoff_ms = wifi_sta_backoff_ms(p_run->sta_retry);
    dbgtx("wifi sta disc, retry=%u wait=%ums\r\n", p_run->sta_retry, p_run->sta_backoff_ms);
}

static unsigned int wifi_sta_do_connect(struct st_task_wifi_run *p_run, unsigned int disc_first)
{
    wifi_ap_record_t ap;

    if(p_run == NULL) return c_ret_nk;
    p_run->sta_reconn = 0;
    if(disc_first)
    {
        /* 已连上才disconnect; 未连时IDF可能不报DISC, ignore会把随后真正的失败吞掉 */
        if(esp_wifi_sta_get_ap_info(&ap) == ESP_OK)
        {
            p_run->sta_disc_ignore = 1;
            esp_wifi_disconnect();
        }
    }
    if(esp_wifi_connect() != ESP_OK)
    {
        p_run->sta_disc_ignore = 0;
        return c_ret_nk;
    }
    return c_ret_ok;
}

static void wifi_sta_reconn_poll(struct st_task_wifi_run *p_run)
{
    if((p_run == NULL) || (p_run->sta_reconn == 0)) return;
    if(tick_cmp(p_run->sta_backoff_tmr, p_run->sta_backoff_ms) != c_ret_ok) return;
    dbgtx("wifi sta reconnect retry=%u\r\n", p_run->sta_retry);
    if(wifi_sta_do_connect(p_run, 0) != c_ret_ok)
    {
        dbgtx("wifi sta reconnect err\r\n");
        wifi_sta_arm_reconn(p_run);
    }
}

static void wifi_ev_post(struct st_wifi_ev *p_ev)
{
    if((s_wifi_ev_q == NULL) || (p_ev == NULL)) return;
    xQueueSend(s_wifi_ev_q, p_ev, 0);
}

static void wifi_ev_drain(struct st_task_wifi_run *p_run)
{
    struct st_wifi_ev ev;
    wifi_config_t cfg;
    char hex[33];
    int i;

    if((p_run == NULL) || (s_wifi_ev_q == NULL)) return;

    while(xQueueReceive(s_wifi_ev_q, &ev, 0) == pdTRUE)
    {
        switch(ev.id)
        {
            case c_wifi_ev_scan_done:
                if(p_run->scan_busy == 0) break;
                p_run->scan_status = ev.scan_status;
                p_run->scan_done = 1;
            break;

            case c_wifi_ev_sta_disc:
                if(p_run->sta_busy == 0) break;
                if(p_run->sta_disc_ignore)
                {
                    p_run->sta_disc_ignore = 0;
                    break;
                }
                if(p_run->sta_reconn) break;
                wifi_sta_arm_reconn(p_run);
            break;

            case c_wifi_ev_got_ip:
                if(p_run->sta_busy == 0) break;
                memcpy(p_run->sta_ip, ev.sta_ip, sizeof(p_run->sta_ip));
                p_run->sta_retry = 0;
                p_run->sta_reconn = 0;
                p_run->sta_disc_ignore = 0;
                p_run->sta_busy = 0;
                p_run->sta_got_ip = 1;
            break;

            case c_wifi_ev_sc_scan_done:
                if(p_run->sc_busy == 0) break;
                p_run->sc_scan_done = 1;
                dbgtx("wifi sc scan done\r\n");
            break;

            case c_wifi_ev_sc_found_ch:
                if(p_run->sc_busy == 0) break;
                dbgtx("wifi sc found channel\r\n");
            break;

            case c_wifi_ev_sc_got_ssid:
                if(p_run->sc_busy == 0) break;
                if(ev.sc.ssid[0] == 0)
                {
                    p_run->sc_fail = 1;
                    dbgtx("wifi sc got ssid empty\r\n");
                    break;
                }
                memset(p_run->run.ssid, 0, sizeof(p_run->run.ssid));
                memset(p_run->run.pwd, 0, sizeof(p_run->run.pwd));
                memcpy(p_run->run.ssid, ev.sc.ssid, sizeof(ev.sc.ssid));
                memcpy(p_run->run.pwd, ev.sc.password, sizeof(ev.sc.password));
                p_run->sc_got_ssid = 1;

                memset(&cfg, 0, sizeof(cfg));
                memcpy(cfg.sta.ssid, ev.sc.ssid, sizeof(cfg.sta.ssid));
                memcpy(cfg.sta.password, ev.sc.password, sizeof(cfg.sta.password));
                cfg.sta.bssid_set = ev.sc.bssid_set;
                if(cfg.sta.bssid_set)
                    memcpy(cfg.sta.bssid, ev.sc.bssid, sizeof(cfg.sta.bssid));
                if(p_run->run.pwd[0] == 0)
                    cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
                else
                    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

                p_run->sta_got_ip = 0;
                p_run->sta_fail = 0;
                p_run->sta_retry = 0;
                p_run->sta_reconn = 0;
                p_run->sta_ip[0] = 0;
                p_run->sta_busy = 1;

                dbgtx("wifi sc got ssid=%s pwd_len=%u\r\n", p_run->run.ssid, (unsigned int)strlen(p_run->run.pwd));
                if(ev.rvd_ok)
                {
                    for(i = 0; i < 16; i++)
                        sprintf(&hex[i * 2], "%02x", ev.rvd[i]);
                    hex[32] = 0;
                    dbgtx("wifi sc rvd=%s\r\n", hex);
                }

                if(esp_wifi_set_config(WIFI_IF_STA, &cfg) != ESP_OK)
                {
                    p_run->sta_busy = 0;
                    p_run->sc_fail = 1;
                    dbgtx("wifi sc set_config err\r\n");
                    break;
                }
                if(wifi_sta_do_connect(p_run, 1) != c_ret_ok)
                {
                    p_run->sta_busy = 0;
                    p_run->sc_fail = 1;
                    dbgtx("wifi sc connect err\r\n");
                }
            break;

            case c_wifi_ev_sc_ack:
                if(p_run->sc_busy == 0) break;
                p_run->sc_ack_done = 1;
                dbgtx("wifi sc ack done\r\n");
            break;

            default:
            break;
        }
    }
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    struct st_wifi_ev ev;
    ip_event_got_ip_t *got_ip;
    wifi_event_sta_scan_done_t *done;
    smartconfig_event_got_ssid_pswd_t *sc;

    (void)arg;
    memset(&ev, 0, sizeof(ev));

    if((event_base == WIFI_EVENT) && (event_id == WIFI_EVENT_SCAN_DONE))
    {
        done = (wifi_event_sta_scan_done_t *)event_data;
        ev.id = c_wifi_ev_scan_done;
        ev.scan_status = (done != NULL) ? done->status : 1;
        wifi_ev_post(&ev);
    }
    else if((event_base == WIFI_EVENT) && (event_id == WIFI_EVENT_STA_DISCONNECTED))
    {
        ev.id = c_wifi_ev_sta_disc;
        wifi_ev_post(&ev);
    }
    else if((event_base == IP_EVENT) && (event_id == IP_EVENT_STA_GOT_IP))
    {
        got_ip = (ip_event_got_ip_t *)event_data;
        ev.id = c_wifi_ev_got_ip;
        if(got_ip != NULL)
            snprintf(ev.sta_ip, sizeof(ev.sta_ip), IPSTR, IP2STR(&got_ip->ip_info.ip));
        wifi_ev_post(&ev);
    }
    else if((event_base == SC_EVENT) && (event_id == SC_EVENT_SCAN_DONE))
    {
        ev.id = c_wifi_ev_sc_scan_done;
        wifi_ev_post(&ev);
    }
    else if((event_base == SC_EVENT) && (event_id == SC_EVENT_FOUND_CHANNEL))
    {
        ev.id = c_wifi_ev_sc_found_ch;
        wifi_ev_post(&ev);
    }
    else if((event_base == SC_EVENT) && (event_id == SC_EVENT_GOT_SSID_PSWD))
    {
        sc = (smartconfig_event_got_ssid_pswd_t *)event_data;
        ev.id = c_wifi_ev_sc_got_ssid;
        if(sc != NULL)
        {
            ev.sc = *sc;
            if(sc->type == SC_TYPE_ESPTOUCH_V2)
            {
                if(esp_smartconfig_get_rvd_data(ev.rvd, sizeof(ev.rvd)) == ESP_OK)
                    ev.rvd_ok = 1;
            }
        }
        wifi_ev_post(&ev);
    }
    else if((event_base == SC_EVENT) && (event_id == SC_EVENT_SEND_ACK_DONE))
    {
        ev.id = c_wifi_ev_sc_ack;
        wifi_ev_post(&ev);
    }
}

static void wifi_view_scan_done(struct st_task_wifi_run *p)
{
    unsigned int i, n;

    s_wifi_view.scanning = 0;
    if(p == NULL)
        return;
    n = p->ap_count;
    if(n > c_wifi_ap_max)
        n = c_wifi_ap_max;
    s_wifi_view.ap_count = n;
    for(i = 0; i < n; i++)
    {
        memset(s_wifi_view.ap_ssid[i], 0, sizeof(s_wifi_view.ap_ssid[i]));
        strncpy(s_wifi_view.ap_ssid[i], (char *)p->ap_info[i].ssid, c_wifi_ssid_max);
        s_wifi_view.ap_rssi[i] = p->ap_info[i].rssi;
    }
}

static void wifi_view_sta(struct st_task_wifi_run *p, unsigned int flag)
{
    if(p == NULL)
        return;
    memset(s_wifi_view.ssid, 0, sizeof(s_wifi_view.ssid));
    strncpy(s_wifi_view.ssid, p->run.ssid, c_wifi_ssid_max);
    s_wifi_view.connecting = (flag == 0);
    s_wifi_view.fail = (flag == 1);
    s_wifi_view.got_ip = (flag == 2);
    if(flag == 2)
    {
        memset(s_wifi_view.ip, 0, sizeof(s_wifi_view.ip));
        strncpy(s_wifi_view.ip, p->sta_ip, sizeof(s_wifi_view.ip) - 1);
    }
}

/* 清屏+标题+全部AP一次入队, 一次setup_done */
static void wifi_lcd_show_all(struct st_task_wifi_run *p_task_arg)
{
    char msg[96];
    char ssid_show[21];
    unsigned int i, n;

    if(p_task_arg == NULL) return;
    wifi_view_scan_done(p_task_arg);
    if(lvgl_ui_ready())
        return;

    msg_send("task_lcd_msg", "fill:x=0,y=0,w=320,h=240,color=0x0000");
    msg_send("task_lcd_msg", "str:x=10,y=5,size=16,color=0xffff,bk=0x0000,txt=WiFi SCAN:");

    n = p_task_arg->ap_count;
    if(n > c_wifi_ap_max) n = c_wifi_ap_max;
    for(i = 0; i < n; i++)
    {
        memset(ssid_show, 0, sizeof(ssid_show));
        strncpy(ssid_show, (char *)p_task_arg->ap_info[i].ssid, sizeof(ssid_show) - 1);
        sprintf(msg, "str:x=10,y=%d,size=12,color=0x07e0,bk=0x0000,txt=%s rssi:%d", 24 + i * 15, ssid_show, p_task_arg->ap_info[i].rssi);
        msg_send("task_lcd_msg", msg);
    }
    msg_send("task_lcd_msg", "setup_done");
}

/* flag: 0连接中 1失败 2成功, 对齐正点原子connet_display */
static void wifi_lcd_show_sta(struct st_task_wifi_run *p_task_arg, unsigned int flag)
{
    char msg[96];
    char show[21];

    if(p_task_arg == NULL) return;
    wifi_view_sta(p_task_arg, flag);
    if(lvgl_ui_ready())
        return;

    if(flag == 0)
        msg_send("task_lcd_msg", "fill:x=0,y=0,w=320,h=240,color=0x0000");
    else
        msg_send("task_lcd_msg", "fill:x=0,y=24,w=320,h=216,color=0x0000");
    msg_send("task_lcd_msg", "str:x=10,y=5,size=16,color=0xffff,bk=0x0000,txt=WiFi STA:");

    if(flag == 0)
    {
        msg_send("task_lcd_msg", "str:x=10,y=40,size=16,color=0x07ff,bk=0x0000,txt=wifi connecting......");
    }
    else if(flag == 1)
    {
        msg_send("task_lcd_msg", "str:x=10,y=40,size=16,color=0xf800,bk=0x0000,txt=wifi connecting fail");
    }
    else
    {
        memset(show, 0, sizeof(show));
        strncpy(show, p_task_arg->run.ssid, sizeof(show) - 1);
        sprintf(msg, "str:x=10,y=40,size=16,color=0x001f,bk=0x0000,txt=ssid:%s", show);
        msg_send("task_lcd_msg", msg);

        memset(show, 0, sizeof(show));
        strncpy(show, p_task_arg->run.pwd, sizeof(show) - 1);
        sprintf(msg, "str:x=10,y=60,size=16,color=0x001f,bk=0x0000,txt=psw:%s", show);
        msg_send("task_lcd_msg", msg);

        sprintf(msg, "str:x=10,y=80,size=16,color=0x07e0,bk=0x0000,txt=ip:%s", p_task_arg->sta_ip);
        msg_send("task_lcd_msg", msg);
    }
    msg_send("task_lcd_msg", "setup_done");
}

/* flag: 0配网中 1失败 2成功, 对齐正点原子SmartConfig屏显 */
static void wifi_lcd_show_sc(struct st_task_wifi_run *p_task_arg, unsigned int flag)
{
    char msg[96];
    char show[21];

    if(p_task_arg == NULL) return;
    if(lvgl_ui_ready())
        return;

    if(flag == 0)
        msg_send("task_lcd_msg", "fill:x=0,y=0,w=320,h=240,color=0x0000");
    else
        msg_send("task_lcd_msg", "fill:x=0,y=24,w=320,h=216,color=0x0000");
    msg_send("task_lcd_msg", "str:x=10,y=5,size=16,color=0xffff,bk=0x0000,txt=WiFi SmartConfig:");

    if(flag == 0)
    {
        msg_send("task_lcd_msg", "str:x=10,y=40,size=16,color=0x07ff,bk=0x0000,txt=In the distribution network......");
    }
    else if(flag == 1)
    {
        msg_send("task_lcd_msg", "str:x=10,y=40,size=16,color=0xf800,bk=0x0000,txt=smartconfig fail");
    }
    else
    {
        memset(show, 0, sizeof(show));
        strncpy(show, p_task_arg->run.ssid, sizeof(show) - 1);
        sprintf(msg, "str:x=10,y=40,size=16,color=0x001f,bk=0x0000,txt=ssid:%s", show);
        msg_send("task_lcd_msg", msg);

        memset(show, 0, sizeof(show));
        strncpy(show, p_task_arg->run.pwd, sizeof(show) - 1);
        sprintf(msg, "str:x=10,y=60,size=16,color=0x001f,bk=0x0000,txt=psw:%s", show);
        msg_send("task_lcd_msg", msg);

        sprintf(msg, "str:x=10,y=80,size=16,color=0x07e0,bk=0x0000,txt=ip:%s", p_task_arg->sta_ip);
        msg_send("task_lcd_msg", msg);

        msg_send("task_lcd_msg", "str:x=10,y=100,size=16,color=0x07e0,bk=0x0000,txt=Successful distribution network");
    }
    msg_send("task_lcd_msg", "setup_done");
}

/* ======================================================================
 * WiFi扫描小状态: scan发起(非阻塞) -> wait等SCAN_DONE -> show刷屏
 * 等事件时返回c_ret_wt, 不堵协作循环
 * ====================================================================== */
static unsigned int wifi_scan_run(struct st_task_wifi_run *p_task_arg)
{
    #define c_step_scan     0
    #define c_step_wait     1
    #define c_step_show     2
    static unsigned int scan_step = 0;
    static unsigned int scan_tmr = 0;
    uint16_t number;

    if(p_task_arg == NULL) return c_ret_nk;

    /* 新任务dat已清零; stop后step可能仍停在wait, 这里拉回scan */
    if((p_task_arg->scan_busy == 0) && (p_task_arg->scan_done == 0))
        scan_step = c_step_scan;

    switch(scan_step)
    {
        case c_step_scan:
            memset(p_task_arg->ap_info, 0, sizeof(p_task_arg->ap_info));
            p_task_arg->ap_count = 0;
            p_task_arg->scan_done = 0;
            p_task_arg->scan_status = 0;
            p_task_arg->scan_busy = 1;
            s_wifi_view.scanning = 1;
            s_wifi_view.fail = 0;
            dbgtx("wifi scan start...\r\n");
            if(esp_wifi_scan_start(NULL, false) != ESP_OK)
            {
                p_task_arg->scan_busy = 0;
                s_wifi_view.scanning = 0;
                scan_step = c_step_scan;
                dbgtx("wifi scan start err\r\n");
                return c_ret_nk;
            }
            scan_tmr = tick_get();
            scan_step = c_step_wait;
            return c_ret_wt;

        case c_step_wait:
            if(p_task_arg->scan_done == 0)
            {
                if(tick_cmp(scan_tmr, c_wifi_scan_tmo_ms) == c_ret_ok)
                {
                    p_task_arg->scan_busy = 0;
                    s_wifi_view.scanning = 0;
                    scan_step = c_step_scan;
                    esp_wifi_scan_stop();
                    dbgtx("wifi scan timeout\r\n");
                    return c_ret_nk;
                }
                return c_ret_wt;
            }
            p_task_arg->scan_busy = 0;
            if(p_task_arg->scan_status != 0)
            {
                s_wifi_view.scanning = 0;
                scan_step = c_step_scan;
                dbgtx("wifi scan done err, status=%u\r\n", p_task_arg->scan_status);
                return c_ret_nk;
            }
            number = c_wifi_ap_max;
            if(esp_wifi_scan_get_ap_records(&number, p_task_arg->ap_info) != ESP_OK)
            {
                scan_step = c_step_scan;
                dbgtx("wifi scan get records err\r\n");
                return c_ret_nk;
            }
            if(esp_wifi_scan_get_ap_num(&p_task_arg->ap_count) != ESP_OK)
            {
                scan_step = c_step_scan;
                dbgtx("wifi scan get num err\r\n");
                return c_ret_nk;
            }
            if(p_task_arg->ap_count > c_wifi_ap_max)
                p_task_arg->ap_count = c_wifi_ap_max;
            dbgtx("wifi scan done, ap_count=%u\r\n", p_task_arg->ap_count);
            scan_step = c_step_show;
            return c_ret_wt;

        case c_step_show:
            for(int i = 0; (i < c_wifi_ap_max) && (i < p_task_arg->ap_count); i++)
            {
                dbgtx("ap[%d] ssid=%s rssi=%d ch=%u auth=%d\r\n",
                      i, (char *)p_task_arg->ap_info[i].ssid, p_task_arg->ap_info[i].rssi,
                      p_task_arg->ap_info[i].primary, p_task_arg->ap_info[i].authmode);
            }
            wifi_lcd_show_all(p_task_arg);
            dbgtx("wifi scan display done\r\n");
            scan_step = c_step_scan;
            return c_ret_ok;

        default:
            scan_step = c_step_scan;
            return c_ret_nk;
    }
}


/* ======================================================================
 * STA小状态: sta发起connect -> wait等GOT_IP/失败 -> show刷屏
 * step/tmr 仅本函数使用, 用static; 事件由wifi_ev_drain写入标志
 * ====================================================================== */
static unsigned int wifi_sta_run(struct st_task_wifi_run *p_task_arg)
{
    #define c_step_sta      0
    #define c_step_wait     1
    #define c_step_show     2
    static unsigned int sta_step = 0;
    static unsigned int sta_tmr = 0;
    wifi_config_t cfg;

    if(p_task_arg == NULL) return c_ret_nk;

    /* 新任务dat已清零; stop后step可能仍停在wait, 这里拉回sta */
    if((p_task_arg->sta_busy == 0) && (p_task_arg->sta_got_ip == 0) && (p_task_arg->sta_fail == 0))
        sta_step = c_step_sta;

    switch(sta_step)
    {
        case c_step_sta:
            if(p_task_arg->run.ssid[0] == 0)
            {
                dbgtx("wifi sta ssid empty\r\n");
                return c_ret_nk;
            }
            memset(&cfg, 0, sizeof(cfg));
            memcpy(cfg.sta.ssid, p_task_arg->run.ssid, c_wifi_ssid_max);
            memcpy(cfg.sta.password, p_task_arg->run.pwd, c_wifi_pwd_max);
            if(p_task_arg->run.pwd[0] == 0)
                cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
            else
                cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

            p_task_arg->sta_got_ip = 0;
            p_task_arg->sta_fail = 0;
            p_task_arg->sta_retry = 0;
            p_task_arg->sta_reconn = 0;
            p_task_arg->sta_disc_ignore = 0;
            p_task_arg->sta_ip[0] = 0;
            p_task_arg->sta_busy = 1;

            if(esp_wifi_set_config(WIFI_IF_STA, &cfg) != ESP_OK)
            {
                p_task_arg->sta_busy = 0;
                sta_step = c_step_sta;
                dbgtx("wifi sta set_config err\r\n");
                return c_ret_nk;
            }

            wifi_lcd_show_sta(p_task_arg, 0);
            dbgtx("wifi sta connecting ssid=%s\r\n", p_task_arg->run.ssid);

            if(wifi_sta_do_connect(p_task_arg, 1) != c_ret_ok)
            {
                p_task_arg->sta_busy = 0;
                sta_step = c_step_sta;
                dbgtx("wifi sta connect err\r\n");
                return c_ret_nk;
            }

            sta_tmr = tick_get();
            sta_step = c_step_wait;
            return c_ret_wt;

        case c_step_wait:
            wifi_sta_reconn_poll(p_task_arg);
            if((p_task_arg->sta_got_ip == 0) && (p_task_arg->sta_fail == 0))
            {
                if(tick_cmp(sta_tmr, c_wifi_sta_tmo_ms) == c_ret_ok)
                {
                    p_task_arg->sta_busy = 0;
                    p_task_arg->sta_fail = 1;
                    p_task_arg->sta_reconn = 0;
                    dbgtx("wifi sta timeout ssid=%s retry=%u\r\n", p_task_arg->run.ssid, p_task_arg->sta_retry);
                    sta_step = c_step_show;
                    return c_ret_wt;
                }
                return c_ret_wt;
            }
            p_task_arg->sta_busy = 0;
            p_task_arg->sta_reconn = 0;
            if(p_task_arg->sta_got_ip)
                dbgtx("wifi sta ok ssid=%s ip=%s\r\n", p_task_arg->run.ssid, p_task_arg->sta_ip);
            else
                dbgtx("wifi sta fail ssid=%s retry=%u\r\n", p_task_arg->run.ssid, p_task_arg->sta_retry);
            sta_step = c_step_show;
            return c_ret_wt;

        case c_step_show:
            if(p_task_arg->sta_got_ip)
            {
                wifi_lcd_show_sta(p_task_arg, 2);
                dbgtx("wifi sta display done\r\n");
                sta_step = c_step_sta;
                return c_ret_ok;
            }
            wifi_lcd_show_sta(p_task_arg, 1);
            dbgtx("wifi sta display done\r\n");
            sta_step = c_step_sta;
            return c_ret_nk;

        default:
            sta_step = c_step_sta;
            return c_ret_nk;
    }
}

/* ======================================================================
 * SmartConfig小状态: start配网 -> wait等SSID/GOT_IP/ACK -> show刷屏
 * 参考正点原子04_WiFi_SmartConfig, 不用EventGroup/portMAX_DELAY/独立任务
 * ====================================================================== */
static unsigned int wifi_smartconfig_run(struct st_task_wifi_run *p_task_arg)
{
    #define c_step_sc_start 0
    #define c_step_sc_wait  1
    #define c_step_sc_show  2
    static unsigned int sc_step = 0;
    static unsigned int sc_tmr = 0;
    smartconfig_start_config_t cfg;

    if(p_task_arg == NULL) return c_ret_nk;

    if((p_task_arg->sc_busy == 0) && (p_task_arg->sc_got_ssid == 0) &&
       (p_task_arg->sc_fail == 0) && (p_task_arg->sta_got_ip == 0))
        sc_step = c_step_sc_start;

    switch(sc_step)
    {
        case c_step_sc_start:
            p_task_arg->sc_scan_done = 0;
            p_task_arg->sc_got_ssid = 0;
            p_task_arg->sc_ack_done = 0;
            p_task_arg->sc_fail = 0;
            p_task_arg->sta_got_ip = 0;
            p_task_arg->sta_fail = 0;
            p_task_arg->sta_retry = 0;
            p_task_arg->sta_reconn = 0;
            p_task_arg->sta_disc_ignore = 0;
            p_task_arg->sta_busy = 0;
            p_task_arg->sta_ip[0] = 0;
            p_task_arg->run.ssid[0] = 0;
            p_task_arg->run.pwd[0] = 0;

            esp_wifi_disconnect();
            esp_smartconfig_stop();

            if(esp_smartconfig_set_type(SC_TYPE_ESPTOUCH) != ESP_OK)
            {
                sc_step = c_step_sc_start;
                dbgtx("wifi sc set_type err\r\n");
                return c_ret_nk;
            }

            cfg = (smartconfig_start_config_t)SMARTCONFIG_START_CONFIG_DEFAULT();
            p_task_arg->sc_busy = 1;
            if(esp_smartconfig_start(&cfg) != ESP_OK)
            {
                p_task_arg->sc_busy = 0;
                sc_step = c_step_sc_start;
                dbgtx("wifi sc start err\r\n");
                return c_ret_nk;
            }

            wifi_lcd_show_sc(p_task_arg, 0);
            dbgtx("wifi sc start, wait EspTouch\r\n");
            sc_tmr = tick_get();
            sc_step = c_step_sc_wait;
            return c_ret_wt;

        case c_step_sc_wait:
            wifi_sta_reconn_poll(p_task_arg);
            if(p_task_arg->sc_fail || p_task_arg->sta_fail)
            {
                dbgtx("wifi sc fail ssid=%s retry=%u\r\n", p_task_arg->run.ssid, p_task_arg->sta_retry);
                sc_step = c_step_sc_show;
                return c_ret_wt;
            }
            if(p_task_arg->sta_got_ip && p_task_arg->sc_ack_done)
            {
                dbgtx("wifi sc ok ssid=%s ip=%s\r\n", p_task_arg->run.ssid, p_task_arg->sta_ip);
                sc_step = c_step_sc_show;
                return c_ret_wt;
            }
            if(tick_cmp(sc_tmr, c_wifi_sc_tmo_ms) == c_ret_ok)
            {
                if(p_task_arg->sta_got_ip)
                {
                    dbgtx("wifi sc ok(no ack) ssid=%s ip=%s\r\n", p_task_arg->run.ssid, p_task_arg->sta_ip);
                }
                else
                {
                    p_task_arg->sc_fail = 1;
                    dbgtx("wifi sc timeout\r\n");
                }
                sc_step = c_step_sc_show;
                return c_ret_wt;
            }
            return c_ret_wt;

        case c_step_sc_show:
            wifi_sc_abort(p_task_arg);
            if(p_task_arg->sta_got_ip)
            {
                wifi_lcd_show_sc(p_task_arg, 2);
                dbgtx("wifi sc display done\r\n");
                sc_step = c_step_sc_start;
                return c_ret_ok;
            }
            wifi_lcd_show_sc(p_task_arg, 1);
            dbgtx("wifi sc display done\r\n");
            sc_step = c_step_sc_start;
            return c_ret_nk;

        default:
            sc_step = c_step_sc_start;
            return c_ret_nk;
    }
}

/* ======================================================================
 * WiFi任务状态机 (在初始化上面)
 * ====================================================================== */
void task_wifi_proc(void)
{
    #define c_step_idle             0
    #define c_step_wifi_scan        1
    #define c_step_wifi_sta         2
    #define c_step_wifi_sc          3
    struct st_task_wifi_run *p_task_arg;
    unsigned int ret;

    p_task_arg = (struct st_task_wifi_run *)sys.curr_task->dat;

    if(task_wifi_set.stop_req_seq_bk != task_wifi_set.stop_req_seq)
    {
        task_wifi_set.stop_req_seq_bk = task_wifi_set.stop_req_seq;
        if((p_task_arg != NULL) && p_task_arg->sta_busy)
        {
            p_task_arg->sta_busy = 0;
            p_task_arg->sta_reconn = 0;
            p_task_arg->sta_disc_ignore = 0;
            esp_wifi_disconnect();
        }
        if((p_task_arg != NULL) && p_task_arg->scan_busy)
        {
            p_task_arg->scan_busy = 0;
            esp_wifi_scan_stop();
        }
        wifi_sc_abort(p_task_arg);
        if(s_wifi_ev_q != NULL) xQueueReset(s_wifi_ev_q);
        sys.curr_task->step = c_step_idle;
        sys.curr_task->done = c_done_nk;
        return;
    }
    wifi_ev_drain(p_task_arg);
    switch(sys.curr_task->step)
    {
        case c_step_idle:
            if(task_wifi_set.setup_req_seq_bk == task_wifi_set.setup_req_seq) return;
            task_wifi_set.setup_req_seq_bk = task_wifi_set.setup_req_seq;
            if(s_wifi_ev_q != NULL) xQueueReset(s_wifi_ev_q);

            p_task_arg->run = task_wifi_set.set;
            if(p_task_arg->run.cmd == c_wifi_cmd_scan)
                sys.curr_task->step = c_step_wifi_scan;
            else if(p_task_arg->run.cmd == c_wifi_cmd_sta)
                sys.curr_task->step = c_step_wifi_sta;
            else if(p_task_arg->run.cmd == c_wifi_cmd_sc)
                sys.curr_task->step = c_step_wifi_sc;
            else
            {
                sys.curr_task->step = c_step_idle;
                sys.curr_task->done = c_done_ok;
            }
        break;

        case c_step_wifi_scan:
            ret = wifi_scan_run(p_task_arg);
            if(ret == c_ret_wt) return;
            sys.curr_task->step = c_step_idle;
            sys.curr_task->done = c_done_ok;
        break;

        case c_step_wifi_sta:
            ret = wifi_sta_run(p_task_arg);
            if(ret == c_ret_wt) return;
            sys.curr_task->step = c_step_idle;
            sys.curr_task->done = c_done_ok;
        break;

        case c_step_wifi_sc:
            ret = wifi_smartconfig_run(p_task_arg);
            if(ret == c_ret_wt) return;
            sys.curr_task->step = c_step_idle;
            sys.curr_task->done = c_done_ok;
        break;

        default: sys.curr_task->step = c_step_idle; break;
    }
}

/* ======================================================================
 * WiFi初始化注册 (在最下面)
 * 参考: 正点原子01_WiFi_SCAN / 02_WiFi_STA / 04_WiFi_SmartConfig
 * ====================================================================== */
void wifi_init(void)
{
    esp_err_t ret;

    /* NVS初始化 (WiFi需要) */
    ret = nvs_flash_init();
    if(ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* 网卡/事件循环/WiFi初始化 */
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_wifi_ev_q = xQueueCreate(c_wifi_ev_q_len, sizeof(struct st_wifi_ev));
    ESP_ERROR_CHECK((s_wifi_ev_q != NULL) ? ESP_OK : ESP_ERR_NO_MEM);
    esp_netif_create_default_wifi_sta();
    
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(SC_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());

    msg_add("task_wifi_msg", task_wifi_msg_parse);
    dbgtx("wifi_init ok\r\n");
}
INIT_REG(wifi_init, 1);     /* 优先级1: 最早执行, WiFi/事件循环不依赖其他模块 */

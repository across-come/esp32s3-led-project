/*
 * LVGL 界面 (阶段3): 只读状态、只发 msg_send, 不连 WiFi / 不写卡
 * flush 仍走 esp_lcd DMA 并等待, 与 TF 卡分时 SPI2
 */
#include "user_ext.h"
#include "task_lcd.h"
#include "task_lvgl.h"
#include "task_wifi.h"
#include "task_tcp.h"
#include "wifi_ip.h"
#include "task_data.h"
#include "task_sd.h"
#include "cache_store.h"
#include "lvgl.h"
#include "esp_err.h"
#include "esp_timer.h"

#define LVGL_DRAW_ROWS      40
#define LVGL_HANDLER_MS     10
#define LVGL_REFRESH_MS     200

#define PAGE_HOME           0
#define PAGE_SCAN           1
#define PAGE_STA            2
#define PAGE_DATA           3
#define PAGE_TCP            4
#define PAGE_N              5

#define HOME_WIFI           0
#define HOME_TCP            1
#define HOME_DATA           2
#define HOME_SD             3
#define HOME_N              4

#define COL_WHITE           0xFFFFFF
#define COL_CYAN            0x00FFFF
#define COL_GREEN           0x00FF00
#define COL_RED             0xFF0000
#define COL_YELLOW          0xFFFF00
#define COL_GRAY            0x808080
#define COL_DARK            0x202020

static lv_disp_draw_buf_t s_disp_buf;
static lv_disp_drv_t s_disp_drv;
static unsigned int s_lvgl_ok = 0;
static unsigned int s_page_id = PAGE_HOME;
static unsigned int s_home_focus = HOME_WIFI;
static unsigned int s_scan_sel = 0;
static char s_sta_ssid[c_wifi_ssid_max + 1] = {0};
static char s_sta_pwd[c_wifi_pwd_max + 1] = {0};
static unsigned int s_sta_pwd_ok = 0;
static unsigned int s_refresh_tmr = 0;

static lv_obj_t *s_title;
static lv_obj_t *s_hint;
static lv_obj_t *s_dot[3];
static lv_obj_t *s_page[PAGE_N];
static lv_obj_t *s_home_line[HOME_N];
static lv_obj_t *s_scan_line[c_wifi_ap_max];
static lv_obj_t *s_scan_empty;
static lv_obj_t *s_sta_ssid_lab;
static lv_obj_t *s_sta_st_lab;
static lv_obj_t *s_sta_ip_lab;
static lv_obj_t *s_sta_hint;
static lv_obj_t *s_data_lab[6];
static lv_obj_t *s_tcp_lab[5];

unsigned int lvgl_ui_ready(void)
{
    return s_lvgl_ok;
}

static void increase_lvgl_tick(void *arg)
{
    (void)arg;
    lv_tick_inc(1);
}

static void lvgl_disp_flush_cb(lv_disp_drv_t *drv, const lv_area_t *area, lv_color_t *color_map)
{
    lcd_draw_bitmap_wait(area->x1, area->y1, area->x2 + 1, area->y2 + 1, color_map);
    lv_disp_flush_ready(drv);
}

static void label_set(lv_obj_t *lab, const char *txt)
{
    const char *cur;

    if((lab == NULL) || (txt == NULL))
        return;
    cur = lv_label_get_text(lab);
    if((cur != NULL) && (strcmp(cur, txt) == 0))
        return;
    lv_label_set_text(lab, txt);
}

static void label_color(lv_obj_t *lab, unsigned int rgb)
{
    if(lab == NULL)
        return;
    lv_obj_set_style_text_color(lab, lv_color_hex(rgb), 0);
}

static lv_obj_t *mk_label(lv_obj_t *parent, const lv_font_t *font, unsigned int rgb, lv_coord_t x, lv_coord_t y)
{
    lv_obj_t *lab = lv_label_create(parent);

    lv_obj_set_style_text_font(lab, font, 0);
    lv_obj_set_style_text_color(lab, lv_color_hex(rgb), 0);
    lv_obj_set_pos(lab, x, y);
    lv_label_set_text(lab, "");
    return lab;
}

static lv_obj_t *mk_page(lv_obj_t *parent)
{
    lv_obj_t *p = lv_obj_create(parent);

    lv_obj_set_size(p, LCD_W, LCD_H - 32 - 28);
    lv_obj_set_pos(p, 0, 32);
    lv_obj_set_style_bg_color(p, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_pad_all(p, 0, 0);
    lv_obj_set_style_radius(p, 0, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

static unsigned int pwd_lookup(const char *ssid, char *pwd, unsigned int pwd_n)
{
    static const struct
    {
        const char *ssid;
        const char *pwd;
    } tab[] = {
        {"test_8266", "12345678"},
        {"1111", "1234567890"},
    };
    unsigned int i;

    if((ssid == NULL) || (pwd == NULL) || (pwd_n == 0))
        return 0;
    pwd[0] = 0;
    for(i = 0; i < (sizeof(tab) / sizeof(tab[0])); i++)
    {
        if(strcmp(ssid, tab[i].ssid) == 0)
        {
            strncpy(pwd, tab[i].pwd, pwd_n - 1);
            pwd[pwd_n - 1] = 0;
            return 1;
        }
    }
    return 0;
}

static void ui_show_page(unsigned int id)
{
    static const char *title[PAGE_N] = {"HOME", "WiFi SCAN", "WiFi STA", "DATA / SD", "TCP"};
    static const char *hint[PAGE_N] = {
        "K0 OK  K1 UP  K2 DN  K3 BK",
        "K0 SEL  K1 UP  K2 DN  K3 BK",
        "K0 CON/DISC  K3 BK",
        "hold K0 DATA  K3 BK",
        "K0 CON/DISC  K3 BK"
    };
    unsigned int i;

    if(id >= PAGE_N)
        return;
    s_page_id = id;
    for(i = 0; i < PAGE_N; i++)
    {
        if(s_page[i] == NULL)
            continue;
        if(i == id)
            lv_obj_clear_flag(s_page[i], LV_OBJ_FLAG_HIDDEN);
        else
            lv_obj_add_flag(s_page[i], LV_OBJ_FLAG_HIDDEN);
    }
    label_set(s_title, title[id]);
    label_set(s_hint, hint[id]);
}

static void ui_dot_set(lv_obj_t *dot, unsigned int rgb)
{
    if(dot == NULL)
        return;
    lv_obj_set_style_bg_color(dot, lv_color_hex(rgb), 0);
}

static void wifi_sta_send(void)
{
    char buf[96];

    if(s_sta_ssid[0] == 0)
        return;
    snprintf(buf, sizeof(buf), "wifi_sta:ssid=%s,pwd=%s", s_sta_ssid, s_sta_pwd);
    dbgtx("ui sta ssid=%s pwd_ok=%u\r\n", s_sta_ssid, s_sta_pwd_ok);
    msg_send("task_wifi_msg", buf);
    msg_send("task_wifi_msg", "setup");
}

static void tcp_toggle_send(void)
{
    char buf[64];

    if(task_get("task_tcp") != NULL)
    {
        msg_send("task_tcp_msg", "stop");
        return;
    }
    if(task_get("task_sd") == NULL)
        msg_send("task_sd_msg", "setup");
    snprintf(buf, sizeof(buf), "tcp:ip=%s,port=%u", c_tcp_ip, c_tcp_port);
    msg_send("task_tcp_msg", buf);
    msg_send("task_tcp_msg", "setup");
}

static void ui_on_up(void)
{
    const struct st_wifi_view *w = wifi_view_get();

    if(s_page_id == PAGE_HOME)
        s_home_focus = (s_home_focus == 0) ? (HOME_N - 1) : (s_home_focus - 1);
    else if(s_page_id == PAGE_SCAN)
    {
        if((w != NULL) && (w->ap_count > 0) && (s_scan_sel > 0))
            s_scan_sel--;
    }
}

static void ui_on_down(void)
{
    const struct st_wifi_view *w = wifi_view_get();

    if(s_page_id == PAGE_HOME)
        s_home_focus = (s_home_focus + 1) % HOME_N;
    else if(s_page_id == PAGE_SCAN)
    {
        if((w != NULL) && (w->ap_count > 0) && ((s_scan_sel + 1) < w->ap_count))
            s_scan_sel++;
    }
}

static void ui_on_ok(void)
{
    const struct st_wifi_view *w = wifi_view_get();
    const struct st_tcp_view *t = tcp_view_get();

    if(s_page_id == PAGE_HOME)
    {
        if(s_home_focus == HOME_WIFI)
            ui_show_page(PAGE_SCAN);
        else if(s_home_focus == HOME_TCP)
            ui_show_page(PAGE_TCP);
        else
            ui_show_page(PAGE_DATA);
        return;
    }
    if(s_page_id == PAGE_SCAN)
    {
        if((w == NULL) || (w->ap_count == 0) || (s_scan_sel >= w->ap_count))
            return;
        memset(s_sta_ssid, 0, sizeof(s_sta_ssid));
        strncpy(s_sta_ssid, w->ap_ssid[s_scan_sel], c_wifi_ssid_max);
        s_sta_pwd_ok = pwd_lookup(s_sta_ssid, s_sta_pwd, sizeof(s_sta_pwd));
        ui_show_page(PAGE_STA);
        if(s_sta_pwd_ok)
            wifi_sta_send();
        return;
    }
    if(s_page_id == PAGE_STA)
    {
        if((w != NULL) && w->got_ip)
            msg_send("task_wifi_msg", "wifi_disc");
        else if(s_sta_pwd_ok)
            wifi_sta_send();
        return;
    }
    if(s_page_id == PAGE_TCP)
    {
        (void)t;
        tcp_toggle_send();
        return;
    }
}

static void ui_on_back(void)
{
    if(s_page_id == PAGE_STA)
        ui_show_page(PAGE_SCAN);
    else if(s_page_id != PAGE_HOME)
        ui_show_page(PAGE_HOME);
}

unsigned int task_lvgl_msg_parse(char *buf)
{
    if(buf == NULL)
        return c_ret_nk;
    if(s_lvgl_ok == 0)
        return c_ret_nk;

    if(strcmp(buf, "up") == 0)
        ui_on_up();
    else if(strcmp(buf, "down") == 0)
        ui_on_down();
    else if(strcmp(buf, "ok") == 0)
        ui_on_ok();
    else if(strcmp(buf, "back") == 0)
        ui_on_back();
    else if(strcmp(buf, "page:home") == 0)
        ui_show_page(PAGE_HOME);
    else if(strcmp(buf, "page:scan") == 0)
        ui_show_page(PAGE_SCAN);
    else if(strcmp(buf, "page:sta") == 0)
        ui_show_page(PAGE_STA);
    else if(strcmp(buf, "page:data") == 0)
        ui_show_page(PAGE_DATA);
    else if(strcmp(buf, "page:tcp") == 0)
        ui_show_page(PAGE_TCP);
    else
        return c_ret_nk;
    return c_ret_ok;
}

static void ui_refresh_home(const struct st_wifi_view *w, const struct st_tcp_view *t)
{
    char line[40];
    char ip[16];
    unsigned int i, nfile, data_run;
    unsigned int col;

    if(w->scanning)
        snprintf(line, sizeof(line), "WiFi  scanning");
    else if(w->connecting)
        snprintf(line, sizeof(line), "WiFi  connecting");
    else if(w->got_ip && (w->ip[0] != 0))
        snprintf(line, sizeof(line), "WiFi  IP %s", w->ip);
    else if(wifi_sta_ip_get(ip, sizeof(ip)) == c_ret_ok)
        snprintf(line, sizeof(line), "WiFi  IP %s", ip);
    else if(w->fail)
        snprintf(line, sizeof(line), "WiFi  fail");
    else
        snprintf(line, sizeof(line), "WiFi  idle");
    label_set(s_home_line[HOME_WIFI], line);

    if(t->connecting)
        snprintf(line, sizeof(line), "TCP   connecting");
    else if(t->ok)
        snprintf(line, sizeof(line), "TCP   dst:%u ok", (unsigned int)c_tcp_port);
    else if(t->fail)
        snprintf(line, sizeof(line), "TCP   fail");
    else
        snprintf(line, sizeof(line), "TCP   idle");
    label_set(s_home_line[HOME_TCP], line);

    nfile = cache_nfile();
    if(sd_mount_fail())
        snprintf(line, sizeof(line), "SD    fail");
    else if(sd_fat_drv() != NULL)
        snprintf(line, sizeof(line), "SD    ok  nfile=%u", nfile);
    else
        snprintf(line, sizeof(line), "SD    unmount");
    label_set(s_home_line[HOME_SD], line);

    data_run = (task_get("task_data") != NULL);
    if(data_run)
        snprintf(line, sizeof(line), "DATA  run  SEQ=%06u", data_gen_seq_get());
    else
        snprintf(line, sizeof(line), "DATA  stop");
    label_set(s_home_line[HOME_DATA], line);

    for(i = 0; i < HOME_N; i++)
    {
        col = (i == s_home_focus) ? COL_CYAN : COL_WHITE;
        if((i == HOME_SD) && sd_mount_fail())
            col = COL_RED;
        if((i == HOME_WIFI) && w->fail && (i != s_home_focus))
            col = COL_RED;
        if((i == HOME_TCP) && t->fail && (i != s_home_focus))
            col = COL_RED;
        label_color(s_home_line[i], col);
    }
}

static void ui_refresh_scan(const struct st_wifi_view *w)
{
    char line[40];
    unsigned int i, n;

    n = w->ap_count;
    if(n > c_wifi_ap_max)
        n = c_wifi_ap_max;
    if((n > 0) && (s_scan_sel >= n))
        s_scan_sel = n - 1;

    if(w->scanning)
        label_set(s_scan_empty, "scanning...");
    else if(n == 0)
        label_set(s_scan_empty, "empty, hold K1 scan");
    else
        label_set(s_scan_empty, "");

    for(i = 0; i < c_wifi_ap_max; i++)
    {
        if(i < n)
        {
            snprintf(line, sizeof(line), "%s  rssi:%d", w->ap_ssid[i], w->ap_rssi[i]);
            label_set(s_scan_line[i], line);
            label_color(s_scan_line[i], (i == s_scan_sel) ? COL_CYAN : COL_WHITE);
            lv_obj_clear_flag(s_scan_line[i], LV_OBJ_FLAG_HIDDEN);
        }
        else
        {
            label_set(s_scan_line[i], "");
            lv_obj_add_flag(s_scan_line[i], LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void ui_refresh_sta(const struct st_wifi_view *w)
{
    char line[40];
    char ip[16];

    snprintf(line, sizeof(line), "ssid: %s", (s_sta_ssid[0] != 0) ? s_sta_ssid : "-");
    label_set(s_sta_ssid_lab, line);

    if(w->connecting)
    {
        label_set(s_sta_st_lab, "connecting...");
        label_color(s_sta_st_lab, COL_YELLOW);
    }
    else if(w->got_ip || (wifi_sta_ip_get(ip, sizeof(ip)) == c_ret_ok))
    {
        label_set(s_sta_st_lab, "connected");
        label_color(s_sta_st_lab, COL_GREEN);
    }
    else if(w->fail)
    {
        label_set(s_sta_st_lab, "connect fail");
        label_color(s_sta_st_lab, COL_RED);
    }
    else
    {
        label_set(s_sta_st_lab, "idle");
        label_color(s_sta_st_lab, COL_WHITE);
    }

    if(w->ip[0] != 0)
        snprintf(line, sizeof(line), "ip: %s", w->ip);
    else if(wifi_sta_ip_get(ip, sizeof(ip)) == c_ret_ok)
        snprintf(line, sizeof(line), "ip: %s", ip);
    else
        snprintf(line, sizeof(line), "ip: -");
    label_set(s_sta_ip_lab, line);

    if(s_sta_pwd_ok == 0)
        label_set(s_sta_hint, "unknown SSID, use serial wifi_sta");
    else
        label_set(s_sta_hint, "K0 connect / disconnect");
}

static void ui_refresh_data(const struct st_tcp_view *t)
{
    char line[48];
    const char *last;
    unsigned int run, nfile;

    run = (task_get("task_data") != NULL);
    nfile = cache_nfile();
    snprintf(line, sizeof(line), "interval: %us", (unsigned int)(c_dg_interval_ms / 1000));
    label_set(s_data_lab[0], line);
    snprintf(line, sizeof(line), "SEQ: %06u", data_gen_seq_get());
    label_set(s_data_lab[1], line);
    snprintf(line, sizeof(line), "cache files: %u", nfile);
    label_set(s_data_lab[2], line);

    last = data_gen_last();
    if((last != NULL) && (last[0] != 0))
        snprintf(line, sizeof(line), "last: %.28s", last);
    else
        snprintf(line, sizeof(line), "last: -");
    label_set(s_data_lab[3], line);

    if(sd_mount_fail())
    {
        label_set(s_data_lab[4], "SD mount fail");
        label_color(s_data_lab[4], COL_RED);
    }
    else if(sd_fat_drv() == NULL)
    {
        label_set(s_data_lab[4], "SD unmount");
        label_color(s_data_lab[4], COL_GRAY);
    }
    else
    {
        label_set(s_data_lab[4], "SD ok");
        label_color(s_data_lab[4], COL_GREEN);
    }

    if(run == 0)
        label_set(s_data_lab[5], "STOPPED");
    else if(t->ok)
        label_set(s_data_lab[5], "ONLINE sending");
    else
        label_set(s_data_lab[5], "OFFLINE writing");
}

static void ui_refresh_tcp(const struct st_tcp_view *t)
{
    char line[48];

    snprintf(line, sizeof(line), "dst %s", (t->dst[0] != 0) ? t->dst : "-");
    label_set(s_tcp_lab[0], line);
    snprintf(line, sizeof(line), "local %s", (t->local[0] != 0) ? t->local : "-");
    label_set(s_tcp_lab[1], line);
    if(t->hello_sent)
        label_set(s_tcp_lab[2], "hello: sent");
    else if(t->connecting)
        label_set(s_tcp_lab[2], "hello: wait");
    else
        label_set(s_tcp_lab[2], "hello: -");
    snprintf(line, sizeof(line), "rx: %.24s", (t->rx[0] != 0) ? t->rx : "-");
    label_set(s_tcp_lab[3], line);
    if(t->cache_wait)
        label_set(s_tcp_lab[4], "cache upload: wait ack");
    else if(t->ok)
        label_set(s_tcp_lab[4], "cache upload: ok");
    else
        label_set(s_tcp_lab[4], "cache upload: -");
}

static void ui_refresh(void)
{
    const struct st_wifi_view *w = wifi_view_get();
    const struct st_tcp_view *t = tcp_view_get();
    char ip[16];
    unsigned int wifi_rgb, tcp_rgb, sd_rgb;

    if((w == NULL) || (t == NULL))
        return;

    if(w->got_ip || (wifi_sta_ip_get(ip, sizeof(ip)) == c_ret_ok))
        wifi_rgb = COL_GREEN;
    else if(w->scanning || w->connecting)
        wifi_rgb = COL_YELLOW;
    else if(w->fail)
        wifi_rgb = COL_RED;
    else
        wifi_rgb = COL_GRAY;

    if(t->ok)
        tcp_rgb = COL_GREEN;
    else if(t->connecting)
        tcp_rgb = COL_YELLOW;
    else if(t->fail)
        tcp_rgb = COL_RED;
    else
        tcp_rgb = COL_GRAY;

    if(sd_mount_fail())
        sd_rgb = COL_RED;
    else if(sd_fat_drv() != NULL)
        sd_rgb = COL_GREEN;
    else
        sd_rgb = COL_GRAY;

    ui_dot_set(s_dot[0], wifi_rgb);
    ui_dot_set(s_dot[1], tcp_rgb);
    ui_dot_set(s_dot[2], sd_rgb);

    ui_refresh_home(w, t);
    ui_refresh_scan(w);
    ui_refresh_sta(w);
    ui_refresh_data(t);
    ui_refresh_tcp(t);
}

static lv_obj_t *mk_dot(lv_obj_t *parent, lv_coord_t x)
{
    lv_obj_t *dot = lv_obj_create(parent);

    lv_obj_set_size(dot, 12, 12);
    lv_obj_set_pos(dot, x, 8);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(COL_GRAY), 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_set_style_pad_all(dot, 0, 0);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
    return dot;
}

static void lvgl_ui_create(void)
{
    lv_obj_t *scr = lv_scr_act();
    lv_obj_t *bar;
    unsigned int i;

    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    s_title = mk_label(scr, &lv_font_montserrat_16, COL_WHITE, 8, 6);
    label_set(s_title, "HOME");
    s_dot[0] = mk_dot(scr, 248);
    s_dot[1] = mk_dot(scr, 268);
    s_dot[2] = mk_dot(scr, 288);

    s_page[PAGE_HOME] = mk_page(scr);
    s_home_line[HOME_WIFI] = mk_label(s_page[PAGE_HOME], &lv_font_montserrat_14, COL_CYAN, 12, 8);
    s_home_line[HOME_TCP] = mk_label(s_page[PAGE_HOME], &lv_font_montserrat_14, COL_WHITE, 12, 38);
    s_home_line[HOME_DATA] = mk_label(s_page[PAGE_HOME], &lv_font_montserrat_14, COL_WHITE, 12, 68);
    s_home_line[HOME_SD] = mk_label(s_page[PAGE_HOME], &lv_font_montserrat_14, COL_WHITE, 12, 98);
    label_set(s_home_line[HOME_WIFI], "WiFi  idle");
    label_set(s_home_line[HOME_TCP], "TCP   idle");
    label_set(s_home_line[HOME_DATA], "DATA  stop");
    label_set(s_home_line[HOME_SD], "SD    unmount");

    s_page[PAGE_SCAN] = mk_page(scr);
    s_scan_empty = mk_label(s_page[PAGE_SCAN], &lv_font_montserrat_12, COL_GRAY, 12, 4);
    for(i = 0; i < c_wifi_ap_max; i++)
        s_scan_line[i] = mk_label(s_page[PAGE_SCAN], &lv_font_montserrat_12, COL_WHITE, 12, (lv_coord_t)(8 + i * 14));

    s_page[PAGE_STA] = mk_page(scr);
    s_sta_ssid_lab = mk_label(s_page[PAGE_STA], &lv_font_montserrat_14, COL_WHITE, 12, 8);
    s_sta_st_lab = mk_label(s_page[PAGE_STA], &lv_font_montserrat_14, COL_WHITE, 12, 36);
    s_sta_ip_lab = mk_label(s_page[PAGE_STA], &lv_font_montserrat_14, COL_WHITE, 12, 64);
    s_sta_hint = mk_label(s_page[PAGE_STA], &lv_font_montserrat_12, COL_GRAY, 12, 100);

    s_page[PAGE_DATA] = mk_page(scr);
    for(i = 0; i < 6; i++)
        s_data_lab[i] = mk_label(s_page[PAGE_DATA], &lv_font_montserrat_14, COL_WHITE, 12, (lv_coord_t)(8 + i * 24));

    s_page[PAGE_TCP] = mk_page(scr);
    for(i = 0; i < 5; i++)
        s_tcp_lab[i] = mk_label(s_page[PAGE_TCP], &lv_font_montserrat_14, COL_WHITE, 12, (lv_coord_t)(8 + i * 24));

    bar = lv_obj_create(scr);
    lv_obj_set_size(bar, LCD_W, 28);
    lv_obj_align(bar, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(bar, lv_color_hex(COL_DARK), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(bar, 0, 0);
    lv_obj_set_style_radius(bar, 0, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);
    s_hint = lv_label_create(bar);
    lv_obj_set_style_text_color(s_hint, lv_color_hex(0xC0C0C0), 0);
    lv_obj_set_style_text_font(s_hint, &lv_font_montserrat_12, 0);
    lv_obj_center(s_hint);

    ui_show_page(PAGE_HOME);
}

static unsigned int lvgl_port_disp_init(void)
{
    void *buf1;
    unsigned int rows, px;
    const unsigned int try_rows[] = {LVGL_DRAW_ROWS, 20, 10};

    buf1 = NULL;
    rows = 0;
    for(unsigned int i = 0; i < (sizeof(try_rows) / sizeof(try_rows[0])); i++)
    {
        rows = try_rows[i];
        px = LCD_W * rows;
        buf1 = bsp_alloc(px * sizeof(lv_color_t));
        if(buf1 != NULL)
            break;
    }
    if(buf1 == NULL)
    {
        dbgtx("lvgl draw buf alloc fail\r\n");
        return c_ret_nk;
    }

    px = LCD_W * rows;
    lv_disp_draw_buf_init(&s_disp_buf, buf1, NULL, px);
    lv_disp_drv_init(&s_disp_drv);
    s_disp_drv.hor_res = LCD_W;
    s_disp_drv.ver_res = LCD_H;
    s_disp_drv.flush_cb = lvgl_disp_flush_cb;
    s_disp_drv.draw_buf = &s_disp_buf;
    lv_disp_drv_register(&s_disp_drv);
    dbgtx("lvgl draw buf rows=%u bytes=%u\r\n", rows, px * (unsigned int)sizeof(lv_color_t));
    return c_ret_ok;
}

void task_lvgl_proc(void)
{
    if(s_lvgl_ok == 0)
        return;
    if(tick_cmp(s_refresh_tmr, LVGL_REFRESH_MS) == c_ret_ok)
    {
        s_refresh_tmr = tick_get();
        ui_refresh();
    }
    if(tick_cmp(sys.curr_task->tmr, LVGL_HANDLER_MS) != c_ret_ok)
        return;
    sys.curr_task->tmr = tick_get();
    lv_timer_handler();
    sys.curr_task->done = c_done_wt;
}

void task_lvgl_init(void)
{
    const esp_timer_create_args_t tick_args = {
        .callback = &increase_lvgl_tick,
        .name = "lvgl_tick"
    };
    esp_timer_handle_t tick_tmr = NULL;

    if(SPI_LCD_MODE != 1)
    {
        dbgtx("lvgl skip: need SPI_LCD_MODE=1\r\n");
        return;
    }

    lv_init();
    if(lvgl_port_disp_init() != c_ret_ok)
        return;

    ESP_ERROR_CHECK(esp_timer_create(&tick_args, &tick_tmr));
    ESP_ERROR_CHECK(esp_timer_start_periodic(tick_tmr, 1000));

    lvgl_ui_create();
    s_refresh_tmr = tick_get();
    s_lvgl_ok = 1;
    msg_add("task_lvgl_msg", task_lvgl_msg_parse);
    task_add("task_lvgl", task_lvgl_proc, c_auto_wait, 0);
    dbgtx("lvgl ui ready pages=5\r\n");
}
INIT_REG(task_lvgl_init, 7);

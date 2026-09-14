#include "user_ext.h"
#include "esp_attr.h"
#include "task_tcp.h"
#include "task_xl9555.h"

/* 对齐原扫描: idle -> 消抖20ms -> 等全部松开再上报; 长按看按下到松开是否>=1s */
#define c_key_step_idle     0
#define c_key_step_db       1
#define c_key_step_free     2

static uint16_t s_port = 0xffff;
static unsigned int s_scan_tmr = 0;
static unsigned int key_scan_seq = 0;
static unsigned int key_scan_val = 0;

static void key_short_evt(unsigned char id);
static void key_long_evt(unsigned char id);

static void key_wifi_scan(void)
{
    dbgtx("key wifi scan\r\n");
    msg_send("task_wifi_msg", "wifi_scan");
    msg_send("task_wifi_msg", "setup");
}

static void key_sd_ensure(void)
{
    if(task_get("task_sd") != NULL)
        return;
    dbgtx("key sd setup\r\n");
    msg_send("task_sd_msg", "setup");
}

static void key_data_toggle(void)
{
    if(task_get("task_data") != NULL)
    {
        dbgtx("key data stop\r\n");
        msg_send("task_data_msg", "stop");
        return;
    }
    dbgtx("key data setup\r\n");
    key_sd_ensure();
    msg_send("task_data_msg", "setup");
}

static void key_tcp_toggle(void)
{
    char buf[64];

    if(task_get("task_tcp") != NULL)
    {
        dbgtx("key tcp stop\r\n");
        msg_send("task_tcp_msg", "stop");
        return;
    }
    snprintf(buf, sizeof(buf), "tcp:ip=%s,port=%u", c_tcp_ip, c_tcp_port);
    dbgtx("key tcp setup %s:%u\r\n", c_tcp_ip, c_tcp_port);
    key_sd_ensure();
    msg_send("task_tcp_msg", buf);
    msg_send("task_tcp_msg", "setup");
}

static void key_short_evt(unsigned char id)
{
    static const char *nav[4] = { "ok", "up", "down", "back" };

    if(id >= 4)
        return;
    msg_send("task_lvgl_msg", (char *)nav[id]);
}

static void key_long_evt(unsigned char id)
{
    if(id == 0)
        key_data_toggle();
    else if(id == 1)
    {
        key_wifi_scan();
        msg_send("task_lvgl_msg", "page:scan");
    }
    else if(id == 2)
    {
        key_tcp_toggle();
        msg_send("task_lvgl_msg", "page:tcp");
    }
    else
        msg_send("task_lvgl_msg", "page:home");
}

/* XL9555 KEY0~3 -> bit0~3, 低有效按下 */
static unsigned int key_tmp_get(void)
{
    unsigned int key_tmp = 0;

    if((s_port & 0x8000) == 0) key_tmp |= c_key_val_0;
    if((s_port & 0x4000) == 0) key_tmp |= c_key_val_1;
    if((s_port & 0x2000) == 0) key_tmp |= c_key_val_2;
    if((s_port & 0x1000) == 0) key_tmp |= c_key_val_3;
    return key_tmp;
}

static unsigned char key_val_to_id(unsigned int val)
{
    if(val == c_key_val_0) return 0;
    if(val == c_key_val_1) return 1;
    if(val == c_key_val_2) return 2;
    if(val == c_key_val_3) return 3;
    return 0xff;
}

static void key_scan(void)
{
    static unsigned int step = c_key_step_idle, tmr = 0, key_val = 0;
    unsigned int key_tmp;
    uint16_t port;
    static uint16_t port_bk = 0;

    if(tick_cmp(s_scan_tmr, c_key_scan_ms) == c_ret_ok)
    {
        s_scan_tmr = tick_get();
        if(xl9555_input_get(&port) == c_ret_ok)
        {
            s_port = port;
            port &= 0xF000;
            if(port != port_bk)
            {
                port_bk = port;
                dbgtx("key port k0-3=%u%u%u%u\r\n",
                      (port & 0x8000) ? 1u : 0u,
                      (port & 0x4000) ? 1u : 0u,
                      (port & 0x2000) ? 1u : 0u,
                      (port & 0x1000) ? 1u : 0u);
            }
        }
    }

    key_tmp = key_tmp_get();

    switch(step)
    {
        case c_key_step_idle:
            if(key_tmp == 0)
                break;
            key_val = key_tmp;
            tmr = tick_get();
            step = c_key_step_db;
        break;

        case c_key_step_db:
            if(tick_cmp(tmr, c_key_db_ms) == c_ret_nk)
                break;
            if(key_tmp == key_val)
            {
                step = c_key_step_free;
                break;
            }
            step = c_key_step_idle;
        break;

        case c_key_step_free:
            if(key_tmp != 0)
            {
                if(key_tmp == key_val)
                    break;
                key_val = 0;                    /* 单键: 中途变键则作废 */
                break;
            }
            if(key_val != 0)
            {
                if(tick_cmp(tmr, c_key_long_ms) == c_ret_ok)
                    key_scan_val = key_val | c_key_val_long;
                else
                    key_scan_val = key_val;
                key_scan_seq++;
            }
            step = c_key_step_idle;
        break;

        default:
            step = c_key_step_idle;
        break;
    }
}

unsigned int task_key_msg_parse(char *buf)
{
    unsigned int id;

    if(buf == NULL)
        return c_ret_nk;

    if(strncmp(buf, "short:", 6) == 0)
    {
        id = (unsigned int)(buf[6] - '0');
        if(id >= 4)
            return c_ret_nk;
        key_short_evt((unsigned char)id);
        return c_ret_ok;
    }
    if(strncmp(buf, "long:", 5) == 0)
    {
        id = (unsigned int)(buf[5] - '0');
        if(id >= 4)
            return c_ret_nk;
        key_long_evt((unsigned char)id);
        return c_ret_ok;
    }
    return c_ret_nk;
}

static volatile unsigned int  boot_down_tick = 0;
static volatile unsigned char boot_short = 0;
static volatile unsigned char boot_long  = 0;

static void IRAM_ATTR boot_isr(void *arg)
{
    unsigned int now = (unsigned int)(xTaskGetTickCountFromISR() * portTICK_PERIOD_MS);

    if(gpio_get_level(BOOT_GPIO_PIN) == KEY_PRESS_LEVEL)
    {
        boot_down_tick = now;
    }
    else
    {
        if((now - boot_down_tick) >= c_key_long_ms)
            boot_long = 1;
        else
            boot_short = 1;
    }
}

void key_proc(void)
{
    static unsigned int seq_bk = 0;
    unsigned char id;

    key_scan();

    if(seq_bk != key_scan_seq)
    {
        seq_bk = key_scan_seq;
        id = key_val_to_id(key_scan_val & ~c_key_val_long);
        if(id < 4)
        {
            if(key_scan_val & c_key_val_long)
            {
                dbgtx("key%u long\r\n", (unsigned int)id);
                key_long_evt(id);
            }
            else
            {
                dbgtx("key%u short\r\n", (unsigned int)id);
                key_short_evt(id);
            }
        }
    }

    if(boot_short)
    {
        boot_short = 0;
        LED0_TOGGLE();
    }
    if(boot_long)
    {
        boot_long = 0;
        LED0_TOGGLE();
    }
}

void key_init(void)
{
    gpio_config_t gpio_init_struct = {0};

    gpio_init_struct.intr_type = GPIO_INTR_ANYEDGE;
    gpio_init_struct.mode = GPIO_MODE_INPUT;
    gpio_init_struct.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_init_struct.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_init_struct.pin_bit_mask = (1ULL << BOOT_GPIO_PIN);
    ESP_ERROR_CHECK(gpio_config(&gpio_init_struct));

    gpio_install_isr_service(0);
    gpio_isr_handler_add(BOOT_GPIO_PIN, boot_isr, NULL);

    msg_add("task_key_msg", task_key_msg_parse);
    task_add("task_key", key_proc, c_auto_quit, 0);
    dbgtx("key map K0 short=ok long=data\r\n");
    dbgtx("key map K1 short=up long=wifi scan\r\n");
    dbgtx("key map K2 short=down long=tcp\r\n");
    dbgtx("key map K3 short=back long=home\r\n");
}
INIT_REG(key_init, 5);

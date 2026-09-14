#include "user_ext.h"
#include "task_lcd.h"
#include "spilcdfont.h"

/* 软/硬 SPI 只改 spi.h 的 SPI_LCD_MODE, 本文件按该宏切两套驱动 */

struct st_task_lcd_set task_lcd_set = {0};

/* exe=dbg_msg_send(msg:task_lcd_msg,dat:help)
 * exe=dbg_msg_send(msg:task_lcd_msg,dat:init)
 * exe=dbg_msg_send(msg:task_lcd_msg,dat:fill:x=0,y=0,w=320,h=240,color=0xf800)
 * exe=dbg_msg_send(msg:task_lcd_msg,dat:pixel:x=10,y=10,color=0xffff)
 * exe=dbg_msg_send(msg:task_lcd_msg,dat:rect:x=10,y=10,w=50,h=30,color=0x07e0)
 * exe=dbg_msg_send(msg:task_lcd_msg,dat:bl:1)
 * exe=dbg_msg_send(msg:task_lcd_msg,dat:char:x=10,y=10,ch=A,size=16,color=0xffff,bk=0x0000)
 * exe=dbg_msg_send(msg:task_lcd_msg,dat:str:x=10,y=30,size=16,color=0xffff,bk=0x0000,txt=hello)
 * exe=dbg_msg_send(msg:task_lcd_msg,dat:str:x=10,y=30,size=16,color=0xffff,bk=0x0000,txt=hello world)
 * exe=dbg_msg_send(msg:task_lcd_msg,dat:setup_done)
 * exe=dbg_msg_send(msg:task_lcd_msg,dat:stop)
 * 可连续发多条fill/str后再setup_done, 一次任务按序执行
 */
static unsigned int lcd_set_push(struct st_lcd_arg *p_arg)
{
    if(p_arg == NULL) return c_ret_nk;
    if(task_lcd_set.set_cnt >= c_lcd_cmd_max)
    {
        dbgtx("lcd set full, cnt=%u\r\n", task_lcd_set.set_cnt);
        return c_ret_nk;
    }
    task_lcd_set.set[task_lcd_set.set_cnt] = *p_arg;
    task_lcd_set.set_cnt++;
    return c_ret_ok;
}

unsigned int task_lcd_msg_parse(char *buf)
{
    unsigned int val;
    struct st_lcd_arg arg;

    if(buf == NULL) return c_ret_nk;

    if(strcmp(buf, "help") == 0)
    {
        dbgtx("init\r\n");
        dbgtx("fill:x=ddd,y=ddd,w=ddd,h=ddd,color=hhhh\r\n");
        dbgtx("pixel:x=ddd,y=ddd,color=hhhh\r\n");
        dbgtx("rect:x=ddd,y=ddd,w=ddd,h=ddd,color=hhhh\r\n");
        dbgtx("bl:0/1\r\n");
        dbgtx("char:x=ddd,y=ddd,ch=c,size=12/16/24/32,color=hhhh,bk=hhhh\r\n");
        dbgtx("str:x=ddd,y=ddd,size=12/16/24/32,color=hhhh,bk=hhhh,txt=abc def\r\n");
        dbgtx("setup_done  (after one or more cmds)\r\n");
        dbgtx("stop\r\n");
        return c_ret_ok;
    }

    memset(&arg, 0, sizeof(arg));
    if(strcmp(buf, "init") == 0)
    {
        arg.cmd = c_lcd_cmd_init;
        return lcd_set_push(&arg);
    }
    else if(sscanf(buf, "fill:x=%d,y=%d,w=%d,h=%d,color=%x", &arg.x, &arg.y, &arg.w, &arg.h, &arg.color) == 5)
    {
        arg.cmd = c_lcd_cmd_fill;
        return lcd_set_push(&arg);
    }
    else if(sscanf(buf, "pixel:x=%d,y=%d,color=%x", &arg.x, &arg.y, &arg.color) == 3)
    {
        arg.cmd = c_lcd_cmd_pixel;
        return lcd_set_push(&arg);
    }
    else if(sscanf(buf, "rect:x=%d,y=%d,w=%d,h=%d,color=%x", &arg.x, &arg.y, &arg.w, &arg.h, &arg.color) == 5)
    {
        arg.cmd = c_lcd_cmd_rect;
        return lcd_set_push(&arg);
    }
    else if(sscanf(buf, "bl:%d", &val) == 1)
    {
        arg.cmd = c_lcd_cmd_bl;
        arg.val = val;
        return lcd_set_push(&arg);
    }
    else if(sscanf(buf, "char:x=%d,y=%d,ch=%c,size=%d,color=%x,bk=%x", &arg.x, &arg.y, &arg.ch, &arg.size, &arg.color, &arg.bk_color) == 6)
    {
        arg.cmd = c_lcd_cmd_char;
        return lcd_set_push(&arg);
    }
    else if(sscanf(buf, "str:x=%d,y=%d,size=%d,color=%x,bk=%x,txt=%31[^\n]", &arg.x, &arg.y, &arg.size, &arg.color, &arg.bk_color, arg.str) == 6)
    {
        arg.cmd = c_lcd_cmd_str;
        dbgtx("lcd str: x=%u y=%u size=%u color=%x bk=%x txt=%s\r\n", arg.x, arg.y, arg.size, arg.color, arg.bk_color, arg.str);
        return lcd_set_push(&arg);
    }
    else if(strcmp(buf, "setup_done") == 0)
    {
        struct st_task *p_lcd;

        /* 任务在跑也提交: proc下一拍丢弃旧帧改画本批, 避免 connecting/成功叠屏 */
        task_lcd_set.setup_done_req_seq++;
        p_lcd = task_get("task_lcd");
        if((p_lcd == NULL) || (p_lcd->done != c_done_wt))
            task_add("task_lcd", task_lcd_proc, c_auto_quit, sizeof(struct st_task_lcd_run));
        return c_ret_ok;
    }
    else if(strcmp(buf, "stop") == 0)
    {
        if(task_get("task_lcd") == NULL) return c_ret_nk;
        task_lcd_set.stop_req_seq++;
        return c_ret_ok;
    }
    return c_ret_nk;
}

/* ============ 共用: 取字库 ============ */
static const unsigned char *lcd_get_font(char ch, unsigned int size)
{
    if((ch < ' ') || (ch > '~'))
        ch = ' ';

    if(size == 12)
        return asc2_1206[ch - ' '];
    else if(size == 16)
        return asc2_1608[ch - ' '];
    else if(size == 24)
        return asc2_2412[ch - ' '];
    else if(size == 32)
        return asc2_3216[ch - ' '];
    return asc2_1608[ch - ' '];
}

#if SPI_LCD_MODE == 0

/* ======================================================================
 * 模块: 软件SPI驱动 (SPI_LCD_MODE=0时编译本模块)
 * SCL/SDA 由 spi_init 配成 GPIO, CS/DC 见 task_lcd.h
 * ====================================================================== */

static void lcd_bus_take(void)
{
    spi_tf_cs_h();                  /* 访问LCD前释放TF卡片选 */
}

/* CS保持低, 连续发命令/数据, 减少片选开关 */
static void lcd_write_cmd(unsigned char cmd)
{
    lcd_bus_take();
    lcd_cs_l();
    lcd_dc_l();
    spi_write_byte(cmd);
    lcd_cs_h();
}

static void lcd_write_dat(unsigned char dat)
{
    lcd_bus_take();
    lcd_cs_l();
    lcd_dc_h();
    spi_write_byte(dat);
    lcd_cs_h();
}

/* CS已拉低: 开窗并切到写GRAM, 返回时CS仍低、DC高, 可直接发像素 */
static void lcd_addr_begin(unsigned int x0, unsigned int y0, unsigned int x1, unsigned int y1)
{
    lcd_bus_take();
    lcd_cs_l();

    lcd_dc_l();
    spi_write_byte(0x2a);
    lcd_dc_h();
    spi_write_byte((unsigned char)(x0 >> 8));
    spi_write_byte((unsigned char)x0);
    spi_write_byte((unsigned char)(x1 >> 8));
    spi_write_byte((unsigned char)x1);

    lcd_dc_l();
    spi_write_byte(0x2b);
    lcd_dc_h();
    spi_write_byte((unsigned char)(y0 >> 8));
    spi_write_byte((unsigned char)y0);
    spi_write_byte((unsigned char)(y1 >> 8));
    spi_write_byte((unsigned char)y1);

    lcd_dc_l();
    spi_write_byte(0x2c);
    lcd_dc_h();
}

static void lcd_addr_end(void)
{
    lcd_cs_h();
}

/* ---------- 填充区域 ---------- */
void lcd_fill(unsigned int x, unsigned int y, unsigned int w, unsigned int h, unsigned int color)
{
    unsigned int cnt;

    if((x >= LCD_W) || (y >= LCD_H) || (w == 0) || (h == 0)) return;
    if((x + w) > LCD_W) w = LCD_W - x;
    if((y + h) > LCD_H) h = LCD_H - y;

    lcd_addr_begin(x, y, x + w - 1, y + h - 1);
    cnt = w * h;
    spi_write_pixels(color, cnt);       /* IRAM dedic GPIO: 纯色只打SCL */
    lcd_addr_end();
}

/* ---------- 画点 ---------- */
void lcd_pixel(unsigned int x, unsigned int y, unsigned int color)
{
    lcd_fill(x, y, 1, 1, color);
}

/* ---------- 显示一个字符 (先构建整字符像素缓冲, 批量发送) ---------- */
void lcd_show_char(unsigned int x, unsigned int y, char ch, unsigned int size, unsigned int color, unsigned int bk_color)
{
    unsigned int row, col, w, byte_w, pix;
    const unsigned char *font;
    unsigned char dat[16 * 32 * 2];     /* 最大32号字: 16x32像素x2字节 */

    if(size == 0) size = 16;
    w = size / 2;
    byte_w = w / 8;
    if(w % 8) byte_w++;
    font = lcd_get_font(ch, size);

    pix = 0;
    for(row = 0; row < size; row++)
    {
        for(col = 0; col < w; col++)
        {
            if(font[row * byte_w + col / 8] & (0x80 >> (col % 8)))
            {
                dat[pix++] = color >> 8;    /* 高字节在前 */
                dat[pix++] = color;
            }
            else
            {
                dat[pix++] = bk_color >> 8;
                dat[pix++] = bk_color;
            }
        }
    }

    lcd_addr_begin(x, y, x + w - 1, y + size - 1);
    spi_write_buf(dat, pix);
    lcd_addr_end();
}

void lcd_show_string(unsigned int x, unsigned int y, char *str, unsigned int size, unsigned int color, unsigned int bk_color)
{
    unsigned int w, n, i, remain, row, col, byte_w, dst_w, pix;
    const unsigned char *font;
    unsigned char *buf;
    unsigned char ch, ch_hi, ch_lo, bk_hi, bk_lo;

    if(str == NULL) return;
    if(size == 0) size = 16;
    w = size / 2;
    byte_w = w / 8;
    if(w % 8) byte_w++;
    ch_hi = (unsigned char)(color >> 8);
    ch_lo = (unsigned char)color;
    bk_hi = (unsigned char)(bk_color >> 8);
    bk_lo = (unsigned char)bk_color;

    while(*str)
    {
        if((x + w) > LCD_W)
        {
            x = 0;
            y += size;
        }
        if((y + size) > LCD_H)
            return;

        remain = (LCD_W - x) / w;
        if(remain == 0)
            continue;

        n = 0;
        while((str[n] != 0) && (n < remain))
            n++;
        if(n == 0)
            break;

        dst_w = n * w;
        pix = dst_w * size * 2;
        buf = bsp_alloc(pix);
        if(buf == NULL)
        {
            lcd_show_char(x, y, *str++, size, color, bk_color);
            x += w;
            continue;
        }

        for(i = 0; i < n; i++)
        {
            ch = (unsigned char)str[i];
            font = lcd_get_font((char)ch, size);
            for(row = 0; row < size; row++)
            {
                for(col = 0; col < w; col++)
                {
                    unsigned int off = (row * dst_w + i * w + col) * 2;
                    if(font[row * byte_w + col / 8] & (0x80 >> (col % 8)))
                    {
                        buf[off] = ch_hi;
                        buf[off + 1] = ch_lo;
                    }
                    else
                    {
                        buf[off] = bk_hi;
                        buf[off + 1] = bk_lo;
                    }
                }
            }
        }
        lcd_addr_begin(x, y, x + dst_w - 1, y + size - 1);
        spi_write_buf(buf, pix);
        lcd_addr_end();
        bsp_free(buf);

        str += n;
        x += dst_w;
    }
}

/* ---------- LCD引脚初始化 (DC/CS, SCL/SDA由spi_init配置) ---------- */
static void lcd_hw_init(void)
{
    gpio_config_t gpio_init_struct = {0};

    gpio_init_struct.intr_type = GPIO_INTR_DISABLE;
    gpio_init_struct.mode = GPIO_MODE_OUTPUT;
    gpio_init_struct.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_init_struct.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_init_struct.pin_bit_mask = (1ULL << LCD_DC_GPIO_PIN) |
                                    (1ULL << LCD_CS_GPIO_PIN);
    ESP_ERROR_CHECK(gpio_config(&gpio_init_struct));

    lcd_dc_h();
    lcd_cs_h();
}

/* ---------- LCD驱动初始化 (ST7789V寄存器序列) ---------- */
void lcd_drv_init(void)
{
    lcd_hw_init();
    lcd_power_h();
    dly_ms(10);

    lcd_rst_l();
    dly_ms(10);
    lcd_rst_h();
    dly_ms(10);

    lcd_write_cmd(0x11);                /* 退出睡眠 */
    dly_ms(120);

    lcd_write_cmd(0x36);
    lcd_write_dat(0x60);                /* 扫描方向: 2.4寸横屏(320x240), MV|MX */

    lcd_write_cmd(0x3a);
    lcd_write_dat(0x05);                /* RGB565 16bit */

    lcd_write_cmd(0xb2);
    lcd_write_dat(0x0c);
    lcd_write_dat(0x0c);
    lcd_write_dat(0x00);
    lcd_write_dat(0x33);
    lcd_write_dat(0x33);

    lcd_write_cmd(0xb7);
    lcd_write_dat(0x35);

    lcd_write_cmd(0xbb);
    lcd_write_dat(0x1f);

    lcd_write_cmd(0xc0);
    lcd_write_dat(0x2c);

    lcd_write_cmd(0xc2);
    lcd_write_dat(0x01);
    lcd_write_dat(0xc3);

    lcd_write_cmd(0xc3);
    lcd_write_dat(0x13);

    lcd_write_cmd(0xc4);
    lcd_write_dat(0x20);

    lcd_write_cmd(0xc6);
    lcd_write_dat(0x0f);

    lcd_write_cmd(0xd0);
    lcd_write_dat(0xa4);
    lcd_write_dat(0xa1);

    lcd_write_cmd(0xe0);                /* 正gamma */
    lcd_write_dat(0xd0);
    lcd_write_dat(0x08);
    lcd_write_dat(0x11);
    lcd_write_dat(0x08);
    lcd_write_dat(0x0c);
    lcd_write_dat(0x15);
    lcd_write_dat(0x39);
    lcd_write_dat(0x33);
    lcd_write_dat(0x50);
    lcd_write_dat(0x36);
    lcd_write_dat(0x13);
    lcd_write_dat(0x14);
    lcd_write_dat(0x29);
    lcd_write_dat(0x2d);

    lcd_write_cmd(0xe1);                /* 负gamma */
    lcd_write_dat(0xd0);
    lcd_write_dat(0x08);
    lcd_write_dat(0x10);
    lcd_write_dat(0x08);
    lcd_write_dat(0x06);
    lcd_write_dat(0x06);
    lcd_write_dat(0x39);
    lcd_write_dat(0x44);
    lcd_write_dat(0x51);
    lcd_write_dat(0x0b);
    lcd_write_dat(0x16);
    lcd_write_dat(0x14);
    lcd_write_dat(0x2f);
    lcd_write_dat(0x31);

    lcd_write_cmd(0x21);                /* 反显开启 */
    lcd_write_cmd(0x29);                /* 开显示 */
    dly_ms(10);

    lcd_fill(0, 0, LCD_W, LCD_H, BLACK);/* 清屏 */
    dbgtx("lcd drv init ok (SPI_LCD_MODE=0 soft gpio)\r\n");
}

void lcd_draw_bitmap_wait(int x1, int y1, int x2, int y2, const void *color)
{
    (void)x1;
    (void)y1;
    (void)x2;
    (void)y2;
    (void)color;
}

#endif /* SPI_LCD_MODE == 0 */

#if SPI_LCD_MODE == 1

/* ======================================================================
 * 模块: 硬件SPI驱动 (SPI_LCD_MODE=1时编译本模块)
 * SPI2_HOST + esp_lcd面板驱动, 80MHz+DMA, DC/CS由驱动自动控制
 * 注意: ESP32-S3 SPI源是80MHz APB, 写60MHz会落到40MHz; 只有 >60MHz 才走80MHz sysclk
 * ====================================================================== */
#include "driver/spi_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_panel_vendor.h"
#include "freertos/semphr.h"
#include "soc/spi_struct.h"

static esp_lcd_panel_io_handle_t lcd_io_handle = NULL;
static esp_lcd_panel_handle_t    lcd_panel_handle = NULL;

/* DMA异步传输完成信号量 (tx_color为排队传输, 必须等DMA读完缓冲才能free)
 * 任务在Take时完全挂起, 由DMA完成回调GiveFromISR唤醒, 不忙等不浪费CPU
 */
static SemaphoreHandle_t lcd_flush_sem = NULL;
static unsigned char lcd_clk_dumped = 0;

static bool lcd_notify_flush_ready(esp_lcd_panel_io_handle_t panel_io, esp_lcd_panel_io_event_data_t *edata, void *user_ctx)
{
    BaseType_t need_yield = pdFALSE;

    xSemaphoreGiveFromISR(lcd_flush_sem, &need_yield);
    return (need_yield == pdTRUE);
}

/* 等待当前一次draw_bitmap的DMA传输完成 (200ms超时防回调丢失卡死) */
static void lcd_draw_wait(void)
{
    xSemaphoreTake(lcd_flush_sem, pdMS_TO_TICKS(200));
}

/* LVGL flush / 其它 DMA 提交: x2/y2 为不含终点, 与 esp_lcd_panel_draw_bitmap 一致 */
void lcd_draw_bitmap_wait(int x1, int y1, int x2, int y2, const void *color)
{
    if((lcd_panel_handle == NULL) || (color == NULL))
        return;
    if((x2 <= x1) || (y2 <= y1))
        return;
    esp_lcd_panel_draw_bitmap(lcd_panel_handle, x1, y1, x2, y2, color);
    lcd_draw_wait();
}

/* DMA刚结束时读SPI2时钟寄存器, 看实际分频(clk_equ=1 即为APB 80MHz) */
static void lcd_spi_hw_clk_dump(void)
{
    unsigned int equ = GPSPI2.clock.clk_equ_sysclk;
    unsigned int n = GPSPI2.clock.clkcnt_n;
    unsigned int pre = GPSPI2.clock.clkdiv_pre;
    unsigned int freq;

    if(equ)
        freq = 80 * 1000 * 1000;
    else
        freq = 80 * 1000 * 1000 / (pre + 1) / (n + 1);
    dbgtx("lcd spi2 hw clk_equ=%u pre=%u n=%u freq=%uHz clkreg=0x%08x\r\n",
          equ, pre, n, freq, (unsigned int)GPSPI2.clock.val);
}

/* ---------- 填充区域 (优先120行一批; 内存不够再退回40行) ---------- */
void lcd_fill(unsigned int x, unsigned int y, unsigned int w, unsigned int h, unsigned int color)
{
    unsigned int i, yb, hh, rows, nfill;
    uint16_t *buf;
    uint16_t color_tmp;

    if((x >= LCD_W) || (y >= LCD_H) || (w == 0) || (h == 0)) return;
    if((x + w) > LCD_W) w = LCD_W - x;
    if((y + h) > LCD_H) h = LCD_H - y;

    rows = 120;
    buf = bsp_alloc(w * rows * sizeof(uint16_t));
    if(buf == NULL)
    {
        rows = 40;
        buf = bsp_alloc(w * rows * sizeof(uint16_t));
        if(buf == NULL) return;
    }

    color_tmp = (uint16_t)(((color & 0x00FF) << 8) | ((color & 0xFF00) >> 8));
    nfill = w * rows;
    for(i = 0; i < nfill; i++) buf[i] = color_tmp;

    for(yb = 0; yb < h; yb += rows)
    {
        hh = (h - yb > rows) ? rows : (h - yb);
        esp_lcd_panel_draw_bitmap(lcd_panel_handle, x, y + yb, x + w, y + yb + hh, buf);
        lcd_draw_wait();
        if((yb == 0) && (lcd_clk_dumped == 0))
        {
            lcd_clk_dumped = 1;
            lcd_spi_hw_clk_dump();
        }
    }
    bsp_free(buf);
}

/* ---------- 画点 ---------- */
void lcd_pixel(unsigned int x, unsigned int y, unsigned int color)
{
    uint16_t color_tmp = (uint16_t)(((color & 0x00FF) << 8) | ((color & 0xFF00) >> 8));
    esp_lcd_panel_draw_bitmap(lcd_panel_handle, x, y, x + 1, y + 1, &color_tmp);
    lcd_draw_wait();
}

/* ---------- 显示一个字符 (整帧一次提交, 缓冲用内存池) ---------- */
void lcd_show_char(unsigned int x, unsigned int y, char ch, unsigned int size, unsigned int color, unsigned int bk_color)
{
    unsigned int row, col, w, byte_w, pix;
    const unsigned char *font;
    uint16_t *buf;
    uint16_t ct, bt;

    if(size == 0) size = 16;
    w = size / 2;
    byte_w = w / 8;
    if(w % 8) byte_w++;
    font = lcd_get_font(ch, size);

    buf = bsp_alloc(w * size * sizeof(uint16_t));   /* 内存池, 最大1024B */
    if(buf == NULL) return;

    ct = (uint16_t)(((color & 0x00FF) << 8) | ((color & 0xFF00) >> 8));
    bt = (uint16_t)(((bk_color & 0x00FF) << 8) | ((bk_color & 0xFF00) >> 8));

    pix = 0;
    for(row = 0; row < size; row++)
    {
        for(col = 0; col < w; col++)
        {
            if(font[row * byte_w + col / 8] & (0x80 >> (col % 8)))
                buf[pix++] = ct;
            else
                buf[pix++] = bt;
        }
    }
    esp_lcd_panel_draw_bitmap(lcd_panel_handle, x, y, x + w, y + size, buf);
    lcd_draw_wait();                    /* 等DMA读完缓冲再free, 防乱码 */
    bsp_free(buf);
}

void lcd_show_string(unsigned int x, unsigned int y, char *str, unsigned int size, unsigned int color, unsigned int bk_color)
{
    unsigned int w, n, i, remain, row, col, byte_w, dst_w;
    const unsigned char *font;
    uint16_t *buf;
    uint16_t ct, bt;
    char ch;

    if(str == NULL) return;
    if(size == 0) size = 16;
    w = size / 2;
    byte_w = w / 8;
    if(w % 8) byte_w++;
    ct = (uint16_t)(((color & 0x00FF) << 8) | ((color & 0xFF00) >> 8));
    bt = (uint16_t)(((bk_color & 0x00FF) << 8) | ((bk_color & 0xFF00) >> 8));

    while(*str)
    {
        if((x + w) > LCD_W)
        {
            x = 0;
            y += size;
        }
        if((y + size) > LCD_H)
            return;

        remain = (LCD_W - x) / w;
        if(remain == 0)
            continue;

        n = 0;
        while((str[n] != 0) && (n < remain))
            n++;
        if(n == 0)
            break;

        dst_w = n * w;
        buf = bsp_alloc(dst_w * size * sizeof(uint16_t));
        if(buf == NULL)
            return;

        for(i = 0; i < n; i++)
        {
            ch = str[i];
            font = lcd_get_font(ch, size);
            for(row = 0; row < size; row++)
            {
                for(col = 0; col < w; col++)
                {
                    if(font[row * byte_w + col / 8] & (0x80 >> (col % 8)))
                        buf[row * dst_w + i * w + col] = ct;
                    else
                        buf[row * dst_w + i * w + col] = bt;
                }
            }
        }
        esp_lcd_panel_draw_bitmap(lcd_panel_handle, x, y, x + dst_w, y + size, buf);
        lcd_draw_wait();
        bsp_free(buf);

        str += n;
        x += dst_w;
    }
}

/* ---------- LCD驱动初始化 (esp_lcd面板API, SPI2 已由 spi_init 挂好) ---------- */
void lcd_drv_init(void)
{
    uint8_t madctl;

    if(lcd_flush_sem == NULL)
        lcd_flush_sem = xSemaphoreCreateBinary();       /* 重复init不重复创建 */

    lcd_power_h();
    dly_ms(10);

    lcd_rst_l();
    dly_ms(10);
    lcd_rst_h();
    dly_ms(10);

    /* 面板IO只能挂一次: 重复init若再次new_panel_io会abort, 屏端MADCTL(含0xA0)会一直残留 */
    if(lcd_panel_handle == NULL)
    {
        esp_lcd_panel_io_spi_config_t io_config = {0};
        esp_lcd_panel_dev_config_t panel_config = {0};

        io_config.dc_gpio_num = LCD_DC_GPIO_PIN;
        io_config.cs_gpio_num = LCD_CS_GPIO_PIN;
        io_config.pclk_hz = 80 * 1000 * 1000;       /* 80MHz; 写60MHz实际只有40MHz */
        io_config.lcd_cmd_bits = 8;
        io_config.lcd_param_bits = 8;
        io_config.spi_mode = 0;
        io_config.trans_queue_depth = 7;
        ESP_ERROR_CHECK(esp_lcd_new_panel_io_spi((esp_lcd_spi_bus_handle_t)LCD_HOST, &io_config, &lcd_io_handle));
        /* APB=80MHz: 请求>60MHz才走 sysclk=80MHz; 写60MHz会落到40MHz */
        dbgtx("lcd spi pclk req=%uHz actual=80MHz\r\n", (unsigned int)io_config.pclk_hz);

        const esp_lcd_panel_io_callbacks_t cbs = {
            .on_color_trans_done = lcd_notify_flush_ready,
        };
        ESP_ERROR_CHECK(esp_lcd_panel_io_register_event_callbacks(lcd_io_handle, &cbs, NULL));

        panel_config.reset_gpio_num = -1;           /* RST由XL9555控制 */
        panel_config.rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB;
        panel_config.bits_per_pixel = 16;
        panel_config.data_endian = LCD_RGB_DATA_ENDIAN_BIG;
        ESP_ERROR_CHECK(esp_lcd_new_panel_st7789(lcd_io_handle, &panel_config, &lcd_panel_handle));
    }

    ESP_ERROR_CHECK(esp_lcd_panel_reset(lcd_panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_init(lcd_panel_handle));
    ESP_ERROR_CHECK(esp_lcd_panel_invert_color(lcd_panel_handle, true));  /* 反显, 同0x21; 必须在init/SLPOUT之后 */
    ESP_ERROR_CHECK(esp_lcd_panel_disp_on_off(lcd_panel_handle, true));

    ESP_ERROR_CHECK(esp_lcd_panel_swap_xy(lcd_panel_handle, true));      /* 横屏320x240 */
    ESP_ERROR_CHECK(esp_lcd_panel_mirror(lcd_panel_handle, true, false));

    madctl = 0x60;      /* 与软件模式一致: MV|MX, 强制写屏端寄存器, 清掉上次0xA0残留 */
    ESP_ERROR_CHECK(esp_lcd_panel_io_tx_param(lcd_io_handle, 0x36, &madctl, 1));
    lcd_fill(0, 0, LCD_W, LCD_H, BLACK);        /* 清屏 */
    dbgtx("lcd drv init ok (SPI_LCD_MODE=1 hw spi2)\r\n");
}

#endif /* SPI_LCD_MODE == 1 */

/* ======================================================================
 * LCD任务状态机 (在初始化上面)
 * ====================================================================== */
static void lcd_cmd_exec(struct st_lcd_arg *p)
{
    if(p == NULL) return;

    if(p->cmd == c_lcd_cmd_init)
        lcd_drv_init();
    else if(p->cmd == c_lcd_cmd_fill)
        lcd_fill(p->x, p->y, p->w, p->h, p->color);
    else if(p->cmd == c_lcd_cmd_pixel)
        lcd_pixel(p->x, p->y, p->color);
    else if(p->cmd == c_lcd_cmd_rect)
    {
        lcd_fill(p->x, p->y, p->w, 1, p->color);
        lcd_fill(p->x, p->y + p->h - 1, p->w, 1, p->color);
        lcd_fill(p->x, p->y, 1, p->h, p->color);
        lcd_fill(p->x + p->w - 1, p->y, 1, p->h, p->color);
    }
    else if(p->cmd == c_lcd_cmd_bl)
    {
        if(p->val)
            lcd_power_h();
        else
            lcd_power_l();
    }
    else if(p->cmd == c_lcd_cmd_char)
        lcd_show_char(p->x, p->y, p->ch, p->size, p->color, p->bk_color);
    else if(p->cmd == c_lcd_cmd_str)
        lcd_show_string(p->x, p->y, p->str, p->size, p->color, p->bk_color);
}

#if SPI_LCD_MODE == 1
/* 一批 fill/str 先画进内存, 再按120行DMA, 避免每条字符串各开一次窗 */
#define c_lcd_stripe_rows   120

static uint16_t lcd_to_dma_color(unsigned int color)
{
    return (uint16_t)(((color & 0x00FF) << 8) | ((color & 0xFF00) >> 8));
}

static unsigned int lcd_is_draw_cmd(unsigned int cmd)
{
    return (cmd == c_lcd_cmd_fill) || (cmd == c_lcd_cmd_pixel) ||
           (cmd == c_lcd_cmd_rect) || (cmd == c_lcd_cmd_char) ||
           (cmd == c_lcd_cmd_str);
}

static void lcd_bbox_add(int *x0, int *y0, int *x1, int *y1, int x, int y, int w, int h)
{
    if((w <= 0) || (h <= 0)) return;
    if(x < 0) { w += x; x = 0; }
    if(y < 0) { h += y; y = 0; }
    if((x + w) > LCD_W) w = LCD_W - x;
    if((y + h) > LCD_H) h = LCD_H - y;
    if((w <= 0) || (h <= 0)) return;
    if(x < *x0) *x0 = x;
    if(y < *y0) *y0 = y;
    if((x + w) > *x1) *x1 = x + w;
    if((y + h) > *y1) *y1 = y + h;
}

static void lcd_str_bbox(int *x0, int *y0, int *x1, int *y1,
                         int x, int y, unsigned int size, const char *str)
{
    int w, remain, n;

    if((str == NULL) || (size == 0)) return;
    w = (int)(size / 2);
    if(w <= 0) return;

    while(*str)
    {
        if((x + w) > LCD_W)
        {
            x = 0;
            y += (int)size;
        }
        if((y + (int)size) > LCD_H)
            return;
        remain = (LCD_W - x) / w;
        if(remain <= 0)
        {
            x = 0;
            y += (int)size;
            continue;
        }
        n = 0;
        while((str[n] != 0) && (n < remain))
            n++;
        if(n == 0)
            return;
        lcd_bbox_add(x0, y0, x1, y1, x, y, n * w, (int)size);
        str += n;
        x += n * w;
    }
}

static void lcd_buf_rect(uint16_t *buf, unsigned int bw, unsigned int bh,
                         int bx, int by, int x, int y, int w, int h, uint16_t c)
{
    int x0, y0, x1, y1, row, col;

    x0 = x;
    y0 = y;
    x1 = x + w;
    y1 = y + h;
    if(x0 < bx) x0 = bx;
    if(y0 < by) y0 = by;
    if(x1 > (int)(bx + bw)) x1 = (int)(bx + bw);
    if(y1 > (int)(by + bh)) y1 = (int)(by + bh);
    if((x1 <= x0) || (y1 <= y0)) return;

    for(row = y0; row < y1; row++)
    {
        for(col = x0; col < x1; col++)
            buf[(unsigned int)(row - by) * bw + (unsigned int)(col - bx)] = c;
    }
}

static void lcd_buf_string(uint16_t *buf, unsigned int bw, unsigned int bh,
                           int bx, int by, int x, int y, unsigned int size,
                           const char *str, uint16_t ct, uint16_t bt)
{
    unsigned int w, byte_w, remain, n, i, row, col;
    const unsigned char *font;
    int px, py, sx, sy;

    if((str == NULL) || (size == 0)) return;
    w = size / 2;
    byte_w = w / 8;
    if(w % 8) byte_w++;

    while(*str)
    {
        if((x + (int)w) > LCD_W)
        {
            x = 0;
            y += (int)size;
        }
        if((y + (int)size) > LCD_H)
            return;
        remain = (LCD_W - x) / (int)w;
        if(remain == 0)
        {
            x = 0;
            y += (int)size;
            continue;
        }
        n = 0;
        while((str[n] != 0) && (n < remain))
            n++;
        if(n == 0)
            return;

        for(i = 0; i < n; i++)
        {
            font = lcd_get_font(str[i], size);
            px = x + (int)(i * w);
            py = y;
            for(row = 0; row < size; row++)
            {
                sy = py + (int)row;
                if((sy < by) || (sy >= (int)(by + bh)))
                    continue;
                for(col = 0; col < w; col++)
                {
                    sx = px + (int)col;
                    if((sx < bx) || (sx >= (int)(bx + bw)))
                        continue;
                    if(font[row * byte_w + col / 8] & (0x80 >> (col % 8)))
                        buf[(unsigned int)(sy - by) * bw + (unsigned int)(sx - bx)] = ct;
                    else
                        buf[(unsigned int)(sy - by) * bw + (unsigned int)(sx - bx)] = bt;
                }
            }
        }
        str += n;
        x += (int)(n * w);
    }
}

static void lcd_compose_apply(uint16_t *buf, unsigned int bw, unsigned int bh,
                              int bx, int by, struct st_lcd_arg *p)
{
    uint16_t c, bt;
    unsigned int size;
    char tmp[2];

    if(p == NULL) return;
    c = lcd_to_dma_color(p->color);
    bt = lcd_to_dma_color(p->bk_color);
    size = p->size;
    if(size == 0) size = 16;

    if(p->cmd == c_lcd_cmd_fill)
        lcd_buf_rect(buf, bw, bh, bx, by, (int)p->x, (int)p->y, (int)p->w, (int)p->h, c);
    else if(p->cmd == c_lcd_cmd_pixel)
        lcd_buf_rect(buf, bw, bh, bx, by, (int)p->x, (int)p->y, 1, 1, c);
    else if(p->cmd == c_lcd_cmd_rect)
    {
        lcd_buf_rect(buf, bw, bh, bx, by, (int)p->x, (int)p->y, (int)p->w, 1, c);
        lcd_buf_rect(buf, bw, bh, bx, by, (int)p->x, (int)(p->y + p->h - 1), (int)p->w, 1, c);
        lcd_buf_rect(buf, bw, bh, bx, by, (int)p->x, (int)p->y, 1, (int)p->h, c);
        lcd_buf_rect(buf, bw, bh, bx, by, (int)(p->x + p->w - 1), (int)p->y, 1, (int)p->h, c);
    }
    else if(p->cmd == c_lcd_cmd_char)
    {
        tmp[0] = p->ch;
        tmp[1] = 0;
        lcd_buf_string(buf, bw, bh, bx, by, (int)p->x, (int)p->y, size, tmp, c, bt);
    }
    else if(p->cmd == c_lcd_cmd_str)
        lcd_buf_string(buf, bw, bh, bx, by, (int)p->x, (int)p->y, size, p->str, c, bt);
}

static void lcd_compose_group(struct st_lcd_arg *cmds, unsigned int cnt)
{
    unsigned int i, has_fill, bw, bh, hh, yb, covered, nfill;
    int x0, y0, x1, y1;
    uint16_t *buf;
    uint16_t full_c;

    if((cmds == NULL) || (cnt == 0)) return;

    has_fill = 0;
    x0 = LCD_W;
    y0 = LCD_H;
    x1 = 0;
    y1 = 0;
    for(i = 0; i < cnt; i++)
    {
        if(cmds[i].cmd == c_lcd_cmd_fill)
        {
            has_fill = 1;
            lcd_bbox_add(&x0, &y0, &x1, &y1, (int)cmds[i].x, (int)cmds[i].y, (int)cmds[i].w, (int)cmds[i].h);
        }
        else if(cmds[i].cmd == c_lcd_cmd_pixel)
            lcd_bbox_add(&x0, &y0, &x1, &y1, (int)cmds[i].x, (int)cmds[i].y, 1, 1);
        else if(cmds[i].cmd == c_lcd_cmd_rect)
            lcd_bbox_add(&x0, &y0, &x1, &y1, (int)cmds[i].x, (int)cmds[i].y, (int)cmds[i].w, (int)cmds[i].h);
        else if(cmds[i].cmd == c_lcd_cmd_char)
            lcd_bbox_add(&x0, &y0, &x1, &y1, (int)cmds[i].x, (int)cmds[i].y, (int)((cmds[i].size ? cmds[i].size : 16) / 2), (int)(cmds[i].size ? cmds[i].size : 16));
        else if(cmds[i].cmd == c_lcd_cmd_str)
            lcd_str_bbox(&x0, &y0, &x1, &y1, (int)cmds[i].x, (int)cmds[i].y, cmds[i].size ? cmds[i].size : 16, cmds[i].str);
    }

    /* 没有fill时合成会把间隙刷成黑, 仍走原来的逐条DMA */
    if((has_fill == 0) || (x1 <= x0) || (y1 <= y0))
    {
        for(i = 0; i < cnt; i++)
            lcd_cmd_exec(&cmds[i]);
        return;
    }

    bw = (unsigned int)(x1 - x0);
    for(yb = (unsigned int)y0; yb < (unsigned int)y1; yb += c_lcd_stripe_rows)
    {
        hh = (unsigned int)y1 - yb;
        if(hh > c_lcd_stripe_rows)
            hh = c_lcd_stripe_rows;
        bh = hh;
        buf = bsp_alloc(bw * bh * sizeof(uint16_t));
        if(buf == NULL)
        {
            for(i = 0; i < cnt; i++)
                lcd_cmd_exec(&cmds[i]);
            return;
        }
        /* 整条带被fill盖住: 顺序填色(同lcd_fill), 不再memset+逐点 */
        full_c = 0;
        covered = 0;
        for(i = 0; i < cnt; i++)
        {
            if((cmds[i].cmd == c_lcd_cmd_fill) &&
               ((int)cmds[i].x <= x0) && ((int)cmds[i].y <= (int)yb) &&
               ((int)(cmds[i].x + cmds[i].w) >= x1) &&
               ((int)(cmds[i].y + cmds[i].h) >= (int)(yb + hh)))
            {
                full_c = lcd_to_dma_color(cmds[i].color);
                covered = 1;
            }
        }
        nfill = bw * bh;
        if(covered)
        {
            for(i = 0; i < nfill; i++)
                buf[i] = full_c;
        }
        else
            memset(buf, 0, nfill * sizeof(uint16_t));
        for(i = 0; i < cnt; i++)
        {
            if(covered && (cmds[i].cmd == c_lcd_cmd_fill) &&
               ((int)cmds[i].x <= x0) && ((int)cmds[i].y <= (int)yb) &&
               ((int)(cmds[i].x + cmds[i].w) >= x1) &&
               ((int)(cmds[i].y + cmds[i].h) >= (int)(yb + hh)))
                continue;
            lcd_compose_apply(buf, bw, bh, x0, (int)yb, &cmds[i]);
        }
        esp_lcd_panel_draw_bitmap(lcd_panel_handle, x0, (int)yb, x1, (int)(yb + hh), buf);
        lcd_draw_wait();
        bsp_free(buf);
    }
}
#endif /* SPI_LCD_MODE == 1 */

static void lcd_flush_batch(struct st_lcd_arg *cmds, unsigned int cnt)
{
    unsigned int i;

    if((cmds == NULL) || (cnt == 0)) return;

#if SPI_LCD_MODE == 1
    unsigned int j;

    i = 0;
    while(i < cnt)
    {
        if(lcd_is_draw_cmd(cmds[i].cmd) == 0)
        {
            lcd_cmd_exec(&cmds[i]);
            i++;
            continue;
        }
        j = i + 1;
        while((j < cnt) && lcd_is_draw_cmd(cmds[j].cmd))
            j++;
        lcd_compose_group(&cmds[i], j - i);
        i = j;
    }
#else
    for(i = 0; i < cnt; i++)
        lcd_cmd_exec(&cmds[i]);
#endif
}

void task_lcd_proc(void)
{
    #define c_step_idle             0
    #define c_step_run              1
    struct st_task_lcd_run  *p_task_arg;

    if(task_lcd_set.stop_req_seq_bk != task_lcd_set.stop_req_seq)
    {
        task_lcd_set.stop_req_seq_bk = task_lcd_set.stop_req_seq;
        sys.curr_task->step = c_step_idle;
        sys.curr_task->done = c_done_nk;
        return;
    }

    p_task_arg = (struct st_task_lcd_run *)sys.curr_task->dat;

    /* 新setup_done随时切帧: 正在画 connecting 时成功/失败画面直接覆盖 */
    if(task_lcd_set.setup_done_req_seq_bk != task_lcd_set.setup_done_req_seq)
    {
        task_lcd_set.setup_done_req_seq_bk = task_lcd_set.setup_done_req_seq;
        p_task_arg->run_cnt = task_lcd_set.set_cnt;
        p_task_arg->run_idx = 0;
        if(p_task_arg->run_cnt > c_lcd_cmd_max)
            p_task_arg->run_cnt = c_lcd_cmd_max;
        if(p_task_arg->run_cnt != 0)
            memcpy(p_task_arg->run, task_lcd_set.set, p_task_arg->run_cnt * sizeof(struct st_lcd_arg));
        task_lcd_set.set_cnt = 0;
        sys.curr_task->step = c_step_run;
        sys.curr_task->done = c_done_wt;
    }

    switch(sys.curr_task->step)
    {
        case c_step_idle:
            return;

        case c_step_run:
            lcd_flush_batch(p_task_arg->run, p_task_arg->run_cnt);
            p_task_arg->run_idx = p_task_arg->run_cnt;
            sys.curr_task->step = c_step_idle;
            sys.curr_task->done = c_done_ok;
        break;

        default: sys.curr_task->step = c_step_idle; break;
    }
}

/* ======================================================================
 * LCD初始化注册 (在最下面)
 * ====================================================================== */
void task_lcd_init(void)
{
    lcd_drv_init();
    msg_add("task_lcd_msg", task_lcd_msg_parse);
}
INIT_REG(task_lcd_init, 6);     /* 优先级6: 必须晚于xl9555_init(4), 电源/复位由XL9555控制 */
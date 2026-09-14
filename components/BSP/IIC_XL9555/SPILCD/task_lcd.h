#ifndef __TASK_LCD_H
#define __TASK_LCD_H

#include "task_xl9555.h"
#include "spi.h"
#include "soc/gpio_struct.h"

/* 硬件SPI模式使用的SPI外设 (spi.h 里 SPI_LCD_MODE=1 时) */
#define LCD_HOST            SPI2_HOST

/* ============ LCD引脚定义 (正点原子DNESP32S3板, 2.4寸SPILCD模块) ============
 * 注意: DC=IO40需在P5端口用跳线帽把IO_SET与LCD_DC短接
 * SCL/SDA/MISO 见 spi.h (SPI2 硬件总线, 与TF共用)
 */
#define LCD_DC_GPIO_PIN     GPIO_NUM_40     /* 命令/数据选择 */
#define LCD_CS_GPIO_PIN     GPIO_NUM_21     /* 片选 */

/* 电源/复位由XL9555控制 (P1.3=PWR高有效亮屏, P1.2=RST低有效复位)
 * 注意: 2.4寸模块PWR为高有效, 与官方例程LCD_PWR(1)=上电一致 */
#define lcd_power_h()       xl9555_pin_write(SLCD_PWR_IO, 1)
#define lcd_power_l()       xl9555_pin_write(SLCD_PWR_IO, 0)
#define lcd_rst_h()         xl9555_pin_write(SLCD_RST_IO, 1)
#define lcd_rst_l()         xl9555_pin_write(SLCD_RST_IO, 0)

/* 寄存器直写(OUT_W1TS/W1TC原子置位/清零), 比gpio_set_level快一个数量级
 * 注意: ESP32-S3的GPIO32~53走独立的out1_w1ts/out1_w1tc高寄存器, 不能写低寄存器
 */
#define lcd_cs_l()          (GPIO.out_w1tc = (1u << LCD_CS_GPIO_PIN))          /* GPIO21<32: 低寄存器 */
#define lcd_cs_h()          (GPIO.out_w1ts = (1u << LCD_CS_GPIO_PIN))
#define lcd_dc_l()          (GPIO.out1_w1tc.val = (1u << (LCD_DC_GPIO_PIN - 32)))   /* GPIO40>=32: 高寄存器 */
#define lcd_dc_h()          (GPIO.out1_w1ts.val = (1u << (LCD_DC_GPIO_PIN - 32)))

/* 屏幕分辨率 (ST7789V 2.4寸 320x240 横屏) */
#define LCD_W               320
#define LCD_H               240

/* 常用颜色值 (RGB565) */
#define WHITE               0xFFFF
#define BLACK               0x0000
#define RED                 0xF800
#define GREEN               0x07E0
#define BLUE                0x001F
#define YELLOW              0xFFE0
#define CYAN                0x07FF
#define MAGENTA             0xF81F

/* 命令定义 */
#define c_lcd_cmd_none      0
#define c_lcd_cmd_init      1
#define c_lcd_cmd_fill      2
#define c_lcd_cmd_pixel     3
#define c_lcd_cmd_rect      4
#define c_lcd_cmd_bl        5
#define c_lcd_cmd_char      6
#define c_lcd_cmd_str       7

#define c_lcd_cmd_max       16          /* 一次setup可排队的命令数(清屏+标题+12个AP) */

struct st_lcd_arg
{
    unsigned int cmd;
    unsigned int x;
    unsigned int y;
    unsigned int w;
    unsigned int h;
    unsigned int color;
    unsigned int bk_color;
    unsigned int val;
    unsigned int size;
    char         ch;
    char         str[32];
};

struct st_task_lcd_run
{
    struct st_lcd_arg   run[c_lcd_cmd_max];
    unsigned int        run_cnt;
    unsigned int        run_idx;
};

struct st_task_lcd_set
{
    struct st_lcd_arg   set[c_lcd_cmd_max];
    unsigned int        set_cnt;
    unsigned int        setup_done_req_seq;
    unsigned int        setup_done_req_seq_bk;
    unsigned int        stop_req_seq;
    unsigned int        stop_req_seq_bk;
};

extern struct st_task_lcd_set task_lcd_set;

/* 函数声明 */
void lcd_drv_init(void);                                /* LCD驱动初始化序列 */
void lcd_draw_bitmap_wait(int x1, int y1, int x2, int y2, const void *color); /* LVGL flush: 打点并等DMA */
void lcd_fill(unsigned int x, unsigned int y, unsigned int w, unsigned int h, unsigned int color);   /* 填充区域 */
void lcd_pixel(unsigned int x, unsigned int y, unsigned int color);                                   /* 画点 */
void lcd_show_char(unsigned int x, unsigned int y, char ch, unsigned int size, unsigned int color, unsigned int bk_color);      /* 显示字符 */
void lcd_show_string(unsigned int x, unsigned int y, char *str, unsigned int size, unsigned int color, unsigned int bk_color);   /* 显示字符串 */

unsigned int task_lcd_msg_parse(char *buf);             /* LCD串口命令解析 */
void task_lcd_proc(void);                               /* LCD任务: 由task_proc周期调用 */
void task_lcd_init(void);                               /* LCD初始化注册 */

#endif
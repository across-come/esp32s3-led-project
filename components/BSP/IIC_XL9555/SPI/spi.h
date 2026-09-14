#ifndef __SPI_H
#define __SPI_H

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "soc/gpio_struct.h"

/* ============ LCD/SPI 唯一开关: 改这一处再编译即可 ============
 * 1 = 硬件SPI2 + esp_lcd (DMA)
 * 0 = 软件GPIO位敲LCD (ESP32-S3 Dedicated GPIO)
 */
#define SPI_LCD_MODE        1

/* ============ 引脚 (LCD 与 TF 共用 SCK/MOSI) ============ */
#define SPI_SCL_GPIO_PIN    GPIO_NUM_12     /* 时钟 */
#define SPI_SDA_GPIO_PIN    GPIO_NUM_11     /* MOSI */
#define SPI_MISO_GPIO_PIN   GPIO_NUM_13     /* MISO, 仅TF; LCD不接 */
#define SPI_TF_CS_GPIO_PIN  GPIO_NUM_2      /* TF片选, 软件LCD时须拉高 */
#define SPI_HOST            SPI2_HOST

#if SPI_LCD_MODE == 0
#define spi_miso()          ((GPIO.in >> SPI_MISO_GPIO_PIN) & 1u)
#define spi_tf_cs_h()       (GPIO.out_w1ts = (1u << SPI_TF_CS_GPIO_PIN))

void spi_write_byte(unsigned char dat);                         /* 只发不收, 命令/单字节 */
void spi_write_pixels(unsigned int color, unsigned int cnt);    /* 连续发cnt个RGB565像素 */
void spi_write_buf(const unsigned char *p, unsigned int n);     /* 连续发n字节 */
unsigned char spi_rw_byte(unsigned char dat);                   /* 全双工 */
void spi_soft_init(void);
#endif

void spi_init(void);                            /* 按 SPI_LCD_MODE 初始化总线或GPIO */

#endif

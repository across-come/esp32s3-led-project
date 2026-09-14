#include "user_ext.h"
#include "esp_attr.h"
#include "spi.h"

#if SPI_LCD_MODE == 0
#include "driver/dedic_gpio.h"
#include "hal/dedic_gpio_cpu_ll.h"

/* ======================================================================
 * 软件SPI: ESP32-S3 Dedicated GPIO (ee.wr_mask_gpio_out, CPU时钟)
 * 通道: gpio_array[0]=MOSI(SDA) [1]=SCL, 绑定到调用 spi_init 的核 (app_main/CPU0)
 *
 * 优化过程 (下面 #if 0 是走过的APB GPIO写法, 现网用 Dedicated GPIO)
 *   1. 用户原版: for 8次, spi_scl_l/h、spi_sda_h/l = GPIO.out_w1tc/w1ts (APB 80MHz)
 *   2. 展开8次, 去掉循环, 仍写同一组APB GPIO寄存器
 *   3. 纯色填0x0000/0xFFFF只拧SCL + IRAM + -O2, 仍是APB GPIO
 *   4. 现网: Dedicated GPIO CPU指令, 不再写 GPIO.out_w1ts
 * ====================================================================== */

#if 0
/* ---------- 1. 用户原版: 对APB上的GPIO寄存器操作, 每个边沿一次外设写 ---------- */
#define spi_scl_h()         (GPIO.out_w1ts = (1u << SPI_SCL_GPIO_PIN))
#define spi_scl_l()         (GPIO.out_w1tc = (1u << SPI_SCL_GPIO_PIN))
#define spi_sda_h()         (GPIO.out_w1ts = (1u << SPI_SDA_GPIO_PIN))
#define spi_sda_l()         (GPIO.out_w1tc = (1u << SPI_SDA_GPIO_PIN))

void spi_write_byte(unsigned char dat)
{
    unsigned int i;

    for(i = 0; i < 8; i++)
    {
        spi_scl_l();
        if(dat & 0x80)
            spi_sda_h();
        else
            spi_sda_l();
        dat <<= 1;
        spi_scl_h();
    }
    spi_scl_l();
}

/* ---------- 2. 循环展开, 仍写 GPIO.out_w1ts/w1tc ---------- */
void spi_write_byte(unsigned char dat)
{
    #define SPI_BIT_WRITE() do { \
        spi_scl_l(); \
        if(dat & 0x80) spi_sda_h(); else spi_sda_l(); \
        dat <<= 1; \
        spi_scl_h(); \
    } while(0)

    SPI_BIT_WRITE();
    SPI_BIT_WRITE();
    SPI_BIT_WRITE();
    SPI_BIT_WRITE();
    SPI_BIT_WRITE();
    SPI_BIT_WRITE();
    SPI_BIT_WRITE();
    SPI_BIT_WRITE();
    #undef SPI_BIT_WRITE
    spi_scl_l();
}

/* ---------- 3. 纯色像素只打SCL, 热路径IRAM, 仍是APB GPIO寄存器 ---------- */
void spi_write_pixels(unsigned int color, unsigned int cnt)
{
    unsigned char hi = (unsigned char)(color >> 8);
    unsigned char lo = (unsigned char)color;
    unsigned int i;

    if((hi == 0) && (lo == 0))
    {
        spi_sda_l();
        while(cnt--)
        {
            for(i = 0; i < 16; i++)
            {
                spi_scl_l();
                spi_scl_h();
            }
        }
        spi_scl_l();
        return;
    }
    while(cnt--)
    {
        spi_write_byte(hi);
        spi_write_byte(lo);
    }
}
#endif

static uint32_t s_sda;      /* dedic 通道位: MOSI */
static uint32_t s_scl;      /* dedic 通道位: SCL */
static uint32_t s_both;

#define SPI_CLK1(scl)       do { \
    dedic_gpio_cpu_ll_write_mask((scl), 0); \
    dedic_gpio_cpu_ll_write_mask((scl), (scl)); \
} while(0)

#define SPI_CLK8(scl)       do { \
    SPI_CLK1(scl); SPI_CLK1(scl); SPI_CLK1(scl); SPI_CLK1(scl); \
    SPI_CLK1(scl); SPI_CLK1(scl); SPI_CLK1(scl); SPI_CLK1(scl); \
} while(0)

#define SPI_CLK16(scl)      do { SPI_CLK8(scl); SPI_CLK8(scl); } while(0)

#define SPI_BIT8(dat, sda, scl, both) do { \
    unsigned char _d = (dat); \
    uint32_t _v; \
    _v = (_d & 0x80) ? (sda) : 0; dedic_gpio_cpu_ll_write_mask((both), _v); dedic_gpio_cpu_ll_write_mask((both), _v | (scl)); _d = (unsigned char)(_d << 1); \
    _v = (_d & 0x80) ? (sda) : 0; dedic_gpio_cpu_ll_write_mask((both), _v); dedic_gpio_cpu_ll_write_mask((both), _v | (scl)); _d = (unsigned char)(_d << 1); \
    _v = (_d & 0x80) ? (sda) : 0; dedic_gpio_cpu_ll_write_mask((both), _v); dedic_gpio_cpu_ll_write_mask((both), _v | (scl)); _d = (unsigned char)(_d << 1); \
    _v = (_d & 0x80) ? (sda) : 0; dedic_gpio_cpu_ll_write_mask((both), _v); dedic_gpio_cpu_ll_write_mask((both), _v | (scl)); _d = (unsigned char)(_d << 1); \
    _v = (_d & 0x80) ? (sda) : 0; dedic_gpio_cpu_ll_write_mask((both), _v); dedic_gpio_cpu_ll_write_mask((both), _v | (scl)); _d = (unsigned char)(_d << 1); \
    _v = (_d & 0x80) ? (sda) : 0; dedic_gpio_cpu_ll_write_mask((both), _v); dedic_gpio_cpu_ll_write_mask((both), _v | (scl)); _d = (unsigned char)(_d << 1); \
    _v = (_d & 0x80) ? (sda) : 0; dedic_gpio_cpu_ll_write_mask((both), _v); dedic_gpio_cpu_ll_write_mask((both), _v | (scl)); _d = (unsigned char)(_d << 1); \
    _v = (_d & 0x80) ? (sda) : 0; dedic_gpio_cpu_ll_write_mask((both), _v); dedic_gpio_cpu_ll_write_mask((both), _v | (scl)); \
} while(0)

void IRAM_ATTR __attribute__((optimize("O2"))) spi_write_byte(unsigned char dat)
{
    uint32_t sda = s_sda;
    uint32_t scl = s_scl;
    uint32_t both = s_both;

    SPI_BIT8(dat, sda, scl, both);
    dedic_gpio_cpu_ll_write_mask(scl, 0);
}

void IRAM_ATTR __attribute__((optimize("O2"))) spi_write_pixels(unsigned int color, unsigned int cnt)
{
    unsigned char hi = (unsigned char)(color >> 8);
    unsigned char lo = (unsigned char)color;
    uint32_t sda = s_sda;
    uint32_t scl = s_scl;
    uint32_t both = s_both;

    if((hi == 0) && (lo == 0))
    {
        dedic_gpio_cpu_ll_write_mask(sda, 0);
        while(cnt--)
            SPI_CLK16(scl);
        dedic_gpio_cpu_ll_write_mask(scl, 0);
        return;
    }
    if((hi == 0xFF) && (lo == 0xFF))
    {
        dedic_gpio_cpu_ll_write_mask(sda, sda);
        while(cnt--)
            SPI_CLK16(scl);
        dedic_gpio_cpu_ll_write_mask(scl, 0);
        return;
    }

    while(cnt--)
    {
        SPI_BIT8(hi, sda, scl, both);
        SPI_BIT8(lo, sda, scl, both);
    }
    dedic_gpio_cpu_ll_write_mask(scl, 0);
}

void IRAM_ATTR __attribute__((optimize("O2"))) spi_write_buf(const unsigned char *p, unsigned int n)
{
    uint32_t sda = s_sda;
    uint32_t scl = s_scl;
    uint32_t both = s_both;

    while(n--)
    {
        SPI_BIT8(*p, sda, scl, both);
        p++;
    }
    dedic_gpio_cpu_ll_write_mask(scl, 0);
}

unsigned char spi_rw_byte(unsigned char dat)
{
    unsigned char r = 0;
    unsigned int i;
    uint32_t sda = s_sda;
    uint32_t scl = s_scl;
    uint32_t both = s_both;
    uint32_t v;

    for(i = 0; i < 8; i++)
    {
        v = (dat & 0x80) ? sda : 0;
        dedic_gpio_cpu_ll_write_mask(both, v);
        dedic_gpio_cpu_ll_write_mask(both, v | scl);
        r = (unsigned char)((r << 1) | spi_miso());
        dat = (unsigned char)(dat << 1);
    }
    dedic_gpio_cpu_ll_write_mask(scl, 0);
    return r;
}

void spi_soft_init(void)
{
    gpio_config_t gpio_init_struct = {0};
    dedic_gpio_bundle_config_t bundle_cfg = {0};
    dedic_gpio_bundle_handle_t bundle = NULL;
    const int pins[2] = { SPI_SDA_GPIO_PIN, SPI_SCL_GPIO_PIN };
    uint32_t off = 0;

    gpio_init_struct.intr_type = GPIO_INTR_DISABLE;
    gpio_init_struct.mode = GPIO_MODE_OUTPUT;
    gpio_init_struct.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_init_struct.pull_down_en = GPIO_PULLDOWN_DISABLE;
    gpio_init_struct.pin_bit_mask = (1ULL << SPI_SCL_GPIO_PIN) |
                                    (1ULL << SPI_SDA_GPIO_PIN) |
                                    (1ULL << SPI_TF_CS_GPIO_PIN);
    ESP_ERROR_CHECK(gpio_config(&gpio_init_struct));
    gpio_set_drive_capability(SPI_SCL_GPIO_PIN, GPIO_DRIVE_CAP_3);
    gpio_set_drive_capability(SPI_SDA_GPIO_PIN, GPIO_DRIVE_CAP_3);

    gpio_init_struct.mode = GPIO_MODE_INPUT;
    gpio_init_struct.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_init_struct.pin_bit_mask = (1ULL << SPI_MISO_GPIO_PIN);
    ESP_ERROR_CHECK(gpio_config(&gpio_init_struct));

    bundle_cfg.gpio_array = pins;
    bundle_cfg.array_size = 2;
    bundle_cfg.flags.out_en = 1;
    ESP_ERROR_CHECK(dedic_gpio_new_bundle(&bundle_cfg, &bundle));
    ESP_ERROR_CHECK(dedic_gpio_get_out_offset(bundle, &off));
    s_sda = (1u << off);
    s_scl = (1u << (off + 1));
    s_both = s_sda | s_scl;

    dedic_gpio_cpu_ll_write_mask(s_both, s_sda);    /* SCL低空闲, MOSI高 */
    spi_tf_cs_h();
}

void spi_init(void)
{
    spi_soft_init();
    dbgtx("spi soft dedic init ok sck=%d mosi=%d tf_cs=%d (SPI_LCD_MODE=0)\r\n",
          SPI_SCL_GPIO_PIN, SPI_SDA_GPIO_PIN, SPI_TF_CS_GPIO_PIN);
}

#else /* SPI_LCD_MODE == 1 */
/* ======================================================================
 * 硬件SPI2: 只 initialize 总线, 不 add_device
 *   LCD 由 esp_lcd_new_panel_io_spi 挂 CS=IO21
 *   TF  由 esp_vfs_fat_sdspi_mount 挂 CS=IO2
 * ====================================================================== */
void spi_init(void)
{
    spi_bus_config_t buscfg = {0};

    buscfg.sclk_io_num = SPI_SCL_GPIO_PIN;
    buscfg.mosi_io_num = SPI_SDA_GPIO_PIN;
    buscfg.miso_io_num = SPI_MISO_GPIO_PIN;
    buscfg.quadwp_io_num = -1;
    buscfg.quadhd_io_num = -1;
    buscfg.max_transfer_sz = 320 * 240 * sizeof(uint16_t);

    ESP_ERROR_CHECK(spi_bus_initialize(SPI_HOST, &buscfg, SPI_DMA_CH_AUTO));
    dbgtx("spi2 bus init ok sck=%d mosi=%d miso=%d (SPI_LCD_MODE=1)\r\n",
          SPI_SCL_GPIO_PIN, SPI_SDA_GPIO_PIN, SPI_MISO_GPIO_PIN);
}
#endif /* SPI_LCD_MODE */

INIT_REG(spi_init, 2);      /* 优先级2: 先于 LCD/SD 挂设备 */

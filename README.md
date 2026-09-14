# ESP32-S3 LED 显示项目 (ESP-IDF + LVGL)

基于 **ESP32-S3 + ESP-IDF v5.x + LVGL 8.3** 的嵌入式 GUI 显示项目，包含完整的 BSP 驱动层、多任务架构与上位机串口调试脚本，适合作为嵌入式岗位面试作品展示。

## 硬件平台

- 主控：ESP32-S3 (内置 512KB SRAM，外挂 8MB PSRAM / 16MB Flash)
- 显示：SPI LCD 屏 + LVGL 8.3 图形库 (RGB565 16bit)
- 外设：LED、按键 (KEY)、IO 扩展芯片 XL9555 (IIC)、EEPROM AT24C02 (IIC)、SD 卡 (SDIO)、LEDC PWM
- 存储分区：FAT vfs (10MB) + SPIFFS storage (4MB)

## 软件架构

```
main/
├── main.c              # app_main 入口
components/
├── BSP/                # 板级驱动与业务任务
│   ├── LED/            # LED 控制任务 (task_led)
│   ├── KEY/            # 按键扫描任务 (task_key)
│   ├── LVGL/           # LVGL UI 任务 (task_lvgl)
│   ├── WIFI/           # WiFi STA + TCP Server/Client + UDP (task_tcp/tcps/udp/wifi)
│   ├── SDIO/           # SD 卡读写任务 (task_sd)
│   ├── IIC_XL9555/     # XL9555 IO 扩展、AT24C02、SPI LCD
│   ├── LEDC/           # PWM 驱动
│   ├── TIMG/           # 定时器/看门狗
│   ├── STORE/          # 缓存存储 (cache_store)
│   ├── DATA/           # 数据任务 (task_data)
│   └── user_bsp.c      # BSP 初始化框架
└── LVGL/               # LVGL 8.3 源码库
```

### 架构亮点

- **构造函数式初始化注册**：通过 `__attribute__((constructor))` + `init_register()` 实现模块按优先级自动注册、`user_init()` 统一遍历执行 (见 `components/BSP/user_bsp.h`)
- **静态内存池**：128KB 静态数组内存池 (16B 块粒度)，避开 PSRAM 非 DMA 可达限制
- **多任务协作**：LED / KEY / LVGL / WiFi / SD 各模块独立 FreeRTOS 任务
- **中断保护**：`int_dis()/int_en()` 中断计数保护机制
- **高精度计时**：基于 `esp_timer` 的 us 级延时与看门狗 (TIMG)

## 上位机调试脚本 (Python)

`com11_*.py` 为基于串口 (COM11) 的 PC 端测试脚本，用于：

- `com11_lvgl_p2/p3.py` — LVGL 交互测试
- `com11_lcd_ack.py` / `com11_lcd_soft_scan.py` — LCD 通信测试
- `com11_sta_tcp.py` / `com11_tcp_loop.py` / `com11_tcp_switch.py` — WiFi/TCP 测试
- `com11_sd_test.py` / `com11_sd_replay.py` — SD 卡读写测试
- `com11_offline_check.py` / `com11_hw_offline30.py` — 硬件离线检测
- `com11_scan_time.py` / `com11_exe_boot.py` — 性能/启动分析

## 构建与烧录

```bash
# 需要 ESP-IDF v5.x 环境
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```

## 目录说明

| 文件 | 说明 |
|------|------|
| `partitions-16MiB.csv` | 16MB Flash 分区表 (nvs / phy / factory / fat vfs / spiffs) |
| `sdkconfig.defaults` | LVGL 8.3 默认配置 |
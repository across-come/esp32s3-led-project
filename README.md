# ESP32-S3 物联网终端与图形交互系统

基于 **ESP32-S3 + ESP-IDF 5.1.2 + LVGL 8.3** 的物联网终端项目：集成 WiFi 扫描 / STA / SmartConfig 配网、TCP/UDP 通信、TF 卡离线缓存补传、ST7789 LCD（硬件 SPI + DMA）图形界面、XL9555 按键扩展、AT24C02 EEPROM 与 UART 在线调试。

项目的核心不是简单拼装多个驱动，而是解决网络、存储、显示和输入设备**同时运行时的模块解耦、非阻塞调度、SPI 总线共享与异常恢复**问题。

## 技术栈

| 类别 | 内容 |
|------|------|
| 主控 | ESP32-S3（内置 512KB SRAM，外挂 8MB PSRAM / 16MB Flash） |
| 开发环境 | ESP-IDF 5.1.2 + VS Code |
| 显示 | ST7789 LCD（320x240 RGB565）、硬件 SPI2 + DMA（目标 80MHz）、LVGL 8.3 |
| 网络 | WiFi 扫描 / STA / SmartConfig(ESPTOUCH)、TCP Client / Server、UDP、lwIP |
| 存储 | TF 卡（SDSPI + FATFS）、RAM FIFO + 文件离线缓存、SPIFFS |
| 外设 | XL9555（I2C IO 扩展）、AT24C02（EEPROM）、LED、按键、LEDC PWM、TIMG 定时器 |

## 软件架构

整体分四层：

1. **硬件和 ESP-IDF 适配层**：GPIO、软件 I2C、SPI2、ESP-IDF WiFi、lwIP、FATFS、esp_lcd、DMA、esp_timer
2. **设备驱动层**：ST7789 LCD、XL9555、AT24C02、TF 卡、UART、按键、LED
3. **业务服务层**：WiFi 状态机、TCP/UDP 通信、SD 缓存、LVGL 页面、数据生成、串口调试
4. **调度和通信层**：初始化注册表、任务链表、状态机、消息回调、flag、WiFi 事件队列、内存池

```
components/
├── BSP/
│   ├── WIFI/             # task_wifi (扫描/STA/SmartConfig 状态机) + task_tcp/tcps/udp
│   ├── SDIO/             # task_sd: TF 卡挂载、文件缓存补传
│   ├── STORE/            # cache_store: 离线数据缓存 (Cxxxxx.DAT)
│   ├── IIC_XL9555/       # 软件 I2C、SPI 总线、ST7789 LCD(DMA)、XL9555、AT24C02
│   ├── LVGL/             # task_lvgl: LVGL 显示适配、页面更新
│   ├── LED/  KEY/        # LED 控制、按键消抖/长短按识别
│   ├── LEDC/  TIMG/      # PWM、定时器/看门狗
│   ├── DATA/             # task_data: 数据生成
│   └── user_bsp.c        # 初始化注册表、任务链表、flag、消息、内存池框架
└── LVGL/                 # LVGL 8.3 源码库
```

## 核心设计

### 1. 自研协作式任务框架

业务层不使用"每个模块一个 FreeRTOS 任务"的结构，而是将原有自研框架移植到 ESP-IDF：

- **初始化注册**：模块通过 `INIT_REG(function, priority)` 注册，`user_init()` 按优先级排序统一执行，避免在 `app_main()` 中手写全部初始化顺序
- **任务链表 + 状态机**：`task_add()` 登记任务（函数指针、步骤、等待状态、计时器），`task_proc()` 每轮遍历调用；任务通过 `step` / `done` / `wait` 保存状态，所有业务非阻塞
- **flag 事件机制**：`flag_set()` 只置位，`task_proc()` 轮询时发现序号变化才执行回调，避免在 ISR 中处理业务
- **消息解耦**：`msg_add()/msg_send()` 名字注册 + 回调，模块间无需包含对方业务结构
- **静态内存池**：128KB 静态内存池（16B 块粒度），降低碎片和分配时间不确定性

架构主线：

```text
app_main
  +-- user_init(): 按优先级执行注册的初始化函数
  +-- task_proc(): 轮询自研业务任务 (flag_proc / task_wifi / task_tcp / task_sd / task_lcd / task_lvgl / task_key)

ESP-IDF 后台任务: WiFi 驱动任务 / esp_event 事件循环 / lwIP tcpip / esp_timer

WiFi 事件回调 -> FreeRTOS Queue -> task_wifi_proc -> wifi_ev_drain -> 业务状态机
```

### 2. WiFi 事件驱动

- `wifi_event_handler()` 由 ESP-IDF 事件循环任务调用，只做三件事：构造事件结构、复制必要数据、`xQueueSend(..., 0)` 非阻塞入队
- `wifi_ev_drain()` 在主循环中消费事件队列，把 ESP-IDF 事件翻译成业务状态机可识别的标志（扫描完成、断开退避重连、SmartConfig 获取 SSID/密码、STA 获取 IP）
- 扫描 / STA 连接 / 断线重连 / SmartConfig 全部拆成非阻塞状态机，配合超时定时器推进，回调与业务状态机完全解耦

### 3. 断网数据不丢（RAM + SD 两级缓存）

1. 网络正常时数据进入发送缓冲，通过 TCP 上传
2. 发送失败 / 断连 / RAM 缓冲不足时，数据落盘为 `Cxxxxx.DAT` 缓存文件（`f_sync()` 刷新）
3. 网络恢复后从最早的缓存文件开始补传
4. **收到应用层回声确认后才删除文件**（至少一次传输思路）

### 4. LCD + DMA + LVGL

- ST7789 走 SPI2 硬件 SPI + DMA，320x240 RGB565 分批刷新（LVGL 绘图缓冲 40 行优先，LCD 大区域填充 120 行优先），整屏内存控制在几十 KB
- DMA 完成回调通过 `xSemaphoreGiveFromISR()` 释放二值信号量，提交函数阻塞等待，保证 DMA 使用中的缓冲区不会被提前释放
- LVGL 只运行在主循环的 `task_lvgl_proc()` 中，其他模块通过消息发送 UI 命令，避免多线程访问
- esp_timer 1ms 调用 `lv_tick_inc()`，业务任务 10ms 调用 `lv_timer_handler()`，200ms 刷新 WiFi/TCP/SD 状态到页面

### 5. SPI2 总线共享

LCD 与 TF 卡共用 SCLK/MOSI/MISO，独立 CS 片选，由 ESP-IDF SPI 设备机制排队总线访问；应用层避免 DMA 未结束时修改缓冲区。

### 6. UART 在线调试

UART0 (115200, 8N1) 按行解析命令，通过 `var_add()` / `exe_add()` / `msg_add()` 注册机制在运行中触发 WiFi 扫描、STA 连接、SmartConfig、TCP setup、SD 挂载、LCD 绘图和状态查询，无需重新编译固件即可联调各模块。

## 上位机测试脚本 (Python)

保留两个具有代表性的串口联调脚本（其余开发期脚本已清理）：

| 脚本 | 说明 |
|------|------|
| `com11_sta_tcp.py` | 自动完成 WiFi STA 连接 → TCP 上传 → 数据生成全流程，解析日志统计 LCD/SD 任务耗时，输出 PASS 判定 |
| `com11_lvgl_p3.py` | LVGL 按键消息 UI 导航 + 页面切换冒烟测试 |

> 使用前请将脚本中的 SSID / 密码 / 服务器地址替换为你自己的环境。

## 构建与烧录

```bash
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```

## 目录说明

| 文件 | 说明 |
|------|------|
| `partitions-16MiB.csv` | 16MB Flash 分区表（nvs / phy_init / factory / fat vfs / spiffs） |
| `sdkconfig.defaults` | LVGL 8.3 默认配置 |
| `main/main.c` | 入口：`user_init()` 注册驱动 + `task_proc()` 主循环 |
| `components/BSP/user_bsp.c` | 初始化注册、任务链表、flag、消息、内存池框架 |
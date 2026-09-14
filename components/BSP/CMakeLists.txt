# 1. 确保你的组件目录下真的有一个叫 "LED" 的文件夹，且里面有 .c 和 .h 文件
#    注意1: 变量名加BSP_前缀, 防止与IDF官方组件(bt等)的同名变量冲突
#    注意2: 目录token全部加引号, 且目录名禁用 "CACHE"/"DATA" 等CMake关键字(见STORE目录)
set(BSP_SRC_DIRS "LED" "KEY" "TIMG" "LEDC" "IIC_XL9555/IIC" "IIC_XL9555/SPI" "IIC_XL9555/XL9555" "IIC_XL9555/AT24C02" "IIC_XL9555/SPILCD" "WIFI" "SDIO" "STORE" "DATA" "LVGL" "${CMAKE_CURRENT_LIST_DIR}")
set(BSP_INCLUDE_DIRS "LED" "KEY" "TIMG" "LEDC" "IIC_XL9555/IIC" "IIC_XL9555/SPI" "IIC_XL9555/XL9555" "IIC_XL9555/AT24C02" "IIC_XL9555/SPILCD" "WIFI" "SDIO" "STORE" "DATA" "LVGL" "${CMAKE_CURRENT_LIST_DIR}")
set(BSP_REQUIRES driver esp_timer esp_lcd esp_wifi esp_netif esp_event nvs_flash lwip fatfs sdmmc esp_hw_support LVGL)

# 2. 注册组件
idf_component_register(
    SRC_DIRS ${BSP_SRC_DIRS} 
    INCLUDE_DIRS ${BSP_INCLUDE_DIRS} 
    REQUIRES ${BSP_REQUIRES}
)

# 3. 修正编译选项（去掉了 format= 后面的等号，移除了危险的 -O3）
component_compile_options(
    -ffast-math 
    -Wno-error=format 
    -Wno-format
)

# 软件SPI位敲热路径: 覆盖工程默认 -Og
set_source_files_properties(
    "${CMAKE_CURRENT_LIST_DIR}/IIC_XL9555/SPI/spi.c"
    "${CMAKE_CURRENT_LIST_DIR}/IIC_XL9555/SPILCD/task_lcd.c"
    PROPERTIES COMPILE_OPTIONS "-O2"
)

# 4. ESP-IDF静态库只提取被引用的目标文件, 导致INIT_REG构造函数不执行
#    用 -u 强制链接各模块init符号, 使模块对象进入链接、自注册生效
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u key_init" "-u led_init" "-u timg_init" "-u ledc_init" "-u iic_init" "-u spi_init" "-u xl9555_init" "-u at24c02_init" "-u task_lcd_init" "-u wifi_init" "-u task_udp_init" "-u task_tcp_init" "-u task_tcps_init" "-u data_gen_init" "-u task_sd_init" "-u task_lvgl_init")
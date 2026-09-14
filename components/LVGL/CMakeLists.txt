# LVGL 8.3, 取自正点原子 55_lvgl_conversion_of_number
# 只编 src, 不编 examples/demos/tests; 配置走 Kconfig (CONFIG_LV_CONF_SKIP)

file(GLOB_RECURSE SOURCES ${CMAKE_CURRENT_LIST_DIR}/src/*.c)

idf_component_register(
    SRCS ${SOURCES}
    INCLUDE_DIRS ${CMAKE_CURRENT_LIST_DIR} ${CMAKE_CURRENT_LIST_DIR}/src
    REQUIRES esp_timer
)

target_compile_definitions(${COMPONENT_LIB} PUBLIC "-DLV_CONF_INCLUDE_SIMPLE")

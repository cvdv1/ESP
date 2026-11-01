# main 组件的源文件清单。
# 每新增一个 .c 都必须写进 SRCS，否则链接时会报 undefined reference。
# REQUIRES 声明本组件直接 include 的驱动组件（ESP-IDF 5.x 的驱动已拆成独立组件，
# 不写 REQUIRES 也能编过 main，但显式声明更清晰、增量编译更准）。
idf_component_register(
    SRCS
        "main.c"
        "wave_ledc.c"
        "wave_rmt.c"
        "wave_bus.c"
    INCLUDE_DIRS
        "."
    REQUIRES
        esp_driver_gpio
        esp_driver_ledc
        esp_driver_rmt
        esp_driver_uart
        esp_driver_spi
)

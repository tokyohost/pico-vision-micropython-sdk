# 将 ESP32-S3 LCD 内部 DMA 双缓冲模块编译进 MicroPython。
add_library(usermod_fn_lcd INTERFACE)

target_sources(usermod_fn_lcd INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/fn_lcd_dma.c
    ${CMAKE_CURRENT_LIST_DIR}/mod_fn_lcd.c
)

target_include_directories(usermod_fn_lcd INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(usermod INTERFACE usermod_fn_lcd)

# 将 fn_canvas 原生绘图模块编译进 MicroPython RP2 固件。
add_library(usermod_fn_canvas INTERFACE)

target_sources(usermod_fn_canvas INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/mod_fn_canvas.c
)

target_include_directories(usermod_fn_canvas INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(usermod INTERFACE usermod_fn_canvas)

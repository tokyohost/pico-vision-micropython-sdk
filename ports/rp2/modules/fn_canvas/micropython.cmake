# 将 fn_canvas 原生绘图模块编译进 MicroPython RP2 固件。
add_library(usermod_fn_canvas INTERFACE)

if(FN_CANVAS_BUILTIN_FONTS)
    # 字体数组不包含 QSTR，单独编译可避免 MicroPython 扫描三兆字节生成源码。
    add_library(fn_canvas_font_data STATIC
        ${CMAKE_CURRENT_LIST_DIR}/font_builtin_data.c
    )
    target_compile_definitions(usermod_fn_canvas INTERFACE
        FN_CANVAS_BUILTIN_FONTS=1
    )
    target_link_libraries(usermod_fn_canvas INTERFACE fn_canvas_font_data)
endif()

target_sources(usermod_fn_canvas INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/mod_fn_canvas.c
)

target_include_directories(usermod_fn_canvas INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(usermod INTERFACE usermod_fn_canvas)

# 将通用 fn_canvas 原生绘图模块编译进 MicroPython ESP32 固件。
set(FN_CANVAS_BUILTIN_FONTS 1)
include(${CMAKE_CURRENT_LIST_DIR}/../../../rp2/modules/fn_canvas/micropython.cmake)

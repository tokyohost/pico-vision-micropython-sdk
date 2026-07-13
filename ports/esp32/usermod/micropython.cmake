# 统一加载 fn-vision 在 ESP32 系列芯片上使用的原生模块。
# 模块源码不依赖 RP2 外设，因此复用现有实现，避免不同芯片维护两份代码。
include(${CMAKE_CURRENT_LIST_DIR}/fn_canvas/micropython.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/fn_protocol/micropython.cmake)

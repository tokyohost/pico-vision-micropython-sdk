# 将 ESP32-S3 WebSocket 原生数据面编译进 MicroPython 固件。
add_library(usermod_fn_websocket INTERFACE)

target_sources(usermod_fn_websocket INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/mod_fn_websocket.c
)

target_include_directories(usermod_fn_websocket INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(usermod INTERFACE usermod_fn_websocket)

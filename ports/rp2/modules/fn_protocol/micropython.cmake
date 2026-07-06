# 将 fn_protocol 原生协议模块编译进 MicroPython RP2 固件。
add_library(usermod_fn_protocol INTERFACE)

target_sources(usermod_fn_protocol INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/mod_fn_protocol.c
)

target_include_directories(usermod_fn_protocol INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(usermod INTERFACE usermod_fn_protocol)

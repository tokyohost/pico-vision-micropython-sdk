# 将 ESP32-S3 固件内置数据 CDC 的 Python 绑定编译进 MicroPython。
add_library(usermod_fn_usb_cdc INTERFACE)

target_sources(usermod_fn_usb_cdc INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}/mod_usb_cdc_data.c
    ${CMAKE_CURRENT_LIST_DIR}/usb_cdc_data_port.h
)

target_include_directories(usermod_fn_usb_cdc INTERFACE
    ${CMAKE_CURRENT_LIST_DIR}
)

target_link_libraries(usermod INTERFACE usermod_fn_usb_cdc)

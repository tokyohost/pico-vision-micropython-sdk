# ESP32-S3 N8R8：8MiB Flash 与 8MiB Octal PSRAM。
list(APPEND SDKCONFIG_DEFAULTS
    boards/sdkconfig.240mhz
    boards/sdkconfig.spiram_oct
    boards/ESP32_GENERIC_S3/sdkconfig.n8r8
)

list(APPEND MICROPY_DEF_BOARD
    MICROPY_HW_BOARD_NAME="Generic ESP32S3 N8R8 module with Octal-SPIRAM"
)

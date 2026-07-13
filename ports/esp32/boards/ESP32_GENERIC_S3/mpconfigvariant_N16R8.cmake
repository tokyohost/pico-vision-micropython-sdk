# ESP32-S3 N16R8：16MiB Flash 与 8MiB Octal PSRAM。
list(APPEND SDKCONFIG_DEFAULTS
    boards/sdkconfig.240mhz
    boards/sdkconfig.spiram_oct
    boards/ESP32_GENERIC_S3/sdkconfig.n16r8
)

list(APPEND MICROPY_DEF_BOARD
    MICROPY_HW_BOARD_NAME="Generic ESP32S3 N16R8 module with Octal-SPIRAM"
)

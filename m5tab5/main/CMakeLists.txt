# SPDX-License-Identifier: MIT
idf_component_register(
    SRCS "main.c" "install.c" "console.c" "vol.c"
    INCLUDE_DIRS "."
    REQUIRES app_update esp_app_format spi_flash esp_partition bootloader_support nvs_flash
             fatfs sdmmc esp_driver_sdmmc esp_driver_usb_serial_jtag vfs
             esp_lcd esp_timer feckless_graphics_handler feckless_drivers
             feckless_storage_handler
)

# SPDX-License-Identifier: MIT

# console.c reads the USB-Serial/JTAG's FIFO directly during a transfer;
# its hal header moved from hal to esp_hal_usb in IDF 6.
if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    set(DUP_HAL_USB esp_hal_usb)
else()
    set(DUP_HAL_USB hal)
endif()

idf_component_register(
    SRCS "main.c" "install.c" "console.c" "vol.c"
         "ymodem.c" "sha256.c" "xfer.c" "elfrun.c"
    INCLUDE_DIRS "."
    REQUIRES app_update esp_app_format spi_flash esp_partition bootloader_support nvs_flash
             fatfs sdmmc esp_driver_sdmmc esp_driver_usb_serial_jtag vfs
             esp_lcd esp_timer ${DUP_HAL_USB} feckless_graphics_handler feckless_drivers
             feckless_storage_handler feckless_abi
)

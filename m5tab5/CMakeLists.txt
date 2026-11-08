# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.16)

# exFAT is feckless-libs' fatfs component (0031), named in
# main/idf_component.yml: it patches this IDF's FatFs at configure time.
# CONFIG_FECKLESS_FATFS_EXFAT=n builds FAT32 only.

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# 0057: ESP-IDF 6.1 only. The feckless libraries dropped 5.x at v0.10.0,
# and the options that only IDF 6 has are in sdkconfig.defaults now.
if("${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR}" VERSION_LESS "6.1")
    message(FATAL_ERROR "Avoidant builds with ESP-IDF 6.1 or later "
                        "(this is ${IDF_VERSION_MAJOR}.${IDF_VERSION_MINOR})")
endif()

project(avoidant)

# 0028: libversions.h, the settings page's list of libraries, generated
# from the lock and components/ when the build is configured. See the file.
idf_component_get_property(main_lib main COMPONENT_LIB)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/libversions.cmake)
target_include_directories(${main_lib} PRIVATE "${LIBVERSIONS_DIR}")

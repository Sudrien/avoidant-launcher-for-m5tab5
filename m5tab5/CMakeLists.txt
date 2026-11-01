# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.16)

# exFAT is feckless-libs' fatfs component (0031), named in
# main/idf_component.yml: it patches this IDF's FatFs at configure time.
# CONFIG_FECKLESS_FATFS_EXFAT=n builds FAT32 only.

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# 0046: the options only IDF 6 has, after the shared defaults. See the top
# of sdkconfig.defaults.idf6.
if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.defaults.idf6")
endif()

project(avoidant)

# 0028: libversions.h, the settings page's list of libraries, generated
# from the lock and components/ when the build is configured. See the file.
idf_component_get_property(main_lib main COMPONENT_LIB)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/libversions.cmake)
target_include_directories(${main_lib} PRIVATE "${LIBVERSIONS_DIR}")

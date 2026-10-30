# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.16)

# exFAT: a patched copy of this IDF's fatfs into components/fatfs, made
# at configure time and before project.cmake scans components/. From
# defeatist-music-player-for-m5tab5; see the file. TAB5_NO_EXFAT in the
# environment opts out.
include(${CMAKE_CURRENT_LIST_DIR}/cmake/idfcopy.cmake)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/exfat.cmake)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(duplicitous)

# 0028: libversions.h, the settings page's list of libraries, generated
# from the lock and components/ when the build is configured. See the file.
idf_component_get_property(main_lib main COMPONENT_LIB)
include(${CMAKE_CURRENT_LIST_DIR}/cmake/libversions.cmake)
target_include_directories(${main_lib} PRIVATE "${LIBVERSIONS_DIR}")

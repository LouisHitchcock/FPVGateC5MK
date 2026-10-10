# FPVGate C5MK firmware: an ESP-IDF project (PlatformIO framework = espidf).
cmake_minimum_required(VERSION 3.16)

# The board (platformio.ini board_build.cmake_extra_args) goes into the
# image's version, "<version>+<board>", so an image says which board it is
# for: the S3 checks it before sending one over the link, as the boards'
# link pins differ.
if(NOT DEFINED NODE_BOARD)
    set(NODE_BOARD c5zero)
endif()
if(NOT DEFINED C5MK_VERSION)
    set(C5MK_VERSION 1.0.0)
endif()
set(PROJECT_VER "${C5MK_VERSION}+${NODE_BOARD}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(fpvgate_c5mk)

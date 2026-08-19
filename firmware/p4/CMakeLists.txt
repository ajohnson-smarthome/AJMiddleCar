cmake_minimum_required(VERSION 3.16)

# Firmware version = v<semver from version.txt>+<git commit count>.
# Set PROJECT_VER BEFORE project() so esp_app_desc / esp_app_get_description()->version uses it
# (takes precedence over IDF's version.txt auto-detection and git-describe fallback).
file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/version.txt" SEMVER LIMIT_COUNT 1)
string(STRIP "${SEMVER}" SEMVER)
execute_process(
    COMMAND git rev-list --count HEAD
    WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
    OUTPUT_VARIABLE BUILD_NUM
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET)
if(NOT BUILD_NUM)
    set(BUILD_NUM "0")
endif()
set(PROJECT_VER "v${SEMVER}+${BUILD_NUM}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(ajmiddlecar)

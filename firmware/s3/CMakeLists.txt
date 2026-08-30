cmake_minimum_required(VERSION 3.16)

# Firmware version = v<semver from the repo-root version.txt>+<git commit count> — the same
# string the car embeds, from the same file, because one release ships both images and the app
# compares one version against one tag. Set before project() so esp_app_get_description()->version
# carries it, which is what GET /status reports as `fw`.
set(VERSION_FILE "${CMAKE_CURRENT_LIST_DIR}/../../version.txt")
if(NOT EXISTS "${VERSION_FILE}")
    message(FATAL_ERROR "version.txt not found at ${VERSION_FILE}")
endif()
file(STRINGS "${VERSION_FILE}" SEMVER LIMIT_COUNT 1)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${VERSION_FILE}")
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
project(ajdongle)

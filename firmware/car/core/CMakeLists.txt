cmake_minimum_required(VERSION 3.16)

# Firmware version = v<semver from the repo-root version.txt>+<git commit count>.
# Set PROJECT_VER BEFORE project() so esp_app_desc / esp_app_get_description()->version uses it
# (takes precedence over IDF's version.txt auto-detection and git-describe fallback).
#
# The file sits at the repo root, not here, because the version identifies a RELEASE and a release
# carries two images — this one and firmware/s3's. firmware/s3/CMakeLists.txt reads the same file
# the same way. The EXISTS check is not ceremony: the 2026-08-19 migration noted that a
# mispointed path "would not fail loudly", producing an empty semver and a car reporting a
# garbage version. It fails loudly now.
set(VERSION_FILE "${CMAKE_CURRENT_LIST_DIR}/../../version.txt")
if(NOT EXISTS "${VERSION_FILE}")
    message(FATAL_ERROR "version.txt not found at ${VERSION_FILE}")
endif()
file(STRINGS "${VERSION_FILE}" SEMVER LIMIT_COUNT 1)
# Re-run configure when version.txt changes. The commit COUNT still refreshes only on
# configure (a git state cannot be a configure dependency), so an incremental bench build
# after new commits reports the count of its last configure — the release path fullcleans,
# which is what makes release binaries exact. Recorded by the 2026-08-23 audit.
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
project(ajmiddlecar)

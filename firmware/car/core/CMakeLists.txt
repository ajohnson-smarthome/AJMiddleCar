cmake_minimum_required(VERSION 3.16)

# Firmware version = v<semver from the repo-root version.txt>+<git commit count>.
# Set PROJECT_VER BEFORE project() so esp_app_desc / esp_app_get_description()->version uses it
# (takes precedence over IDF's version.txt auto-detection and git-describe fallback).
#
# The file sits at the repo root, not here, because the version identifies a RELEASE and a release
# carries two images — this one and firmware/dongle's. firmware/dongle/CMakeLists.txt reads the same file
# the same way. The EXISTS check is not ceremony: the 2026-08-19 migration noted that a
# mispointed path "would not fail loudly", producing an empty semver and a car reporting a
# garbage version. It fails loudly now.
set(VERSION_FILE "${CMAKE_CURRENT_LIST_DIR}/../../../version.txt")
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

# ESP32-P4 revision 1.3 has no Zba/Zbb/Zbs. esp_ipa 2.3.0 ships its IDF-6 archive built WITH them
# (`readelf -A lib/esp32p4/v6.0+/libesp_ipa.a` → `zba1p0_zbb1p0_zbs1p0`), so the first `sh2add`
# inside esp_ipa_pipeline_create is an illegal-instruction panic the moment a sensor is
# detected — a boot loop, found on the bench 2026-09-14. The same component ships an IDF-5.5
# archive built for the older ISA (`xesploop` only); it links cleanly against 6.0.2 and the
# sensor comes up on it, so the component's imported target is pointed at that one instead.
# Keyed on the revision selection, not on the IDF version: a board on revision 3.0 keeps the
# archive the vendor meant for it. Re-check on every esp_ipa bump — the day the v6.0+ archive
# is built without the B extension (or the vendor ships one per revision), this goes.
if(CONFIG_ESP32P4_SELECTS_REV_LESS_V3 AND TARGET prebuilt)
    set(ipa_old_isa "${CMAKE_CURRENT_LIST_DIR}/managed_components/espressif__esp_ipa/lib/esp32p4/v5.5/libesp_ipa.a")
    if(NOT EXISTS "${ipa_old_isa}")
        message(FATAL_ERROR "esp_ipa: no IDF-5.5 archive at ${ipa_old_isa} — the revision <3.0 override needs it")
    endif()
    set_target_properties(prebuilt PROPERTIES IMPORTED_LOCATION "${ipa_old_isa}")
    message(STATUS "esp_ipa: revision <3.0 selected — linking the IDF-5.5 archive (no Zba/Zbb)")
endif()

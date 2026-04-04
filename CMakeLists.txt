cmake_minimum_required(VERSION 3.16)
project(batchpress VERSION 2.0.0 LANGUAGES CXX)

# ── Standards ──────────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD          17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS        OFF)

# ── Compiler flags ─────────────────────────────────────────────────────────────
if(MSVC)
    add_compile_options(/W4 /O2 /utf-8)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -O2)
    if(NOT ANDROID)
        add_compile_options(-pthread)
        add_link_options(-pthread)
    endif()
endif()

# ── stb headers (auto-download if not present) ────────────────────────────────
# stb is header-only and public domain. We download the headers automatically
# so developers don't need a manual step.

set(STB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/core/third_party")

function(ensure_stb_header FILENAME URL)
    set(DEST "${STB_DIR}/${FILENAME}")
    if(NOT EXISTS "${DEST}")
        message(STATUS "Downloading ${FILENAME} from nothings/stb...")
        file(DOWNLOAD "${URL}" "${DEST}"
            STATUS DL_STATUS
            TLS_VERIFY ON)
        list(GET DL_STATUS 0 DL_CODE)
        if(NOT DL_CODE EQUAL 0)
            message(FATAL_ERROR
                "Failed to download ${FILENAME}.\n"
                "Download manually:\n"
                "  curl -L ${URL} -o ${DEST}")
        endif()
        message(STATUS "Downloaded ${FILENAME}")
    endif()
endfunction()

set(STB_RAW "https://raw.githubusercontent.com/nothings/stb/master")
ensure_stb_header(stb_image.h        "${STB_RAW}/stb_image.h")
ensure_stb_header(stb_image_write.h  "${STB_RAW}/stb_image_write.h")
ensure_stb_header(stb_image_resize2.h "${STB_RAW}/stb_image_resize2.h")

# ══════════════════════════════════════════════════════════════════════════════
#  CORE — shared library (libbatchpress_core.so / batchpress_core.dll)
#
#  Rules:
#    · No <iostream>, <cstdio> or any UI header in core/src/
#    · Compiles on Linux, Windows, macOS and Android NDK without changes
#    · BATCHPRESS_BUILD_SHARED is set so export.hpp emits dllexport on Windows
# ══════════════════════════════════════════════════════════════════════════════

add_library(batchpress_core SHARED
    core/src/types.cpp
    core/src/processor.cpp
    core/src/scanner.cpp
)

# Video processor is optional on Android (requires ffmpeg-kit)
if(BATCHPRESS_ENABLE_VIDEO AND (NOT ANDROID OR DEFINED FFMPEG_KIT_DIR))
    target_sources(batchpress_core PRIVATE core/src/video_processor.cpp)
    if(ANDROID)
        target_compile_definitions(batchpress_core PRIVATE BATCHPRESS_HAS_VIDEO)
    endif()
endif()

target_include_directories(batchpress_core
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/core/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/core/third_party
)

target_compile_definitions(batchpress_core PRIVATE BATCHPRESS_BUILD_SHARED)

# Suppress third-party warnings from stb headers included in these files
foreach(STB_FILE core/src/processor.cpp core/src/scanner.cpp)
    set_source_files_properties(${STB_FILE} PROPERTIES
        COMPILE_OPTIONS
            "-Wno-unused-but-set-variable;-Wno-missing-field-initializers;-Wno-unused-function")
endforeach()

if(NOT MSVC AND NOT ANDROID)
    target_compile_options(batchpress_core PRIVATE -fvisibility=hidden)
endif()

# ── FFmpeg / libav linking ─────────────────────────────────────────────────────

option(BATCHPRESS_ENABLE_VIDEO "Enable video processing (requires ffmpeg-kit on Android)" ON)

if(ANDROID)
    # ffmpeg-kit: set FFMPEG_KIT_DIR to the extracted ffmpeg-kit AAR/zip path
    # e.g. cmake ... -DFFMPEG_KIT_DIR=/path/to/ffmpeg-kit-full
    if(BATCHPRESS_ENABLE_VIDEO AND DEFINED FFMPEG_KIT_DIR)
        set(FFMPEG_LIB_DIR "${FFMPEG_KIT_DIR}/android/libs/${ANDROID_ABI}")
        target_include_directories(batchpress_core PRIVATE "${FFMPEG_KIT_DIR}/android/include")
        foreach(LIB avcodec avformat avutil swscale swresample)
            add_library(${LIB} SHARED IMPORTED)
            set_target_properties(${LIB} PROPERTIES
                IMPORTED_LOCATION "${FFMPEG_LIB_DIR}/lib${LIB}.so")
            target_link_libraries(batchpress_core PRIVATE ${LIB})
        endforeach()
        target_link_libraries(batchpress_core PRIVATE log)
        target_compile_definitions(batchpress_core PRIVATE BATCHPRESS_HAS_VIDEO)
        message(STATUS "Video processing ENABLED (ffmpeg-kit: ${FFMPEG_KIT_DIR})")
    else()
        message(WARNING "Video processing DISABLED — ffmpeg-kit not found. "
                        "Download from https://github.com/arthenica/ffmpeg-kit/releases "
                        "and set -DFFMPEG_KIT_DIR=<path> or -DBATCHPRESS_ENABLE_VIDEO=ON")
    endif()
else()
    # Linux / Windows / macOS: use system FFmpeg or vcpkg
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(FFMPEG REQUIRED
            libavcodec libavformat libavutil libswscale libswresample)
        target_include_directories(batchpress_core PRIVATE ${FFMPEG_INCLUDE_DIRS})
        target_link_libraries(batchpress_core PRIVATE ${FFMPEG_LIBRARIES})
        target_compile_options(batchpress_core PRIVATE ${FFMPEG_CFLAGS_OTHER})
    else()
        # Fallback: manual paths (set FFMPEG_ROOT)
        if(DEFINED FFMPEG_ROOT)
            target_include_directories(batchpress_core PRIVATE "${FFMPEG_ROOT}/include")
            target_link_directories(batchpress_core PRIVATE "${FFMPEG_ROOT}/lib")
        endif()
        target_link_libraries(batchpress_core PRIVATE
            avcodec avformat avutil swscale swresample)
    endif()
    target_link_libraries(batchpress_core PRIVATE pthread)

    # ── libwebp for WebP encoding ───────────────────────────────────────────────
    pkg_check_modules(WEBP QUIET libwebp)
    if(WEBP_FOUND)
        target_include_directories(batchpress_core PRIVATE ${WEBP_INCLUDE_DIRS})
        target_link_libraries(batchpress_core PRIVATE ${WEBP_LIBRARIES})
        target_compile_options(batchpress_core PRIVATE ${WEBP_CFLAGS_OTHER})
        target_compile_definitions(batchpress_core PRIVATE BATCHPRESS_HAS_WEBP)
        message(STATUS "WebP support enabled (libwebp ${WEBP_VERSION})")
    else()
        message(WARNING "libwebp not found — WebP encoding will fall back to PNG. "
                        "Install: sudo apt install libwebp-dev")
    endif()
endif()

# ══════════════════════════════════════════════════════════════════════════════
#  UI: CLI — Linux / Windows terminal executable
#  Built when NOT cross-compiling for Android.
# ══════════════════════════════════════════════════════════════════════════════

if(NOT ANDROID)
    add_executable(batchpress
        ui/cli/main.cpp
        ui/cli/cli.cpp
        ui/cli/progress.cpp
        ui/cli/scan_report.cpp
        ui/cli/video_scan_report.cpp
        ui/cli/select.cpp
    )

    target_include_directories(batchpress PRIVATE ui/cli)
    target_link_libraries(batchpress PRIVATE batchpress_core ${FFMPEG_LIBRARIES})
endif()

# ══════════════════════════════════════════════════════════════════════════════
#  UI: Android JNI bridge — libbatchpress_jni.so
#  Built only when CMake is invoked by the Android NDK toolchain.
# ══════════════════════════════════════════════════════════════════════════════

if(ANDROID)
    add_library(batchpress_jni SHARED
        ui/android/jni/jni_bridge.cpp
    )

    target_link_libraries(batchpress_jni
        PRIVATE
            batchpress_core
            log          # __android_log_print
    )
endif()

# ══════════════════════════════════════════════════════════════════════════════
#  UI: Qt — placeholder (uncomment when adding Qt frontend)
# ══════════════════════════════════════════════════════════════════════════════

# find_package(Qt6 COMPONENTS Widgets QUIET)
# if(Qt6_FOUND AND NOT ANDROID)
#     qt_add_executable(batchpress_qt ui/qt/main_qt.cpp)
#     target_link_libraries(batchpress_qt PRIVATE batchpress_core Qt6::Widgets)
# endif()

# ══════════════════════════════════════════════════════════════════════════════
#  Tests (Linux/Windows only)
# ══════════════════════════════════════════════════════════════════════════════

option(BUILD_TESTS "Build unit tests" ON)

if(BUILD_TESTS AND NOT ANDROID)
    find_package(GTest QUIET)
    if(GTest_FOUND)
        enable_testing()
        add_executable(test_batchpress tests/test_batchpress.cpp)
        target_link_libraries(test_batchpress PRIVATE batchpress_core GTest::GTest GTest::Main)
        add_test(NAME batchpress_tests COMMAND test_batchpress)
    else()
        message(STATUS "GTest not found — skipping tests")
    endif()
endif()

# ══════════════════════════════════════════════════════════════════════════════
#  Install
# ══════════════════════════════════════════════════════════════════════════════

install(TARGETS batchpress_core
    LIBRARY  DESTINATION lib     # .so
    RUNTIME  DESTINATION bin     # .dll on Windows
    ARCHIVE  DESTINATION lib     # .lib import lib on Windows
)

install(DIRECTORY core/include/batchpress DESTINATION include)

if(NOT ANDROID)
    install(TARGETS batchpress RUNTIME DESTINATION bin)
endif()

cmake_minimum_required(VERSION 3.22)
project(obs-airplay VERSION 1.2.2 LANGUAGES C CXX)

set(CMAKE_C_STANDARD 99)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(OBS_AIRPLAY_BUILD_TESTS "Build native verification tests" ON)
set(OBS_STUDIO_DIR "C:/Program Files/obs-studio" CACHE PATH "Path to installed OBS Studio")
set(OBS_STUDIO_BIN_DIR "${OBS_STUDIO_DIR}/bin/64bit" CACHE PATH "Path to OBS Studio 64-bit DLLs")

# ---------------------------------------------------------------------------
# Dependencies: OpenSSL (static), libplist (static), winpthread (static)
# ---------------------------------------------------------------------------
find_package(PkgConfig REQUIRED)

pkg_check_modules(PLIST REQUIRED libplist-2.0)
pkg_check_modules(PLIST_21 libplist-2.0>=2.1.0)
pkg_check_modules(PLIST_23 libplist-2.0>=2.3.0)

if(WIN32 AND MINGW)
    find_library(LIBPLIST_STATIC NAMES libplist-2.0.a HINTS ${PLIST_LIBRARY_DIRS} REQUIRED)
    find_library(LIBCRYPTO_STATIC NAMES libcrypto.a HINTS "C:/msys64/ucrt64/lib" REQUIRED)
    find_library(LIBWINPTHREAD_STATIC NAMES libwinpthread.a HINTS "C:/msys64/ucrt64/lib" REQUIRED)
    find_path(OPENSSL_INCLUDE_DIR openssl/evp.h HINTS "C:/msys64/ucrt64/include" REQUIRED)
    message(STATUS "Static libplist:     ${LIBPLIST_STATIC}")
    message(STATUS "Static libcrypto:    ${LIBCRYPTO_STATIC}")
    message(STATUS "Static libwinpthread:${LIBWINPTHREAD_STATIC}")
else()
    find_package(OpenSSL 1.1.1 REQUIRED)
endif()

# ---------------------------------------------------------------------------
# UxPlay AirPlay Mirroring Core (pure C, internal mDNS responder, no GStreamer)
# ---------------------------------------------------------------------------
set(UXPLAY_LIB_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/uxplay/lib")

add_subdirectory("${UXPLAY_LIB_DIR}/mdnsd" "${CMAKE_BINARY_DIR}/uxplay_mdnsd")
add_subdirectory("${UXPLAY_LIB_DIR}/llhttp" "${CMAKE_BINARY_DIR}/uxplay_llhttp")
add_subdirectory("${UXPLAY_LIB_DIR}/playfair" "${CMAKE_BINARY_DIR}/uxplay_playfair")
add_subdirectory("${UXPLAY_LIB_DIR}/fairplay_v2" "${CMAKE_BINARY_DIR}/uxplay_fairplay_v2")

file(GLOB UXPLAY_CORE_SOURCES "${UXPLAY_LIB_DIR}/*.c")

add_library(uxplay_airplay STATIC ${UXPLAY_CORE_SOURCES})
target_include_directories(uxplay_airplay
    PUBLIC
        "${UXPLAY_LIB_DIR}"
    PRIVATE
        "${UXPLAY_LIB_DIR}/playfair"
        "${UXPLAY_LIB_DIR}/fairplay_v2"
        "${UXPLAY_LIB_DIR}/llhttp"
        ${PLIST_INCLUDE_DIRS}
        ${OPENSSL_INCLUDE_DIR}
)

target_compile_definitions(uxplay_airplay
    PRIVATE
        STANDALONE
        __STDC_CONSTANT_MACROS
        __STDC_LIMIT_MACROS
        _REENTRANT
        _LARGEFILE64_SOURCE
        _FILE_OFFSET_BITS=64
        NOHOLD
        OPENSSL_API_COMPAT=0x10101000L
        $<$<BOOL:${PLIST_21_FOUND}>:PLIST_210>
        $<$<BOOL:${PLIST_23_FOUND}>:PLIST_230>
        $<$<BOOL:${WIN32}>:LIBPLIST_STATIC>
        $<$<BOOL:${WIN32}>:TARGET_POSIX>
        $<$<BOOL:${WIN32}>:_WIN32>
)

target_compile_options(uxplay_airplay PRIVATE -O2 -Wno-unused-variable -Wno-unused-function -Wno-deprecated-declarations)

if(WIN32 AND MINGW)
    target_link_libraries(uxplay_airplay
        PUBLIC
            dnssd
            llhttp
            playfair
            fairplay_v2
            "${LIBPLIST_STATIC}"
            "${LIBCRYPTO_STATIC}"
            "${LIBWINPTHREAD_STATIC}"
            ws2_32
            wsock32
            iphlpapi
            crypt32
            bcrypt
    )
else()
    target_link_libraries(uxplay_airplay
        PUBLIC
            dnssd
            llhttp
            playfair
            fairplay_v2
            ${PLIST_LIBRARIES}
            OpenSSL::Crypto
            pthread
    )
endif()

# ---------------------------------------------------------------------------
# OBS Studio (libobs 30-33+ universal C ABI) & Static Minimal FFmpeg 8.0 Decoders
# ---------------------------------------------------------------------------
set(LIBOBS_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/obs-studio/libobs")
set(FFMPEG_STATIC_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffmpeg-static-install")
set(FFMPEG8_INCLUDE_DIR "${FFMPEG_STATIC_DIR}/include")
set(GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

if(WIN32 AND MINGW)
    find_program(GENDEF_EXECUTABLE gendef HINTS "C:/msys64/ucrt64/bin" REQUIRED)
    find_program(DLLTOOL_EXECUTABLE dlltool HINTS "C:/msys64/ucrt64/bin" REQUIRED)
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/generate_obs_import_libs.cmake")

    # Only link obs.dll from OBS Studio; FFmpeg decoders are statically linked so
    # obs-airplay.dll works identically across OBS Studio 30, 31, 32, 33, and newer!
    generate_import_lib("obs" LIBOBS_IMPORT)

    set(FFMPEG_STATIC_LIBS
        "${FFMPEG_STATIC_DIR}/lib/libavcodec.a"
        "${FFMPEG_STATIC_DIR}/lib/libswresample.a"
        "${FFMPEG_STATIC_DIR}/lib/libswscale.a"
        "${FFMPEG_STATIC_DIR}/lib/libavutil.a"
    )
    foreach(_fflib ${FFMPEG_STATIC_LIBS})
        if(NOT EXISTS "${_fflib}")
            message(FATAL_ERROR "Missing static FFmpeg library: ${_fflib}. Run scripts/build-static-ffmpeg.sh first.")
        endif()
    endforeach()
endif()

# ---------------------------------------------------------------------------
# Native OBS Plugin Target: obs-airplay.dll
# ---------------------------------------------------------------------------
add_library(obs-airplay MODULE
    src/plugin-main.cpp
    src/airplay-source.cpp
    src/airplay-source.hpp
    src/airplay-frame-filter.cpp
    src/airplay-frame-filter.hpp
    src/airplay-receiver.cpp
    src/airplay-receiver.hpp
    src/media-decoder.cpp
    src/media-decoder.hpp
    src/security-manager.cpp
    src/security-manager.hpp
    $<$<AND:$<BOOL:${WIN32}>,$<BOOL:${MINGW}>>:src/obs-airplay.def>
    $<$<BOOL:${WIN32}>:packaging/version.rc>
)

set_target_properties(obs-airplay PROPERTIES
    PREFIX ""
    OUTPUT_NAME "obs-airplay"
)

target_include_directories(obs-airplay PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${GENERATED_INCLUDE_DIR}"
    "${LIBOBS_INCLUDE_DIR}"
    "${FFMPEG8_INCLUDE_DIR}"
)

target_compile_options(obs-airplay PRIVATE -O2 -Wall -Wextra)

if(WIN32 AND MINGW)
    target_link_libraries(obs-airplay PRIVATE
        uxplay_airplay
        "${LIBOBS_IMPORT}"
        ${FFMPEG_STATIC_LIBS}
        ole32
        gdi32
        gdiplus
        dwmapi
    )
    # Self-contained DLL: statically embed FFmpeg decoders, libgcc, libstdc++, libwinpthread, libcrypto, libplist
    target_link_options(obs-airplay PRIVATE
        -static-libgcc
        -static-libstdc++
        -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic
    )
endif()

# Copy locale files alongside build output for easy packaging/testing
add_custom_command(TARGET obs-airplay POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_CURRENT_SOURCE_DIR}/data"
        "${CMAKE_BINARY_DIR}/data"
)

# ---------------------------------------------------------------------------
# Automated Verification Suite
# ---------------------------------------------------------------------------
if(OBS_AIRPLAY_BUILD_TESTS)
    add_executable(test-obs-airplay
        tests/test-decoder-and-receiver.cpp
        src/airplay-receiver.cpp
        src/media-decoder.cpp
        src/security-manager.cpp
    )
    target_include_directories(test-obs-airplay PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src"
        "${GENERATED_INCLUDE_DIR}"
        "${LIBOBS_INCLUDE_DIR}"
        "${FFMPEG8_INCLUDE_DIR}"
    )
    if(WIN32 AND MINGW)
        target_link_libraries(test-obs-airplay PRIVATE
            uxplay_airplay
            "${LIBOBS_IMPORT}"
            ${FFMPEG_STATIC_LIBS}
            ole32
            gdi32
            gdiplus
            dwmapi
        )
        target_link_options(test-obs-airplay PRIVATE
            -static-libgcc
            -static-libstdc++
            -Wl,-Bstatic -lwinpthread -Wl,-Bdynamic
        )
    endif()
endif()

cmake_minimum_required(VERSION 3.24)
project(brokit LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(MSVC)
    add_compile_options(/MP /permissive- /utf-8)
    # Static CRT only when building standalone (not consumed as a subdirectory)
    if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
        set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
    endif()
endif()

# Dependencies resolve through cmake/bro_deps.cmake, like every repo in the bro
# ecosystem: an existing target (bro adds these before brokit, and its pins
# win) first; otherwise the ../<name> working tree beside the top-level project
# (the third-party ones, curl and FastNoise2, skip this step); otherwise the
# commit pinned here, fetched at configure.
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/bro_deps.cmake)

# bronze, the JavaScript runtime the api binds (it brings brass, its code
# generator). The api is bound across bronze's C++ embed boundary, so it has to
# build against the bronze the surrounding tree builds against.
bro_dependency(bronze GITHUB wlejon/bronze TARGET bronze_runtime_shared
               OPTIONS BRONZE_BUILD_SHARED_RUNTIME=ON BRONZE_BUILD_TESTS=OFF)

# libcurl — built as static library, no extras
if(APPLE AND NOT DEFINED OPENSSL_ROOT_DIR)
    if(EXISTS "/opt/homebrew/opt/openssl@3")
        set(OPENSSL_ROOT_DIR "/opt/homebrew/opt/openssl@3")
    elseif(EXISTS "/usr/local/opt/openssl@3")
        set(OPENSSL_ROOT_DIR "/usr/local/opt/openssl@3")
    elseif(EXISTS "/opt/homebrew/opt/openssl")
        set(OPENSSL_ROOT_DIR "/opt/homebrew/opt/openssl")
    elseif(EXISTS "/usr/local/opt/openssl")
        set(OPENSSL_ROOT_DIR "/usr/local/opt/openssl")
    endif()
endif()

if(NOT TARGET libcurl_static)
    set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
    set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
    set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mbedtls/CMakeLists.txt" OR BROKIT_USE_MBEDTLS)
        set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
        set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
        set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
        set(INSTALL_MBEDTLS_HEADERS OFF CACHE BOOL "" FORCE)
        set(DISABLE_PACKAGE_CONFIG_AND_INSTALL ON CACHE BOOL "" FORCE)
        if(NOT TARGET mbedtls)
            add_subdirectory(third_party/mbedtls EXCLUDE_FROM_ALL)
        endif()
        set(MBEDTLS_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mbedtls/include" CACHE PATH "" FORCE)
        set(MBEDTLS_LIBRARY mbedtls CACHE STRING "" FORCE)
        set(MBEDX509_LIBRARY mbedx509 CACHE STRING "" FORCE)
        set(MBEDCRYPTO_LIBRARY mbedcrypto CACHE STRING "" FORCE)
        set(CURL_USE_MBEDTLS ON CACHE BOOL "" FORCE)
        set(CURL_USE_SCHANNEL OFF CACHE BOOL "" FORCE)
        set(CURL_USE_OPENSSL OFF CACHE BOOL "" FORCE)
    elseif(WIN32)
        set(CURL_USE_SCHANNEL ON CACHE BOOL "" FORCE)   # Windows native TLS
        set(CURL_USE_OPENSSL OFF CACHE BOOL "" FORCE)
        set(CURL_USE_MBEDTLS OFF CACHE BOOL "" FORCE)
    else()
        set(CURL_USE_SCHANNEL OFF CACHE BOOL "" FORCE)
        set(CURL_USE_OPENSSL ON CACHE BOOL "" FORCE)
        set(CURL_USE_MBEDTLS OFF CACHE BOOL "" FORCE)
        set(CURL_CA_FALLBACK ON CACHE BOOL "" FORCE)
        if(APPLE)
            set(USE_APPLE_SECTRUST ON CACHE BOOL "" FORCE)
        endif()
    endif()

    set(CURL_USE_LIBSSH2 OFF CACHE BOOL "" FORCE)
    set(CURL_ZLIB OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
    set(ENABLE_MANUAL OFF CACHE BOOL "" FORCE)
    set(CURL_DISABLE_LDAP ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_LDAPS ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_TELNET ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_DICT ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_TFTP ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_POP3 ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_IMAP ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_SMTP ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_GOPHER ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_RTSP ON CACHE BOOL "" FORCE)
    set(CURL_DISABLE_MQTT ON CACHE BOOL "" FORCE)
    set(CURL_USE_LIBPSL OFF CACHE BOOL "" FORCE)
    set(CURL_BROTLI OFF CACHE BOOL "" FORCE)
    set(CURL_ZSTD OFF CACHE BOOL "" FORCE)
    set(USE_NGHTTP2 OFF CACHE BOOL "" FORCE)
    set(USE_LIBIDN2 OFF CACHE BOOL "" FORCE)
    bro_dependency(curl GITHUB curl/curl REF 527573490eb2564b3d7c9dd51d8bff963b5d6303 THIRD_PARTY TARGET libcurl_static)
endif()

# FastNoise2 — SIMD noise generation for procedural worlds
option(BROKIT_ENABLE_NOISE "Build FastNoise2 noise API" ON)
if(BROKIT_ENABLE_NOISE)
    bro_dependency(FastNoise2 GITHUB Auburn/FastNoise2 REF ba93f17ec40a9d09066c8d07b3e72b789e5b5657 THIRD_PARTY TARGET FastNoise
                   OPTIONS FASTNOISE2_NOISETOOL=OFF FASTNOISE2_TESTS=OFF)
endif()

# miniz — amalgamation build (single file, no submodule); DEFLATE backend
# for CompressionStream / DecompressionStream
if(NOT TARGET miniz)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/miniz/CMakeLists.txt")
        add_subdirectory(third_party/miniz EXCLUDE_FROM_ALL)
    else()
        message(FATAL_ERROR "miniz not found. Place miniz.c/h in third_party/miniz/")
    endif()
endif()

# SQLite — amalgamation build (single file, no submodule)
if(NOT TARGET sqlite3)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/sqlite/CMakeLists.txt")
        add_subdirectory(third_party/sqlite EXCLUDE_FROM_ALL)
    else()
        message(FATAL_ERROR "SQLite not found. Place sqlite3.c/h in third_party/sqlite/")
    endif()
endif()

add_subdirectory(src)
enable_testing()
add_subdirectory(tests)

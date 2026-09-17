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

# bronze, the JavaScript runtime the api binds: a checkout beside this one
# (../bronze, with ../brass beside it — bronze resolves and add_subdirectory()s
# its own code generator), the layout of a bro tree and of CI (bronze's
# .github/actions/checkout-toolchain), or an explicit -DBRONZE_DIR. No vendored
# fallback: the api is bound across bronze's C++ embed boundary, so it has to
# build against the bronze the surrounding tree builds against. Under bro this
# block is skipped, bro having added bronze already.
if(NOT TARGET bronze_runtime_shared)
    set(BRONZE_BUILD_SHARED_RUNTIME ON CACHE BOOL "" FORCE)
    set(BRONZE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    if(DEFINED BRONZE_DIR AND EXISTS "${BRONZE_DIR}/CMakeLists.txt")
        # Explicit BRONZE_DIR supplied
    elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../bronze/CMakeLists.txt")
        set(BRONZE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../bronze" CACHE PATH "bronze source tree")
    endif()

    if(NOT BRONZE_DIR OR NOT EXISTS "${BRONZE_DIR}/CMakeLists.txt")
        message(FATAL_ERROR
            "bronze not found: check it out beside this repository (../bronze, with ../brass beside it) "
            "or pass -DBRONZE_DIR=<path>.")
    endif()

    add_subdirectory("${BRONZE_DIR}" "${CMAKE_BINARY_DIR}/bronze-build" EXCLUDE_FROM_ALL)
endif()

# libcurl — built as static library, no extras
if(NOT TARGET libcurl_static)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/curl/CMakeLists.txt")
        set(BUILD_CURL_EXE OFF CACHE BOOL "" FORCE)
        set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
        set(BUILD_STATIC_LIBS ON CACHE BOOL "" FORCE)
        set(CURL_USE_SCHANNEL ON CACHE BOOL "" FORCE)   # Windows native TLS
        set(CURL_USE_OPENSSL OFF CACHE BOOL "" FORCE)
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
        add_subdirectory(third_party/curl EXCLUDE_FROM_ALL)
    else()
        message(FATAL_ERROR "libcurl not found. Run: git submodule update --init")
    endif()
endif()

# FastNoise2 — SIMD noise generation for procedural worlds
option(BROKIT_ENABLE_NOISE "Build FastNoise2 noise API" ON)
if(BROKIT_ENABLE_NOISE)
    if(NOT TARGET FastNoise)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/FastNoise2/CMakeLists.txt")
            set(FASTNOISE2_NOISETOOL OFF CACHE BOOL "" FORCE)
            set(FASTNOISE2_TESTS OFF CACHE BOOL "" FORCE)
            add_subdirectory(third_party/FastNoise2 EXCLUDE_FROM_ALL)
        else()
            message(FATAL_ERROR "FastNoise2 not found. Run: git submodule update --init")
        endif()
    endif()
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

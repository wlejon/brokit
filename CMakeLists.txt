cmake_minimum_required(VERSION 3.24)
project(brokit LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(MSVC)
    add_compile_options(/MP /permissive- /utf-8)
    # Static CRT so executables run without vcruntime DLLs (e.g. in Windows Sandbox)
    set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
endif()

# QuickJS — consumer can provide it, otherwise build from bundled submodule
if(NOT TARGET qjs)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/third_party/quickjs/CMakeLists.txt")
        set(QJS_BUILD_CLI OFF CACHE BOOL "" FORCE)
        set(QJS_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
        set(BUILD_QJS_LIBC ON CACHE BOOL "" FORCE)
        add_subdirectory(third_party/quickjs EXCLUDE_FROM_ALL)
    else()
        message(FATAL_ERROR "QuickJS not found. Either provide the qjs target or place QuickJS in third_party/quickjs/")
    endif()
endif()

add_subdirectory(src)
add_subdirectory(tests)

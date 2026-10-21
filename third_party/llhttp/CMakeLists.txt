# llhttp 9.4.3 (https://github.com/nodejs/llhttp) — Node.js official HTTP/1.1
# C parser, MIT. Backs the `http` module in src/api/http.cpp + js/http.js.
add_library(llhttp STATIC llhttp.c api.c http.c)
target_include_directories(llhttp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Suppress warnings in third-party code
if(MSVC)
    target_compile_options(llhttp PRIVATE /W0)
else()
    target_compile_options(llhttp PRIVATE -w)
endif()

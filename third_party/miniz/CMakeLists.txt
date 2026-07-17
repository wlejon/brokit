# miniz 3.0.2 (https://github.com/richgel999/miniz) — single-TU DEFLATE
# codec (RFC 1950/1951) + crc32, MIT. Backs CompressionStream /
# DecompressionStream; the gzip (RFC 1952) header/trailer layer lives in
# src/api/compression.cpp.
add_library(miniz STATIC miniz.c)
target_include_directories(miniz PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# mz_* names only — the zlib-compatible alias macros (#define compress,
# crc32, deflate, ...) would pollute every consumer TU. Archive/stdio APIs
# are dead weight here.
target_compile_definitions(miniz PUBLIC
    MINIZ_NO_ZLIB_COMPATIBLE_NAMES
    MINIZ_NO_ARCHIVE_APIS
    MINIZ_NO_ARCHIVE_WRITING_APIS
    MINIZ_NO_STDIO
)

# Suppress warnings in third-party code
if(MSVC)
    target_compile_options(miniz PRIVATE /W0)
else()
    target_compile_options(miniz PRIVATE -w)
endif()

## ── Embed JS polyfills as C++ headers at build time ──────────────────────────
# Each .js file in js/ becomes a .h with the content as a C string literal.
# The generated headers land in ${CMAKE_CURRENT_BINARY_DIR}/generated/.

set(EMBED_SCRIPT "${CMAKE_SOURCE_DIR}/cmake/embed_js.cmake")
set(GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${GEN_DIR}")

function(embed_js JS_FILE VAR_NAME)
    set(INPUT  "${CMAKE_CURRENT_SOURCE_DIR}/js/${JS_FILE}")
    set(OUTPUT "${GEN_DIR}/${JS_FILE}.h")
    add_custom_command(
        OUTPUT  "${OUTPUT}"
        COMMAND ${CMAKE_COMMAND}
                -DINPUT="${INPUT}"
                -DOUTPUT="${OUTPUT}"
                -DVAR_NAME="${VAR_NAME}"
                -P "${EMBED_SCRIPT}"
        DEPENDS "${INPUT}" "${EMBED_SCRIPT}"
        COMMENT "Embedding js/${JS_FILE} -> ${JS_FILE}.h"
    )
    list(APPEND GENERATED_HEADERS "${OUTPUT}")
    set(GENERATED_HEADERS "${GENERATED_HEADERS}" PARENT_SCOPE)
endfunction()

embed_js(abort.js           js_abort)
embed_js(structuredclone.js js_structuredclone)
embed_js(timers.js          js_timers)
embed_js(console_time.js    js_console_time)
embed_js(url.js             js_url)
embed_js(encoding.js        js_encoding)
embed_js(treewalker.js      js_treewalker)
embed_js(url_object.js      js_url_object)
embed_js(path.js            js_path)
embed_js(readable_stream.js js_readable_stream)
embed_js(fs.js              js_fs)
embed_js(child_process.js   js_child_process)

## ── Library ──────────────────────────────────────────────────────────────────

add_library(brokit_api STATIC
    api.cpp
    console.cpp
    timers.cpp
    url.cpp
    crypto.cpp
    encoding.cpp
    treewalker.cpp
    abort.cpp
    structuredclone.cpp
    blob.cpp
    url_object.cpp
    process.cpp
    os.cpp
    path.cpp
    readable_stream.cpp
    fetch.cpp
    fs.cpp
    child_process.cpp
    ${GENERATED_HEADERS}
)
target_include_directories(brokit_api PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_include_directories(brokit_api PRIVATE ${GEN_DIR})
target_link_libraries(brokit_api PUBLIC brokit_runtime)
target_link_libraries(brokit_api PUBLIC libcurl_static)
target_include_directories(brokit_api PRIVATE ${CMAKE_SOURCE_DIR}/third_party/quickjs)

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
embed_js(indexeddb.js       js_indexeddb)
embed_js(writable_stream.js js_writable_stream)
embed_js(websocket.js       js_websocket)
embed_js(eventsource.js     js_eventsource)
embed_js(formdata.js        js_formdata)
embed_js(fetch_classes.js   js_fetch_classes)
embed_js(base64.js          js_base64)
embed_js(navigator.js       js_navigator)
embed_js(event_target.js    js_event_target)
embed_js(message_channel.js js_message_channel)
embed_js(fs_watch.js        js_fs_watch)
embed_js(fetch_helpers.js   js_fetch_helpers)
embed_js(process.js         js_process)
embed_js(buffer.js          js_buffer)
embed_js(events.js          js_events)
embed_js(util.js            js_util)
embed_js(compression.js     js_compression)
embed_js(net.js             js_net)
embed_js(dgram.js           js_dgram)

## ── Library ──────────────────────────────────────────────────────────────────

set(BROKIT_API_SOURCES
    api.cpp
    console.cpp
    timers.cpp
    url.cpp
    crypto.cpp
    crypto_subtle.cpp
    encoding.cpp
    treewalker.cpp
    abort.cpp
    structuredclone.cpp
    blob.cpp
    url_object.cpp
    process.cpp
    os.cpp
    path.cpp
    storage.cpp
    indexeddb.cpp
    indexeddb_js.cpp
    readable_stream.cpp
    fetch.cpp
    fs.cpp
    fs_watch.cpp
    fs_watch_linux.cpp
    fs_watch_macos.cpp
    fs_watch_windows.cpp
    child_process.cpp
    writable_stream.cpp
    websocket.cpp
    websocket_js.cpp
    eventsource.cpp
    formdata.cpp
    fetch_classes.cpp
    base64.cpp
    navigator.cpp
    event_target.cpp
    message_channel.cpp
    buffer.cpp
    events.cpp
    util.cpp
    compression.cpp
    net.cpp
    net_js.cpp
)

if(BROKIT_ENABLE_NOISE)
    list(APPEND BROKIT_API_SOURCES noise.cpp)
endif()

option(BROKIT_ENABLE_IMAGE "Build bro.image typed-array kernels" ON)
if(BROKIT_ENABLE_IMAGE)
    list(APPEND BROKIT_API_SOURCES image.cpp)
    # broimage provides the kernel implementations; image.cpp is the JS glue
    # that delegates to it. Standalone repo at ../broimage preferred, with a
    # third_party/broimage submodule fallback. When brokit builds inside bro,
    # broimage is already a target (added by bro/third_party/CMakeLists.txt
    # before brokit) and this block reuses it via the NOT TARGET guard.
    if(NOT TARGET broimage)
        set(BROIMAGE_DIR "${CMAKE_SOURCE_DIR}/../broimage" CACHE PATH
            "Path to standalone broimage repo")
        if(EXISTS "${BROIMAGE_DIR}/CMakeLists.txt")
            message(STATUS "broimage: using standalone repo at ${BROIMAGE_DIR}")
            add_subdirectory("${BROIMAGE_DIR}" "${CMAKE_BINARY_DIR}/broimage"
                             EXCLUDE_FROM_ALL)
        elseif(EXISTS "${CMAKE_SOURCE_DIR}/third_party/broimage/CMakeLists.txt")
            message(STATUS "broimage: using submodule at third_party/broimage")
            add_subdirectory("${CMAKE_SOURCE_DIR}/third_party/broimage"
                             "${CMAKE_BINARY_DIR}/broimage" EXCLUDE_FROM_ALL)
        else()
            message(FATAL_ERROR
                "BROKIT_ENABLE_IMAGE=ON requires broimage at ${BROIMAGE_DIR} "
                "or ${CMAKE_SOURCE_DIR}/third_party/broimage. Clone broimage "
                "next to brokit, or override with -DBROIMAGE_DIR=...")
        endif()
    endif()
endif()

add_library(brokit_api STATIC
    ${BROKIT_API_SOURCES}
    ${GENERATED_HEADERS}
)
target_include_directories(brokit_api PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_include_directories(brokit_api PRIVATE ${GEN_DIR})
target_link_libraries(brokit_api PUBLIC brokit_runtime)
target_link_libraries(brokit_api PUBLIC libcurl_static)
target_link_libraries(brokit_api PUBLIC sqlite3)
target_link_libraries(brokit_api PRIVATE miniz)
target_include_directories(brokit_api PRIVATE ${CMAKE_SOURCE_DIR}/third_party/quickjs)

if(BROKIT_ENABLE_NOISE)
    target_link_libraries(brokit_api PUBLIC FastNoise)
    target_compile_definitions(brokit_api PUBLIC BROKIT_HAS_NOISE)
endif()

if(BROKIT_ENABLE_IMAGE)
    target_compile_definitions(brokit_api PUBLIC BROKIT_HAS_IMAGE)
    target_link_libraries(brokit_api PRIVATE broimage::broimage)
endif()

# fs.watch — native filesystem watcher uses one OS thread per watcher.
# Threads is required on Linux for std::thread; macOS needs the CoreServices
# framework for FSEvents.
find_package(Threads REQUIRED)
target_link_libraries(brokit_api PUBLIC Threads::Threads)
# net.cpp — raw TCP/UDP sockets need Winsock on Windows.
if(WIN32)
    target_link_libraries(brokit_api PUBLIC ws2_32)
endif()
if(APPLE)
    target_link_libraries(brokit_api PUBLIC "-framework CoreServices" "-framework CoreFoundation")
endif()

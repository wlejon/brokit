## ── Compile JS polyfills with bronze AOT at build time ───────────────────
# Each .js file in js/ is compiled directly to an object file (.o) with its
# unique entry symbol and linked into brokit_api.
# On a target brass has no code generator for (BRASS_HOST_BACKEND, decided in
# brass's CMakeLists — Apple Silicon today) nothing is compiled and native
# no-op stubs are linked instead; tests/CMakeLists.txt reads the same
# variable to know there is no JavaScript to run there.

if(NOT BRASS_HOST_BACKEND)
    set(BROKIT_COMPILED_JS_OBJECTS "${CMAKE_CURRENT_SOURCE_DIR}/bronze_js_stubs_arm64.cpp")
    message(STATUS "brokit: api built with the no-op JS entries (brass has no code generator for this target)")
else()
    set(GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
    file(MAKE_DIRECTORY "${GEN_DIR}")

    set(BROKIT_GLOBALS "${CMAKE_CURRENT_SOURCE_DIR}/brokit.globals")
    set(BROKIT_COMPILED_JS_OBJECTS "")

    function(bronze_compile_js JS_FILE ENTRY_SYMBOL)
        set(INPUT  "${CMAKE_CURRENT_SOURCE_DIR}/js/${JS_FILE}.js")
        set(OUTPUT "${GEN_DIR}/${JS_FILE}.o")
        add_custom_command(
            OUTPUT  "${OUTPUT}"
            COMMAND $<TARGET_FILE:bronze> build "${INPUT}" --emit-obj --host-globals "${BROKIT_GLOBALS}" --entry-symbol=${ENTRY_SYMBOL} -o "${OUTPUT}"
            DEPENDS bronze "${INPUT}" "${BROKIT_GLOBALS}"
            COMMENT "bronze compile js/${JS_FILE}.js -> ${JS_FILE}.o (${ENTRY_SYMBOL})"
            VERBATIM
        )
        set_source_files_properties("${OUTPUT}" PROPERTIES EXTERNAL_OBJECT TRUE GENERATED TRUE)
        list(APPEND BROKIT_COMPILED_JS_OBJECTS "${OUTPUT}")
        set(BROKIT_COMPILED_JS_OBJECTS "${BROKIT_COMPILED_JS_OBJECTS}" PARENT_SCOPE)
    endfunction()

    bronze_compile_js(abort              bronze_abort_main)
    bronze_compile_js(structuredclone    bronze_structuredclone_main)
    bronze_compile_js(timers             bronze_timers_main)
    bronze_compile_js(console_time       bronze_console_time_main)
    bronze_compile_js(url                bronze_url_main)
    bronze_compile_js(encoding           bronze_encoding_main)
    bronze_compile_js(treewalker         bronze_treewalker_main)
    bronze_compile_js(url_object         bronze_url_object_main)
    bronze_compile_js(path               bronze_path_main)
    bronze_compile_js(readable_stream    bronze_readable_stream_main)
    bronze_compile_js(fs                 bronze_fs_main)
    bronze_compile_js(child_process      bronze_child_process_main)
    bronze_compile_js(indexeddb          bronze_indexeddb_main)
    bronze_compile_js(writable_stream    bronze_writable_stream_main)
    bronze_compile_js(websocket          bronze_websocket_main)
    bronze_compile_js(eventsource        bronze_eventsource_main)
    bronze_compile_js(formdata           bronze_formdata_main)
    bronze_compile_js(fetch_classes      bronze_fetch_classes_main)
    bronze_compile_js(xhr                bronze_xhr_main)
    bronze_compile_js(base64             bronze_base64_main)
    bronze_compile_js(navigator          bronze_navigator_main)
    bronze_compile_js(event_target       bronze_event_target_main)
    bronze_compile_js(message_channel    bronze_message_channel_main)
    bronze_compile_js(fs_watch           bronze_fs_watch_main)
    bronze_compile_js(fetch_helpers      bronze_fetch_helpers_main)
    bronze_compile_js(process            bronze_process_main)
    bronze_compile_js(buffer             bronze_buffer_main)
    bronze_compile_js(events             bronze_events_main)
    bronze_compile_js(util               bronze_util_main)
    bronze_compile_js(compression        bronze_compression_main)
    bronze_compile_js(net                bronze_net_main)
    bronze_compile_js(dgram              bronze_dgram_main)
    bronze_compile_js(websocket_server   bronze_websocket_server_main)
endif()

## ── Library ──────────────────────────────────────────────────────────────────

set(BROKIT_API_SOURCES
    host_class.cpp
    host_proxy.cpp
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
    xhr.cpp
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
    websocket_server_js.cpp
)

if(BROKIT_ENABLE_NOISE)
    list(APPEND BROKIT_API_SOURCES noise.cpp)
endif()

option(BROKIT_ENABLE_IMAGE "Build bro.image typed-array kernels" ON)
if(BROKIT_ENABLE_IMAGE)
    list(APPEND BROKIT_API_SOURCES image.cpp)
    if(NOT TARGET broimage)
        set(BROIMAGE_DIR "${brokit_SOURCE_DIR}/../broimage" CACHE PATH
            "Path to standalone broimage repo")
        if(EXISTS "${BROIMAGE_DIR}/CMakeLists.txt")
            message(STATUS "broimage: using standalone repo at ${BROIMAGE_DIR}")
            add_subdirectory("${BROIMAGE_DIR}" "${CMAKE_BINARY_DIR}/broimage"
                             EXCLUDE_FROM_ALL)
        elseif(EXISTS "${brokit_SOURCE_DIR}/third_party/broimage/CMakeLists.txt")
            message(STATUS "broimage: using submodule at third_party/broimage")
            add_subdirectory("${brokit_SOURCE_DIR}/third_party/broimage"
                             "${CMAKE_BINARY_DIR}/broimage" EXCLUDE_FROM_ALL)
        else()
            message(FATAL_ERROR
                "BROKIT_ENABLE_IMAGE=ON requires broimage at ${BROIMAGE_DIR} "
                "or ${brokit_SOURCE_DIR}/third_party/broimage. Clone broimage "
                "next to brokit, or override with -DBROIMAGE_DIR=...")
        endif()
    endif()
endif()

add_library(brokit_api STATIC
    ${BROKIT_API_SOURCES}
    ${BROKIT_COMPILED_JS_OBJECTS}
)
target_include_directories(brokit_api PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(brokit_api PUBLIC brokit_runtime)
target_link_libraries(brokit_api PUBLIC libcurl_static)
target_link_libraries(brokit_api PUBLIC sqlite3)
target_link_libraries(brokit_api PRIVATE miniz)
target_link_libraries(brokit_api PUBLIC bronze::runtime_shared)

if(BROKIT_ENABLE_NOISE)
    target_link_libraries(brokit_api PUBLIC FastNoise)
    target_compile_definitions(brokit_api PUBLIC BROKIT_HAS_NOISE)
endif()

if(BROKIT_ENABLE_IMAGE)
    target_compile_definitions(brokit_api PUBLIC BROKIT_HAS_IMAGE)
    target_link_libraries(brokit_api PRIVATE broimage::broimage)
endif()

# fs.watch — native filesystem watcher uses one OS thread per watcher.
find_package(Threads REQUIRED)
target_link_libraries(brokit_api PUBLIC Threads::Threads)
# net.cpp — raw TCP/UDP sockets need Winsock on Windows.
if(WIN32)
    target_link_libraries(brokit_api PUBLIC ws2_32)
endif()
if(APPLE)
    target_link_libraries(brokit_api PUBLIC "-framework CoreServices" "-framework CoreFoundation")
endif()

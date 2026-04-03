add_library(brokit_api STATIC
    api.cpp
    console.cpp
    timers.cpp
    url.cpp
    crypto.cpp
    encoding.cpp
    treewalker.cpp
)
target_include_directories(brokit_api PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(brokit_api PUBLIC brokit_runtime)
target_include_directories(brokit_api PRIVATE ${CMAKE_SOURCE_DIR}/third_party/quickjs)

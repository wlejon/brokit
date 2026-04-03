add_library(brokit_runtime STATIC
    runtime.cpp
)
target_include_directories(brokit_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(brokit_runtime PUBLIC qjs)
target_include_directories(brokit_runtime PRIVATE ${CMAKE_SOURCE_DIR}/third_party/quickjs)

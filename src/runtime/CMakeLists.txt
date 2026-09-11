add_library(brokit_runtime STATIC
    runtime.cpp
)
target_include_directories(brokit_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(brokit_runtime PUBLIC bronze::runtime_shared bronze::cli)

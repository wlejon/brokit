add_subdirectory(runtime)
add_subdirectory(api)

# Main library: combines runtime + api into a single target for consumers
add_library(brokit INTERFACE)
target_link_libraries(brokit INTERFACE brokit_runtime brokit_api)
target_include_directories(brokit INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})

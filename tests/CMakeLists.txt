add_executable(brokit_test main.cpp)
target_link_libraries(brokit_test PRIVATE brokit)
target_include_directories(brokit_test PRIVATE ${CMAKE_SOURCE_DIR}/third_party/quickjs)

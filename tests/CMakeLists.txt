add_executable(brokit_test main.cpp)
target_link_libraries(brokit_test PRIVATE brokit)
target_include_directories(brokit_test PRIVATE ${CMAKE_SOURCE_DIR}/third_party/quickjs)

# Register one ctest per JS file so `ctest` drives the suite uniformly across
# platforms (brokit_test <file.js> runs a single file and returns non-zero on
# failure). Each test runs in the build tree so any scratch files (e.g. the
# *.idb SQLite fixtures) land there rather than in the source tree.
#
# test_fetch and test_fetch_abort are excluded: they make live requests to
# httpbin.org, so they are integration tests against a third-party endpoint,
# not deterministic unit tests. Run them locally with
# `brokit_test tests/js/test_fetch.js`.
file(GLOB BROKIT_JS_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/js/*.js")
foreach(js ${BROKIT_JS_TESTS})
    get_filename_component(name "${js}" NAME_WE)
    if(name MATCHES "^(test_fetch|test_fetch_abort)$")
        continue()
    endif()
    add_test(NAME ${name} COMMAND brokit_test "${js}")
    set_tests_properties(${name} PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
endforeach()

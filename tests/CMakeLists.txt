add_executable(brokit_test main.cpp)
target_link_libraries(brokit_test PRIVATE brokit bronze::runtime_shared)
if(MSVC)
    target_link_options(brokit_test PRIVATE "/FORCE:MULTIPLE")
endif()

if(WIN32)
    add_custom_command(TARGET brokit_test POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:bronze_runtime_shared>
            $<TARGET_FILE_DIR:brokit_test>
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_LINKER_FILE:bronze_runtime_shared>
            $<TARGET_FILE_DIR:brokit_test>
    )
endif()

# Register one ctest per JS file so `ctest` drives the suite uniformly across
# platforms (brokit_test <file.js> runs a single file and returns non-zero on
# failure). Each test runs in the build tree so any scratch files (e.g. the
# *.idb SQLite fixtures) land there rather than in the source tree.
#
# test_fetch and test_fetch_abort are excluded: they make live requests to
# httpbin.org, so they are integration tests against a third-party endpoint,
# not deterministic unit tests. Run them locally with
# `brokit_test tests/js/test_fetch.js`.
#
# Every one of these runs JavaScript, which brass compiles to machine code for
# the target — and on a target it has no code generator for (BRASS_HOST_BACKEND,
# decided in brass's CMakeLists; Apple Silicon today) that code cannot run.
# brokit_test still builds there, which is the check that the embed links; the
# tests are not registered rather than registered to bus-error.
if(NOT BRASS_HOST_BACKEND)
    message(STATUS "brokit: JS tests not registered (brass has no code generator for this target)")
    set(BROKIT_JS_TESTS "")
else()
    file(GLOB BROKIT_JS_TESTS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/js/*.js")
endif()
foreach(js ${BROKIT_JS_TESTS})
    get_filename_component(name "${js}" NAME_WE)
    if(name MATCHES "^(test_fetch|test_fetch_abort)$")
        continue()
    endif()
    add_test(NAME ${name} COMMAND brokit_test "${js}")
    set_tests_properties(${name} PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
    # The same script with a collection at every allocation and freed space
    # poisoned: a binding that holds a raw Value across an allocating call
    # reads garbage here instead of passing by luck. `ctest -LE gcstress`
    # skips these.
    add_test(NAME ${name}_gcstress COMMAND brokit_test "${js}")
    set_tests_properties(${name}_gcstress PROPERTIES
        WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}"
        ENVIRONMENT "BRONZE_GC_STRESS=1;BRONZE_GC_POISON=1"
        LABELS gcstress)
endforeach()

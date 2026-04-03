add_library(sqlite3 STATIC sqlite3.c)
target_include_directories(sqlite3 PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Recommended defines for embedded use
target_compile_definitions(sqlite3 PRIVATE
    SQLITE_THREADSAFE=0        # Single-threaded (QuickJS is single-threaded)
    SQLITE_OMIT_LOAD_EXTENSION # No dynamic extension loading
    SQLITE_OMIT_TCL_VARIABLE   # No Tcl
    SQLITE_DQS=0               # Strict double-quote semantics
)

# Suppress warnings in third-party code
if(MSVC)
    target_compile_options(sqlite3 PRIVATE /W0)
else()
    target_compile_options(sqlite3 PRIVATE -w)
endif()

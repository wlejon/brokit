add_library(brokit_runtime STATIC
    runtime.cpp
)
target_include_directories(brokit_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..)
target_link_libraries(brokit_runtime PUBLIC bronze::runtime_shared bronze::cli)

# Two brass images share the process: the shared runtime's, and the static one
# bronze::cli (the in-process compiler behind loadFile) pulls into the
# executable, coroutine.cpp included. libbronze_runtime_shared.so exports
# brass_coro_create, so an ELF link exports the executable's own definition of
# it too, and that one wins: the modules loadFile dlopen's resolve against the
# executable first, so they create their generator and async frames through
# the compiler's brass, which has no heap on this thread and files the frame
# in its own registry,
# where the runtime's brass_coro_unroot cannot find it ("names no frame a live
# heap holds"). Keeping the static brass's symbols out of the executable's
# dynamic table leaves the runtime's the only one a module can bind to. A DLL
# does not export what it does not declare, and a Mach-O module binds each
# import to the library that defines it, so this is an ELF-only concern.
if(TARGET brass AND NOT WIN32 AND NOT APPLE)
    target_link_options(brokit_runtime INTERFACE
        "LINKER:--exclude-libs,$<TARGET_LINKER_FILE_NAME:brass>")
endif()

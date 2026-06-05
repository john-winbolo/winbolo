# copy_brains.cmake — invoked at build time via cmake -P
# Mirrors a brains/<bot> directory tree from SOURCE_DIR into DEST_DIR while
# skipping *.md files. Plan / roadmap / WIP notes live alongside the .lua
# scripts during development but have no business in shipped artifacts
# (the macOS .app bundle, dist.zip, etc.).
# Pass -DSOURCE_DIR=... -DDEST_DIR=... on the command line.

if(NOT DEFINED SOURCE_DIR OR NOT DEFINED DEST_DIR)
    message(FATAL_ERROR "copy_brains.cmake requires -DSOURCE_DIR=... -DDEST_DIR=...")
endif()

file(GLOB_RECURSE _ENTRIES RELATIVE "${SOURCE_DIR}" "${SOURCE_DIR}/*")
foreach(_E ${_ENTRIES})
    if(_E MATCHES "\\.md$")
        continue()
    endif()
    get_filename_component(_DESTSUBDIR "${DEST_DIR}/${_E}" DIRECTORY)
    file(MAKE_DIRECTORY "${_DESTSUBDIR}")
    execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    "${SOURCE_DIR}/${_E}" "${DEST_DIR}/${_E}")
endforeach()

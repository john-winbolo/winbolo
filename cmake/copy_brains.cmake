# copy_brains.cmake — invoked at build time via cmake -P
# Mirrors a brains/<bot> directory tree from SOURCE_DIR into DEST_DIR while
# skipping *.md files. Plan / roadmap / WIP notes live alongside the .lua
# scripts during development but have no business in shipped artifacts
# (the macOS .app bundle, dist.zip, etc.).
# Pass -DSOURCE_DIR=... -DDEST_DIR=... on the command line.
# With -DALL_BRAINS=ON, SOURCE_DIR is the brains/ folder itself: every brain
# folder in it (brain_folders.cmake) is copied to DEST_DIR/<brain>, so a
# brain folder that a merge adds ships with the next build.

if(NOT DEFINED SOURCE_DIR OR NOT DEFINED DEST_DIR)
    message(FATAL_ERROR "copy_brains.cmake requires -DSOURCE_DIR=... -DDEST_DIR=...")
endif()

function(_copy_brain_tree src dest)
    file(GLOB_RECURSE _ENTRIES RELATIVE "${src}" "${src}/*")
    foreach(_E ${_ENTRIES})
        if(_E MATCHES "\\.md$")
            continue()
        endif()
        get_filename_component(_DESTSUBDIR "${dest}/${_E}" DIRECTORY)
        file(MAKE_DIRECTORY "${_DESTSUBDIR}")
        execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${src}/${_E}" "${dest}/${_E}")
    endforeach()
endfunction()

if(ALL_BRAINS)
    include("${CMAKE_CURRENT_LIST_DIR}/brain_folders.cmake")
    winbolo_brain_folders(_BRAINS "${SOURCE_DIR}")
    foreach(_B IN LISTS _BRAINS)
        file(MAKE_DIRECTORY "${DEST_DIR}/${_B}")
        _copy_brain_tree("${SOURCE_DIR}/${_B}" "${DEST_DIR}/${_B}")
    endforeach()
else()
    _copy_brain_tree("${SOURCE_DIR}" "${DEST_DIR}")
endif()

# copy_shared_libs.cmake — invoked at build time via cmake -P
# Copies all shared libraries (.dll / .so* / .dylib) from SOURCE_DIR to DEST_DIR.
# Pass -DSOURCE_DIR=... -DDEST_DIR=... on the command line.

if(WIN32)
    file(GLOB_RECURSE _LIBS "${SOURCE_DIR}/*.dll")
elseif(APPLE)
    file(GLOB_RECURSE _LIBS "${SOURCE_DIR}/*.dylib")
else()
    file(GLOB_RECURSE _LIBS "${SOURCE_DIR}/*.so" "${SOURCE_DIR}/*.so.*")
endif()

foreach(_LIB ${_LIBS})
    get_filename_component(_NAME "${_LIB}" NAME)
    execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_LIB}" "${DEST_DIR}/${_NAME}")
endforeach()

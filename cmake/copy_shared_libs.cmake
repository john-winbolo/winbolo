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
    # Skip the FetchContent build tree.  It contains crashpad's test fixtures
    # (z7_test.dll, pe_only_symbol_test.dll, with-buildid.so, ...) which are not
    # runtime dependencies.  Every real shared lib is already mirrored next to
    # the executables by the per-target POST_BUILD copy steps, so the top-level
    # build dir has everything we need.
    if(_LIB MATCHES "/_deps/")
        continue()
    endif()
    get_filename_component(_NAME "${_LIB}" NAME)
    execute_process(COMMAND ${CMAKE_COMMAND} -E copy_if_different "${_LIB}" "${DEST_DIR}/${_NAME}")
endforeach()

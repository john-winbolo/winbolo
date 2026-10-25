# cmake/bolo_luajit.cmake
#
# The LuaJIT commit every build in this tree pins, and the cross-build of
# LuaJIT the mobile clients run on: the Android client, which Gradle
# configures and builds once per ABI, and the iOS client. The desktop builds
# in the top-level CMakeLists.txt compile LuaJIT for the machine they run on
# and keep their own ExternalProject there, beside the option comments.
#
# LuaJIT is built with its own make (the top-level CMakeLists.txt says why
# the CMake wrappers are not used). A cross-build with that Makefile has two
# halves, both described under "Cross-compiling LuaJIT" in doc/install.html
# of the LuaJIT source:
#   - the host tools, host/minilua and host/buildvm, are compiled with
#     HOST_CC and run on this machine during the build. buildvm writes the
#     target's bytecode and VM tables, so it has to have the target's
#     pointer size: a 64-bit target needs a 64-bit host compiler (buildvm.c
#     stops with "pointer size mismatch in cross-build" otherwise);
#   - the library is compiled with STATIC_CC, linked with TARGET_LD and
#     archived with TARGET_AR, with TARGET_SYS naming the target OS (it
#     picks the assembler syntax, the unwind model and -ldl) and
#     TARGET_FLAGS carried into every target compile and link.
# The NDK's clang and Apple's clang both take the target as a flag, so the
# four target tools are named outright and CROSS, the Makefile's prefix for
# a GNU binutils toolchain, is left empty.

set(WINBOLO_LUAJIT_LUAJIT_TAG   "faaf663340347a78b22ed94c63c24fe090bd9784"
    CACHE STRING "Pinned LuaJIT (v2.1 branch) commit to build")

# bolo_luajit_cross_build(<target>
#     SOURCE_DIR   <dir>        the LuaJIT checkout; the Makefile builds in it
#     OUTPUT_DIR   <dir>        receives libluajit.a and the step's stamps
#     HOST_CC      <args...>    compiler for the host tools, 64-bit output
#     TARGET_CC    <args...>    compiler for the library, with its target and
#                               sysroot flags
#     TARGET_AR    <path>       archiver for the library
#     TARGET_STRIP <path>
#     TARGET_SYS   <Linux|iOS>
#     [TARGET_FLAGS <args...>]  flags for every target compile and link
#     [XCFLAGS <args...>]       LuaJIT's own switches (-DLUAJIT_DISABLE_JIT,
#                               ...). -fPIC is always on: the archive links
#                               into a shared library on Android and a
#                               position-independent app on iOS
#     [ARCHES <arch...>]        Apple: one slice per architecture, built with
#                               TARGET_FLAGS plus -arch, lipo'd together
#     LIB_VAR      <var>        set in the caller to the archive's path
# )
#
# Declares <target> as an ExternalProject whose build step runs
# cmake/luajit_cross_build.cmake: make clean, make, and a copy of
# src/libluajit.a (or the lipo of the slices) to OUTPUT_DIR. The step's
# stamp holds it off once it has run, so an incremental build does not
# rebuild LuaJIT and a fresh build directory builds it from scratch. For iOS
# the step runs on every build instead, because under the Xcode generator
# the SDK and the architectures are the destination's, known only when
# Xcode builds; the script keeps a record of what it built and returns at
# once when nothing changed (see the script's header).
function(bolo_luajit_cross_build _target)
    set(_one SOURCE_DIR OUTPUT_DIR TARGET_AR TARGET_STRIP TARGET_SYS LIB_VAR)
    set(_many HOST_CC TARGET_CC TARGET_FLAGS XCFLAGS ARCHES)
    cmake_parse_arguments(PARSE_ARGV 1 _a "" "${_one}" "${_many}")
    foreach(_need SOURCE_DIR OUTPUT_DIR HOST_CC TARGET_CC TARGET_AR
                  TARGET_STRIP TARGET_SYS LIB_VAR)
        if(NOT _a_${_need})
            message(FATAL_ERROR
                    "bolo_luajit_cross_build(${_target}): ${_need} is required")
        endif()
    endforeach()
    if(_a_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "bolo_luajit_cross_build(${_target}): unknown "
                            "arguments: ${_a_UNPARSED_ARGUMENTS}")
    endif()
    if(CMAKE_HOST_WIN32)
        message(FATAL_ERROR "bolo_luajit_cross_build(${_target}): LuaJIT's "
                            "Makefile needs a POSIX host (WSL or macOS)")
    endif()

    find_program(BOLO_LUAJIT_MAKE NAMES make gmake
                 DOC "make that runs LuaJIT's own build")
    if(NOT BOLO_LUAJIT_MAKE)
        message(FATAL_ERROR "bolo_luajit_cross_build(${_target}): no make on "
                            "this machine to build LuaJIT with")
    endif()

    include(ExternalProject)

    string(JOIN " " _host_cc      ${_a_HOST_CC})
    string(JOIN " " _target_cc    ${_a_TARGET_CC})
    string(JOIN " " _target_flags ${_a_TARGET_FLAGS})
    string(JOIN " " _xcflags      -fPIC ${_a_XCFLAGS})
    string(JOIN " " _arches       ${_a_ARCHES})
    set(_lib "${_a_OUTPUT_DIR}/libluajit.a")

    set(_always FALSE)
    if(_a_TARGET_SYS STREQUAL "iOS")
        set(_always TRUE)
    endif()

    ExternalProject_Add(${_target}
        PREFIX            "${_a_OUTPUT_DIR}"
        SOURCE_DIR        "${_a_SOURCE_DIR}"
        BUILD_IN_SOURCE   TRUE
        CONFIGURE_COMMAND ""
        BUILD_COMMAND     ${CMAKE_COMMAND}
                          "-DLJ_MAKE=${BOLO_LUAJIT_MAKE}"
                          "-DLJ_SOURCE_DIR=${_a_SOURCE_DIR}"
                          "-DLJ_OUTPUT_DIR=${_a_OUTPUT_DIR}"
                          "-DLJ_HOST_CC=${_host_cc}"
                          "-DLJ_TARGET_CC=${_target_cc}"
                          "-DLJ_TARGET_AR=${_a_TARGET_AR}"
                          "-DLJ_TARGET_STRIP=${_a_TARGET_STRIP}"
                          "-DLJ_TARGET_SYS=${_a_TARGET_SYS}"
                          "-DLJ_TARGET_FLAGS=${_target_flags}"
                          "-DLJ_XCFLAGS=${_xcflags}"
                          "-DLJ_ARCHES=${_arches}"
                          -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/luajit_cross_build.cmake"
        BUILD_ALWAYS      ${_always}
        INSTALL_COMMAND   ""
        BUILD_BYPRODUCTS  "${_lib}")
    set(${_a_LIB_VAR} "${_lib}" PARENT_SCOPE)
endfunction()

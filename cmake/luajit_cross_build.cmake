# Cross-compile LuaJIT with its own Makefile. Run with cmake -P as the build
# step of the ExternalProject that bolo_luajit_cross_build() in
# bolo_luajit.cmake declares; the inputs arrive as -D definitions:
#   LJ_MAKE          make
#   LJ_SOURCE_DIR    the LuaJIT checkout; the Makefile builds in place
#   LJ_OUTPUT_DIR    receives libluajit.a and the record of what built it
#   LJ_HOST_CC       compiler for host/minilua and host/buildvm
#   LJ_TARGET_CC     compiler for the library, with its target and sysroot
#   LJ_TARGET_AR     archiver; the Makefile's "rcus" is added here
#   LJ_TARGET_STRIP
#   LJ_TARGET_SYS    Linux or iOS
#   LJ_TARGET_FLAGS  flags for every target compile and link
#   LJ_XCFLAGS       LuaJIT's own switches
#   LJ_ARCHES        Apple only: architectures, space-separated; one slice
#                    each, lipo'd into libluajit.a. Empty: one build with
#                    LJ_TARGET_FLAGS as given
#
# For iOS under the Xcode generator the SDK, the architectures and the
# deployment target belong to the destination Xcode is building for, so
# they are known only when Xcode runs this step, which it does with them in
# the environment as SDKROOT, PLATFORM_NAME, ARCHS and
# IPHONEOS_DEPLOYMENT_TARGET. When those are present they replace
# LJ_TARGET_FLAGS and LJ_ARCHES. The record beside the archive holds the
# make arguments it was built with, so the step can run on every build and
# only rebuilds when the destination, or anything else, changed.

cmake_minimum_required(VERSION 3.22)

set(_lj_flags  "${LJ_TARGET_FLAGS}")
set(_lj_arches "${LJ_ARCHES}")

if(LJ_TARGET_SYS STREQUAL "iOS" AND DEFINED ENV{SDKROOT} AND DEFINED ENV{PLATFORM_NAME})
    set(_lj_sdk "$ENV{SDKROOT}")
    if(NOT IS_ABSOLUTE "${_lj_sdk}")
        execute_process(COMMAND xcrun --sdk "${_lj_sdk}" --show-sdk-path
                        OUTPUT_VARIABLE _lj_sdk
                        OUTPUT_STRIP_TRAILING_WHITESPACE)
    endif()
    set(_lj_flags "-isysroot ${_lj_sdk}")
    if(DEFINED ENV{IPHONEOS_DEPLOYMENT_TARGET})
        # The Makefile reads no deployment variable for iOS (only
        # MACOSX_DEPLOYMENT_TARGET, for Darwin); the floor is a compiler
        # flag, and the Simulator spells it differently.
        if("$ENV{PLATFORM_NAME}" STREQUAL "iphonesimulator")
            string(APPEND _lj_flags " -mios-simulator-version-min=$ENV{IPHONEOS_DEPLOYMENT_TARGET}")
        else()
            string(APPEND _lj_flags " -miphoneos-version-min=$ENV{IPHONEOS_DEPLOYMENT_TARGET}")
        endif()
    endif()
    if(DEFINED ENV{ARCHS})
        set(_lj_arches "$ENV{ARCHS}")
    endif()
endif()
string(REPLACE " " ";" _lj_arches "${_lj_arches}")
list(REMOVE_ITEM _lj_arches "")

# Inside Xcode's build these name the iOS SDK and floor, and Apple's clang
# follows them when a compile names no target of its own: the host tools
# would come out as iOS binaries this machine cannot run. The target
# compiles carry the SDK and the floor in TARGET_FLAGS.
unset(ENV{SDKROOT})
unset(ENV{IPHONEOS_DEPLOYMENT_TARGET})

set(_lj_make_args
    BUILDMODE=static
    "HOST_CC=${LJ_HOST_CC}"
    "STATIC_CC=${LJ_TARGET_CC}"
    "DYNAMIC_CC=${LJ_TARGET_CC} -fPIC"
    "TARGET_LD=${LJ_TARGET_CC}"
    "TARGET_AR=${LJ_TARGET_AR} rcus"
    "TARGET_STRIP=${LJ_TARGET_STRIP}"
    "TARGET_SYS=${LJ_TARGET_SYS}"
    "XCFLAGS=${LJ_XCFLAGS}")

set(_lj_lib    "${LJ_OUTPUT_DIR}/libluajit.a")
set(_lj_record "${LJ_OUTPUT_DIR}/libluajit.a.built")
string(JOIN "\n" _lj_built ${_lj_make_args}
       "TARGET_FLAGS=${_lj_flags}" "ARCHES=${_lj_arches}")

if(EXISTS "${_lj_lib}" AND EXISTS "${_lj_record}")
    file(READ "${_lj_record}" _lj_had)
    if(_lj_had STREQUAL _lj_built)
        return()
    endif()
endif()

function(_lj_run)
    execute_process(COMMAND ${ARGN}
                    WORKING_DIRECTORY "${LJ_SOURCE_DIR}"
                    RESULT_VARIABLE _rc)
    if(NOT _rc EQUAL 0)
        string(JOIN " " _cmd ${ARGN})
        message(FATAL_ERROR "LuaJIT: command failed (${_rc}): ${_cmd}")
    endif()
endfunction()

file(MAKE_DIRECTORY "${LJ_OUTPUT_DIR}")
file(REMOVE "${_lj_lib}" "${_lj_record}")
set(_lj_built_a "${LJ_SOURCE_DIR}/src/libluajit.a")

if(_lj_arches)
    set(_lj_slices "")
    foreach(_lj_arch IN LISTS _lj_arches)
        set(_lj_slice "${LJ_OUTPUT_DIR}/libluajit-${_lj_arch}.a")
        _lj_run(${LJ_MAKE} -C "${LJ_SOURCE_DIR}" ${_lj_make_args}
                "TARGET_FLAGS=${_lj_flags} -arch ${_lj_arch}" clean)
        _lj_run(${LJ_MAKE} -C "${LJ_SOURCE_DIR}" -j ${_lj_make_args}
                "TARGET_FLAGS=${_lj_flags} -arch ${_lj_arch}")
        file(COPY_FILE "${_lj_built_a}" "${_lj_slice}")
        list(APPEND _lj_slices "${_lj_slice}")
    endforeach()
    _lj_run(lipo -create ${_lj_slices} -output "${_lj_lib}")
else()
    _lj_run(${LJ_MAKE} -C "${LJ_SOURCE_DIR}" ${_lj_make_args}
            "TARGET_FLAGS=${_lj_flags}" clean)
    _lj_run(${LJ_MAKE} -C "${LJ_SOURCE_DIR}" -j ${_lj_make_args}
            "TARGET_FLAGS=${_lj_flags}")
    file(COPY_FILE "${_lj_built_a}" "${_lj_lib}")
endif()
file(WRITE "${_lj_record}" "${_lj_built}")

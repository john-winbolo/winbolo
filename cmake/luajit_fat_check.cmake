# Repair libluajit-fat.a when something has clobbered it.
#
# luajit_ext is an ExternalProject with BUILD_IN_SOURCE, so the per-arch slices
# and the lipo'd fat archive all live in the shared _deps LuaJIT source tree
# rather than in a per-target build directory. Its build step runs once and is
# then held off by luajit_ext-build in the stamp directory, so nothing ever
# re-checks the fat archive afterwards. Any other build that writes into that
# tree -- a second build dir over the same checkout, a single-arch build, an
# interrupted one -- can leave an archive that no longer covers every
# architecture, and then every universal link fails with
#
#   ld: warning: ignoring file '.../libluajit-fat.a': fat file missing arch
#       'x86_64', file has 'arm64'
#   Undefined symbols for architecture x86_64: "_luaL_newstate", ...
#
# This script runs as an ALWAYS step of luajit_ext, so it re-checks on every
# build and re-lipos from the slices already on disk when they disagree. That
# costs a couple of lipo -info calls and no rebuild.
#
# Inputs (LJ_ARCHS and LJ_SLICES are "|"-separated so they survive as single
# command-line arguments; the two are in the same order):
#   LJ_FAT        path to the fat archive
#   LJ_ARCHS      architectures it must cover
#   LJ_SLICES     per-arch slices to rebuild it from
#   LJ_STAMP_DIR  luajit_ext stamp directory, cleared when repair is impossible

cmake_minimum_required(VERSION 3.28)

string(REPLACE "|" ";" _lj_archs  "${LJ_ARCHS}")
string(REPLACE "|" ";" _lj_slices "${LJ_SLICES}")

# Architectures in an archive, from either lipo -info spelling:
#   "Architectures in the fat file: <path> are: x86_64 arm64"
#   "Non-fat file: <path> is architecture: arm64"
function(_lj_arches_of _path _out)
    execute_process(COMMAND lipo -info "${_path}"
                    RESULT_VARIABLE _rc
                    OUTPUT_VARIABLE _txt
                    ERROR_QUIET
                    OUTPUT_STRIP_TRAILING_WHITESPACE)
    if(NOT _rc EQUAL 0)
        set(${_out} "" PARENT_SCOPE)
        return()
    endif()
    string(REGEX REPLACE "^.*(are|architecture): *" "" _txt "${_txt}")
    string(STRIP "${_txt}" _txt)
    string(REPLACE " " ";" _txt "${_txt}")
    set(${_out} "${_txt}" PARENT_SCOPE)
endfunction()

set(_lj_repair FALSE)
if(NOT EXISTS "${LJ_FAT}")
    set(_lj_repair TRUE)
else()
    _lj_arches_of("${LJ_FAT}" _lj_have)
    foreach(_lj_arch IN LISTS _lj_archs)
        if(NOT _lj_arch IN_LIST _lj_have)
            set(_lj_repair TRUE)
        endif()
    endforeach()
endif()

if(NOT _lj_repair)
    return()
endif()

# Only the slices can rebuild the fat archive, so they have to be intact and
# hold the architecture they are named for.
list(LENGTH _lj_archs _lj_count)
math(EXPR _lj_last "${_lj_count} - 1")
set(_lj_bad "")
foreach(_lj_i RANGE ${_lj_last})
    list(GET _lj_archs  ${_lj_i} _lj_arch)
    list(GET _lj_slices ${_lj_i} _lj_slice)
    if(NOT EXISTS "${_lj_slice}")
        list(APPEND _lj_bad "${_lj_slice} (missing)")
    else()
        _lj_arches_of("${_lj_slice}" _lj_slice_archs)
        if(NOT _lj_arch IN_LIST _lj_slice_archs)
            list(APPEND _lj_bad
                 "${_lj_slice} (holds '${_lj_slice_archs}', expected ${_lj_arch})")
        endif()
    endif()
endforeach()

if(_lj_bad)
    # Nothing on disk can repair it. Drop the stamps that hold the build step
    # off so the next build rebuilds LuaJIT from source instead of failing the
    # same way again.
    foreach(_lj_stamp build install done)
        file(REMOVE "${LJ_STAMP_DIR}/luajit_ext-${_lj_stamp}")
    endforeach()
    string(REPLACE ";" "\n  " _lj_bad "${_lj_bad}")
    message(FATAL_ERROR
            "LuaJIT: ${LJ_FAT} does not cover ${_lj_archs} and the per-arch "
            "slices cannot rebuild it:\n  ${_lj_bad}\n"
            "Cleared the luajit_ext stamps -- re-run the build to rebuild "
            "LuaJIT from source.")
endif()

execute_process(COMMAND lipo -create ${_lj_slices} -output "${LJ_FAT}"
                RESULT_VARIABLE _lj_rc)
if(NOT _lj_rc EQUAL 0)
    message(FATAL_ERROR "LuaJIT: lipo could not rebuild ${LJ_FAT}")
endif()
message(STATUS "LuaJIT: rebuilt ${LJ_FAT} for ${_lj_archs} "
               "(it no longer covered every architecture)")

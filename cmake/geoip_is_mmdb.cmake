# Shared by cmake/fetch_geoip.cmake and cmake/copy_geoip.cmake.
#
# Sets <_out> to TRUE when the file at <_path> ends with an MMDB metadata
# section. The marker sits in the last 128 KiB of every MaxMind-format file;
# an error page saved in its place does not have it.
function(_geoip_is_mmdb _path _out)
    set(${_out} FALSE PARENT_SCOPE)
    if(NOT EXISTS "${_path}")
        return()
    endif()
    file(SIZE "${_path}" _size)
    if(_size LESS 1048576)
        return()
    endif()
    math(EXPR _offset "${_size} - 131072")
    file(READ "${_path}" _tail OFFSET ${_offset} HEX)
    # "\xAB\xCD\xEFMaxMind.com"
    string(FIND "${_tail}" "abcdef4d61784d696e642e636f6d" _pos)
    if(NOT _pos EQUAL -1)
        set(${_out} TRUE PARENT_SCOPE)
    endif()
endfunction()

# Sets <_out> to the whole number of days since the file at <_path> was
# last written.
function(_geoip_age_days _path _out)
    file(TIMESTAMP "${_path}" _mtime "%s" UTC)
    string(TIMESTAMP _now "%s" UTC)
    math(EXPR _days "(${_now} - ${_mtime}) / 86400")
    set(${_out} "${_days}" PARENT_SCOPE)
endfunction()

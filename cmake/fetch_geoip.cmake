# IPinfo Lite country database (ipinfo_lite.mmdb), downloaded at configure
# time into a per-user cache so every build tree and worktree shares one copy.
#
# The download needs an IPinfo token, read from the IPINFO_TOKEN environment
# variable. The token is never stored: it is not a cache variable, it is not
# written into the generated build files, and the download URL is never
# printed. Without a token the build carries on, using the cached copy if one
# exists and otherwise building without the database (country codes show as
# unknown). The dist target refuses to package without it.
#
# Sets WINBOLO_GEOIP_DB to the cached file's path, whether or not it exists
# yet; the copy steps check for it at build time (cmake/copy_geoip.cmake).
#
#   WINBOLO_GEOIP_CACHE_DIR    where the cached copy lives
#   WINBOLO_GEOIP_MAX_AGE_DAYS download again once the copy is this old
#   WINBOLO_GEOIP_REFRESH      download on this configure whatever its age;
#                              resets itself to OFF afterwards

if(WIN32 AND DEFINED ENV{LOCALAPPDATA})
    set(_geoip_default_dir "$ENV{LOCALAPPDATA}/winbolo")
elseif(DEFINED ENV{XDG_CACHE_HOME} AND NOT "$ENV{XDG_CACHE_HOME}" STREQUAL "")
    set(_geoip_default_dir "$ENV{XDG_CACHE_HOME}/winbolo")
elseif(DEFINED ENV{HOME})
    set(_geoip_default_dir "$ENV{HOME}/.cache/winbolo")
else()
    set(_geoip_default_dir "${CMAKE_BINARY_DIR}/geoip-cache")
endif()
file(TO_CMAKE_PATH "${_geoip_default_dir}" _geoip_default_dir)

set(WINBOLO_GEOIP_CACHE_DIR "${_geoip_default_dir}" CACHE PATH
    "Directory holding the cached IPinfo Lite database (ipinfo_lite.mmdb)")
set(WINBOLO_GEOIP_MAX_AGE_DAYS 30 CACHE STRING
    "Download the IPinfo Lite database again once the cached copy is this many days old")
option(WINBOLO_GEOIP_REFRESH
    "Download the IPinfo Lite database on the next configure, whatever its age" OFF)

set(WINBOLO_GEOIP_DB "${WINBOLO_GEOIP_CACHE_DIR}/ipinfo_lite.mmdb")

# TRUE when the file at _path ends with an MMDB metadata section. The marker
# sits in the last 128 KiB of every MaxMind-format file; an error page saved
# in its place does not have it.
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

set(_geoip_want_download FALSE)
if(WINBOLO_GEOIP_REFRESH OR NOT EXISTS "${WINBOLO_GEOIP_DB}")
    set(_geoip_want_download TRUE)
else()
    file(TIMESTAMP "${WINBOLO_GEOIP_DB}" _geoip_mtime "%s" UTC)
    string(TIMESTAMP _geoip_now "%s" UTC)
    math(EXPR _geoip_age_days "(${_geoip_now} - ${_geoip_mtime}) / 86400")
    if(_geoip_age_days GREATER_EQUAL WINBOLO_GEOIP_MAX_AGE_DAYS)
        set(_geoip_want_download TRUE)
    endif()
endif()

if(_geoip_want_download)
    if(NOT DEFINED ENV{IPINFO_TOKEN} OR "$ENV{IPINFO_TOKEN}" STREQUAL "")
        if(EXISTS "${WINBOLO_GEOIP_DB}")
            message(STATUS "GeoIP: ${WINBOLO_GEOIP_DB} is out of date, but IPINFO_TOKEN is not set; using it as it is")
        else()
            message(WARNING "GeoIP: IPINFO_TOKEN is not set and there is no cached database; "
                "building without it, so players' countries will show as unknown. "
                "Set IPINFO_TOKEN and configure again to download ${WINBOLO_GEOIP_DB}.")
        endif()
    else()
        file(MAKE_DIRECTORY "${WINBOLO_GEOIP_CACHE_DIR}")
        set(_geoip_part "${WINBOLO_GEOIP_DB}.part")
        file(REMOVE "${_geoip_part}")
        message(STATUS "GeoIP: downloading the IPinfo Lite database to ${WINBOLO_GEOIP_DB}")
        # No LOG or SHOW_PROGRESS: either could put the URL, and so the token,
        # in the output.
        file(DOWNLOAD "https://ipinfo.io/data/ipinfo_lite.mmdb?token=$ENV{IPINFO_TOKEN}"
            "${_geoip_part}"
            STATUS _geoip_status
            TLS_VERIFY ON
            INACTIVITY_TIMEOUT 60)
        list(GET _geoip_status 0 _geoip_code)
        _geoip_is_mmdb("${_geoip_part}" _geoip_ok)
        if(_geoip_code EQUAL 0 AND _geoip_ok)
            file(RENAME "${_geoip_part}" "${WINBOLO_GEOIP_DB}")
            message(STATUS "GeoIP: database downloaded")
        else()
            file(REMOVE "${_geoip_part}")
            if(_geoip_code EQUAL 0)
                set(_geoip_why "the file downloaded is not a MaxMind database")
            elseif(_geoip_code EQUAL 22)
                set(_geoip_why "IPinfo refused the request; check IPINFO_TOKEN")
            else()
                set(_geoip_why "download error ${_geoip_code}")
            endif()
            if(EXISTS "${WINBOLO_GEOIP_DB}")
                message(WARNING "GeoIP: could not refresh the database (${_geoip_why}); keeping the cached copy")
            else()
                message(WARNING "GeoIP: could not download the database (${_geoip_why}); "
                    "building without it, so players' countries will show as unknown")
            endif()
        endif()
    endif()
endif()

if(WINBOLO_GEOIP_REFRESH)
    set(WINBOLO_GEOIP_REFRESH OFF CACHE BOOL
        "Download the IPinfo Lite database on the next configure, whatever its age" FORCE)
endif()
